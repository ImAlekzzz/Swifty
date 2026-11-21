//! a plain c abi over `AudioCore` for the qt front end. the header is
//! `include/swifty_core.h`.
//!
//! swift-bridge ran the async calls on a runtime of its own. here the core owns
//! one, and an async call returns at once: its result is queued as a completion
//! and a byte is written to a socket whose other end the front end watches, so
//! results are collected on the caller's own thread and nothing calls back into
//! c++ from a tokio worker.
//!
//! strings crossing the boundary are nul-terminated utf-8. any string this file
//! hands out is owned by the caller and released with `swifty_string_free`.
//! a null error string means success.

#[cfg(unix)]
use std::{
    io::Write,
    os::{fd::AsRawFd, unix::net::UnixStream},
};
use std::{
    collections::VecDeque,
    ffi::{CStr, CString, c_char, c_int, c_void},
    future::Future,
    ptr,
    sync::{Arc, Mutex, OnceLock},
};

use tokio::runtime::{Builder, Runtime};

use crate::{AudioCore, equalizer_frequencies};

/// two workers are plenty for a music player, and keep the process from
/// starting a thread per core.
const RUNTIME_WORKER_THREADS: usize = 2;

fn runtime() -> &'static Runtime {
    static RUNTIME: OnceLock<Runtime> = OnceLock::new();
    RUNTIME.get_or_init(|| {
        Builder::new_multi_thread()
            .worker_threads(RUNTIME_WORKER_THREADS)
            .thread_name("swifty-core")
            .enable_all()
            .build()
            .expect("failed to start the audio core runtime")
    })
}

struct Completion {
    request_id: u64,
    ok: bool,
    payload: CString,
}

/// what the front end asked to be called with when a completion is queued,
/// from whichever thread queued it (`swifty_core_set_completion_callback`)
struct Waker {
    callback: extern "C" fn(*mut c_void),
    context: *mut c_void,
}

// SAFETY: the front end hands over a callback that may be called from any
// thread, with a context it keeps alive until it takes the callback back.
unsafe impl Send for Waker {}

struct CompletionQueue {
    pending: Mutex<VecDeque<Completion>>,
    /// written to whenever a completion is queued
    #[cfg(unix)]
    notifier: UnixStream,
    /// the end the front end polls. kept here so the descriptor lives exactly
    /// as long as anything can still queue a completion.
    #[cfg(unix)]
    receiver: UnixStream,
    /// called too, where there is no descriptor to poll (windows)
    waker: Mutex<Option<Waker>>,
}

impl CompletionQueue {
    fn new() -> std::io::Result<Self> {
        #[cfg(unix)]
        let (notifier, receiver) = {
            let (notifier, receiver) = UnixStream::pair()?;
            notifier.set_nonblocking(true)?;
            receiver.set_nonblocking(true)?;
            (notifier, receiver)
        };
        Ok(Self {
            pending: Mutex::new(VecDeque::new()),
            #[cfg(unix)]
            notifier,
            #[cfg(unix)]
            receiver,
            waker: Mutex::new(None),
        })
    }

    fn push(&self, request_id: u64, result: Result<String, String>) {
        let (ok, payload) = match result {
            Ok(value) => (true, value),
            Err(error) => (false, error),
        };
        let completion = Completion {
            request_id,
            ok,
            payload: c_string_lossy(payload),
        };
        if let Ok(mut pending) = self.pending.lock() {
            pending.push_back(completion);
        }
        // a full socket already has unread bytes waiting, which wakes the
        // reader just the same, so a would-block write needs no retry.
        #[cfg(unix)]
        let _ = (&self.notifier).write(&[1]);
        // held while calling, so taking the callback back waits for a call
        // under way
        if let Ok(waker) = self.waker.lock()
            && let Some(waker) = waker.as_ref()
        {
            (waker.callback)(waker.context);
        }
    }

    fn pop(&self) -> Option<Completion> {
        self.pending.lock().ok()?.pop_front()
    }
}

/// the handle the front end holds.
pub struct SwiftyCore {
    core: Arc<AudioCore>,
    completions: Arc<CompletionQueue>,
}

impl SwiftyCore {
    fn spawn<F, T>(&self, request_id: u64, operation: impl FnOnce(Arc<AudioCore>) -> F)
    where
        F: Future<Output = Result<T, String>> + Send + 'static,
        T: Into<String>,
    {
        let completions = Arc::clone(&self.completions);
        let future = operation(Arc::clone(&self.core));
        runtime().spawn(async move {
            completions.push(request_id, future.await.map(Into::into));
        });
    }
}

struct Unit;

impl From<Unit> for String {
    fn from(_: Unit) -> Self {
        String::new()
    }
}

fn c_string_lossy(value: String) -> CString {
    CString::new(value).unwrap_or_else(|error| {
        let mut bytes = error.into_vec();
        bytes.retain(|byte| *byte != 0);
        CString::new(bytes).unwrap_or_default()
    })
}

fn into_raw(value: String) -> *mut c_char {
    c_string_lossy(value).into_raw()
}

fn error_string(result: Result<(), String>) -> *mut c_char {
    match result {
        Ok(()) => ptr::null_mut(),
        Err(error) => into_raw(error),
    }
}

/// # Safety
/// `value` is null or a nul-terminated string valid for this call.
unsafe fn read_str(value: *const c_char) -> Result<String, String> {
    if value.is_null() {
        return Err("a required string was missing".to_owned());
    }
    // SAFETY: the caller guarantees a valid nul-terminated string.
    unsafe { CStr::from_ptr(value) }
        .to_str()
        .map(str::to_owned)
        .map_err(|_| "a string was not valid utf-8".to_owned())
}

/// # Safety
/// `handle` is null or a live pointer from `swifty_core_new`.
unsafe fn handle<'a>(handle: *const SwiftyCore) -> Option<&'a SwiftyCore> {
    // SAFETY: the caller guarantees the pointer is null or live.
    unsafe { handle.as_ref() }
}

const MISSING_CORE: &str = "the audio core is not available";

/// a new core, or null if its completion socket could not be created.
#[unsafe(no_mangle)]
pub extern "C" fn swifty_core_new() -> *mut SwiftyCore {
    let Ok(completions) = CompletionQueue::new() else {
        return ptr::null_mut();
    };
    Box::into_raw(Box::new(SwiftyCore {
        core: Arc::new(AudioCore::new()),
        completions: Arc::new(completions),
    }))
}

/// # Safety
/// `core` is null or a pointer from `swifty_core_new` that is not used again.
/// calls still running finish in the background and their results are dropped.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn swifty_core_free(core: *mut SwiftyCore) {
    if core.is_null() {
        return;
    }
    // SAFETY: the caller hands back ownership of a pointer from `swifty_core_new`.
    let core = unsafe { Box::from_raw(core) };
    core.core.stop();
}

/// the descriptor that becomes readable when a completion is waiting. the
/// caller drains it and then calls `swifty_core_take_completion` until it
/// returns false. it stays open until `swifty_core_free`.
///
/// # Safety
/// `core` is null or live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn swifty_core_completion_fd(core: *const SwiftyCore) -> c_int {
    #[cfg(unix)]
    {
        // SAFETY: forwarded from the caller.
        unsafe { handle(core) }
            .map(|core| core.completions.receiver.as_raw_fd())
            .unwrap_or(-1)
    }
    // no descriptor to poll here: swifty_core_set_completion_callback
    #[cfg(not(unix))]
    {
        let _ = core;
        -1
    }
}

/// `callback(context)` is called, from whichever thread finished the call,
/// each time a completion is queued; null takes it back, waiting for a call
/// under way to return. for where there is no descriptor to poll (windows).
///
/// # Safety
/// `core` is null or live; `context` stays valid until the callback is taken
/// back or `core` is freed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn swifty_core_set_completion_callback(
    core: *const SwiftyCore,
    callback: Option<extern "C" fn(*mut c_void)>,
    context: *mut c_void,
) {
    // SAFETY: forwarded from the caller.
    let Some(core) = (unsafe { handle(core) }) else {
        return;
    };
    if let Ok(mut waker) = core.completions.waker.lock() {
        *waker = callback.map(|callback| Waker { callback, context });
    }
}

/// takes the oldest finished async call. `payload` receives the call's value on
/// success (an empty string) or its error message, and must be
/// freed with `swifty_string_free`.
///
/// # Safety
/// `core` is null or live; the out pointers are valid for writes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn swifty_core_take_completion(
    core: *const SwiftyCore,
    request_id: *mut u64,
    ok: *mut bool,
    payload: *mut *mut c_char,
) -> bool {
    // SAFETY: forwarded from the caller.
    let Some(core) = (unsafe { handle(core) }) else {
        return false;
    };
    if request_id.is_null() || ok.is_null() || payload.is_null() {
        return false;
    }
    let Some(completion) = core.completions.pop() else {
        return false;
    };
    // SAFETY: the caller guarantees the out pointers are writable.
    unsafe {
        *request_id = completion.request_id;
        *ok = completion.ok;
        *payload = completion.payload.into_raw();
    }
    true
}

/// # Safety
/// `value` is null or a string returned by this library, not yet freed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn swifty_string_free(value: *mut c_char) {
    if !value.is_null() {
        // SAFETY: every string handed out came from `CString::into_raw`.
        drop(unsafe { CString::from_raw(value) });
    }
}

macro_rules! async_call {
    // `prepare` runs at the call, on the caller's thread, before the work is
    // handed to the runtime: what has to happen in the order calls are made
    ($(#[$doc:meta])* $name:ident($($arg:ident),*) => |$core:ident| prepare { $($prepare:tt)* } run $body:expr) => {
        $(#[$doc])*
        ///
        /// # Safety
        /// `core` is null or live; string arguments are null or valid.
        #[unsafe(no_mangle)]
        pub unsafe extern "C" fn $name(
            core: *const SwiftyCore,
            request_id: u64,
            $($arg: *const c_char),*
        ) {
            // SAFETY: forwarded from the caller.
            let Some(handle) = (unsafe { handle(core) }) else {
                return;
            };
            $(
                // SAFETY: forwarded from the caller.
                let $arg = match unsafe { read_str($arg) } {
                    Ok(value) => value,
                    Err(error) => {
                        handle.completions.push(request_id, Err(error));
                        return;
                    }
                };
            )*
            #[allow(unused_variables)]
            let $core = &handle.core;
            $($prepare)*
            handle.spawn(request_id, move |$core| async move { $body });
        }
    };
    ($(#[$doc:meta])* $name:ident($($arg:ident),*) => |$core:ident| $body:expr) => {
        async_call!($(#[$doc])* $name($($arg),*) => |$core| prepare {} run $body);
    };
}

async_call!(
    /// rebuilds the audio output.
    swifty_core_restart_audio_output() => |core| {
        core.restart_audio_output().await.map(|()| Unit)
    }
);

async_call!(
    /// plays a music file from anywhere on disk; replaced by a later play
    /// call, as above
    swifty_core_play_file(path) => |core| prepare {
        let ticket = core.begin_playback_request();
    } run {
        core.play_file_as(path, ticket).await.map(|()| Unit)
    }
);
/// what a music file's tags say, as json (crate::local::LocalTags), or null
/// with `*error` set. an embedded cover is written into `artwork_dir`. it
/// needs no core and may be called from any thread.
///
/// # Safety
/// `path` and `artwork_dir` are null or valid strings; `error` is null or
/// valid for a write.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn swifty_read_local_tags(
    path: *const c_char,
    artwork_dir: *const c_char,
    error: *mut *mut c_char,
) -> *mut c_char {
    let fail = |message: String| {
        if !error.is_null() {
            // SAFETY: the caller guarantees `error` is writable.
            unsafe { *error = into_raw(message) };
        }
        ptr::null_mut()
    };
    // SAFETY: forwarded from the caller.
    let path = match unsafe { read_str(path) } {
        Ok(path) => path,
        Err(message) => return fail(message),
    };
    // SAFETY: forwarded from the caller.
    let artwork_dir = (!artwork_dir.is_null())
        .then(|| unsafe { read_str(artwork_dir) })
        .transpose();
    let artwork_dir = match artwork_dir {
        Ok(directory) => directory.map(std::path::PathBuf::from),
        Err(message) => return fail(message),
    };
    match crate::local::read_tags(std::path::Path::new(&path), artwork_dir.as_deref()) {
        Ok(tags) => match serde_json::to_string(&tags) {
            Ok(json) => into_raw(json),
            Err(message) => fail(message.to_string()),
        },
        Err(message) => fail(message),
    }
}

/// the lyrics embedded in a music file's tags: lrc or plain text, an empty
/// string when it carries none, or null with `*error` set if it cannot be
/// read. it needs no core and may be called from any thread.
///
/// # Safety
/// `path` is null or a valid string; `error` is null or valid for a write.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn swifty_read_local_lyrics(path: *const c_char, error: *mut *mut c_char) -> *mut c_char {
    let fail = |message: String| {
        if !error.is_null() {
            // SAFETY: the caller guarantees `error` is writable.
            unsafe { *error = into_raw(message) };
        }
        ptr::null_mut()
    };
    // SAFETY: forwarded from the caller.
    let path = match unsafe { read_str(path) } {
        Ok(path) => path,
        Err(message) => return fail(message),
    };
    match crate::local::read_lyrics(std::path::Path::new(&path)) {
        Ok(lyrics) => into_raw(lyrics),
        Err(message) => fail(message),
    }
}

/// writes edits (crate::local::TagEdits, as json) into a music file's tags,
/// with `artwork` (`artwork_len` bytes of an image, or null) as its front
/// cover. returns null on success, or an error message to free. the file is
/// left as it was if anything fails. needs no core and may be called from
/// any thread.
///
/// # Safety
/// `path` and `edits` are null or valid strings; `artwork` is null or valid
/// for `artwork_len` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn swifty_write_local_tags(
    path: *const c_char,
    edits: *const c_char,
    artwork: *const u8,
    artwork_len: usize,
) -> *mut c_char {
    // SAFETY: forwarded from the caller.
    let (path, edits) = match unsafe { (read_str(path), read_str(edits)) } {
        (Ok(path), Ok(edits)) => (path, edits),
        (Err(message), _) | (_, Err(message)) => return into_raw(message),
    };
    let edits: crate::local::TagEdits = match serde_json::from_str(&edits) {
        Ok(edits) => edits,
        Err(message) => return into_raw(format!("the edits could not be read: {message}")),
    };
    let artwork = (!artwork.is_null() && artwork_len > 0)
        // SAFETY: the caller guarantees `artwork_len` readable bytes.
        .then(|| unsafe { std::slice::from_raw_parts(artwork, artwork_len) });
    error_string(crate::local::write_tags(
        std::path::Path::new(&path),
        &edits,
        artwork,
    ))
}

macro_rules! fallible_call {
    ($name:ident($($arg:ident: $ty:ty),*) => |$core:ident| $body:expr) => {
        /// returns null on success, or an error message to free.
        ///
        /// # Safety
        /// `core` is null or live.
        #[unsafe(no_mangle)]
        pub unsafe extern "C" fn $name(core: *const SwiftyCore $(, $arg: $ty)*) -> *mut c_char {
            // SAFETY: forwarded from the caller.
            match unsafe { handle(core) } {
                Some(handle) => {
                    let $core = &handle.core;
                    error_string($body)
                }
                None => into_raw(MISSING_CORE.to_owned()),
            }
        }
    };
}

fallible_call!(swifty_core_play() => |core| core.play());
fallible_call!(swifty_core_pause() => |core| core.pause());
fallible_call!(swifty_core_seek(position_ms: u32) => |core| core.seek(position_ms));
fallible_call!(swifty_core_set_volume(volume: f32) => |core| core.set_volume(volume));
fallible_call!(swifty_core_set_playback_shaping(tempo: f32, pitch_semitones: f32, preserves_pitch: bool)
    => |core| core.set_playback_shaping(tempo, pitch_semitones, preserves_pitch));

/// returns null on success, or an error message to free.
///
/// # Safety
/// `core` is null or live; `gains_db` points to `count` floats.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn swifty_core_set_equalizer(
    core: *const SwiftyCore,
    gains_db: *const f32,
    count: usize,
) -> *mut c_char {
    // SAFETY: forwarded from the caller.
    let Some(core) = (unsafe { handle(core) }) else {
        return into_raw(MISSING_CORE.to_owned());
    };
    if gains_db.is_null() {
        return into_raw("equalizer gains were missing".to_owned());
    }
    // SAFETY: the caller guarantees `count` readable floats.
    let gains = unsafe { std::slice::from_raw_parts(gains_db, count) }.to_vec();
    error_string(core.core.set_equalizer(gains))
}

macro_rules! query {
    ($name:ident -> $ty:ty, $fallback:expr, |$core:ident| $body:expr) => {
        /// # Safety
        /// `core` is null or live.
        #[unsafe(no_mangle)]
        pub unsafe extern "C" fn $name(core: *const SwiftyCore) -> $ty {
            // SAFETY: forwarded from the caller.
            match unsafe { handle(core) } {
                Some(handle) => {
                    let $core = &handle.core;
                    $body
                }
                None => $fallback,
            }
        }
    };
}

query!(swifty_core_audio_output_stalled -> bool, false, |core| core.audio_output_stalled());
query!(swifty_core_playback_position_ms -> u32, 0, |core| core.playback_position_ms());
query!(swifty_core_playback_duration_ms -> u32, 0, |core| core.playback_duration_ms());
query!(swifty_core_take_end_of_track -> bool, false, |core| core.take_end_of_track());
query!(swifty_core_volume -> f32, 0.75, |core| core.volume());

/// # Safety
/// `core` is null or live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn swifty_core_set_spectrum_enabled(
    core: *const SwiftyCore,
    enabled: bool,
) {
    // SAFETY: forwarded from the caller.
    if let Some(core) = unsafe { handle(core) } {
        core.core.set_spectrum_enabled(enabled);
    }
}

/// copies up to `capacity` band levels (0...1) into `out` and returns how many
/// were written. allocates nothing, so it is safe to call every frame.
///
/// # Safety
/// `core` is null or live; `out` points to `capacity` writable floats.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn swifty_core_spectrum_levels(
    core: *const SwiftyCore,
    out: *mut f32,
    capacity: usize,
) -> usize {
    // SAFETY: forwarded from the caller.
    let Some(core) = (unsafe { handle(core) }) else {
        return 0;
    };
    if out.is_null() {
        return 0;
    }
    // SAFETY: the caller guarantees `capacity` writable floats.
    let out = unsafe { std::slice::from_raw_parts_mut(out, capacity) };
    core.core.copy_spectrum_levels(out)
}

/// the equalizer's centre frequencies in hz, in band order. returns how many
/// were written.
///
/// # Safety
/// `out` points to `capacity` writable floats.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn swifty_equalizer_frequencies(out: *mut f32, capacity: usize) -> usize {
    if out.is_null() {
        return 0;
    }
    let frequencies = equalizer_frequencies();
    let count = frequencies.len().min(capacity);
    // SAFETY: the caller guarantees `capacity` writable floats.
    unsafe { std::slice::from_raw_parts_mut(out, count) }.copy_from_slice(&frequencies[..count]);
    count
}

#[cfg(test)]
mod tests {
    use std::{io::Read, time::Duration};

    use super::*;

    fn wait_for_completion(core: *const SwiftyCore) -> (u64, bool, String) {
        let fd = unsafe { swifty_core_completion_fd(core) };
        assert!(fd >= 0);
        let reader = unsafe { &(&*core).completions.receiver };
        for _ in 0..200 {
            let mut byte = [0; 16];
            let _ = (&*reader).read(&mut byte);
            let mut request_id = 0;
            let mut ok = true;
            let mut payload = ptr::null_mut();
            if unsafe {
                swifty_core_take_completion(core, &mut request_id, &mut ok, &mut payload)
            } {
                let text = unsafe { CStr::from_ptr(payload) }
                    .to_string_lossy()
                    .into_owned();
                unsafe { swifty_string_free(payload) };
                return (request_id, ok, text);
            }
            std::thread::sleep(Duration::from_millis(10));
        }
        panic!("no completion arrived");
    }

    #[test]
    fn an_async_call_for_a_missing_file_completes_with_its_error() {
        let core = swifty_core_new();
        let path = CString::new("/nowhere/song.flac").unwrap();
        unsafe { swifty_core_play_file(core, 7, path.as_ptr()) };

        let (request_id, ok, payload) = wait_for_completion(core);
        assert_eq!(request_id, 7);
        assert!(!ok);
        assert!(!payload.is_empty());
        unsafe { swifty_core_free(core) };
    }

    #[test]
    fn a_missing_argument_completes_instead_of_crashing() {
        let core = swifty_core_new();
        unsafe { swifty_core_play_file(core, 3, ptr::null()) };

        let (request_id, ok, _) = wait_for_completion(core);
        assert_eq!(request_id, 3);
        assert!(!ok);
        unsafe { swifty_core_free(core) };
    }

    #[test]
    fn sync_calls_report_errors_and_values() {
        let core = swifty_core_new();
        let error = unsafe { swifty_core_play(core) };
        assert!(!error.is_null());
        unsafe { swifty_string_free(error) };

        assert!(unsafe { swifty_core_set_volume(core, 0.4) }.is_null());
        assert!((unsafe { swifty_core_volume(core) } - 0.4).abs() < f32::EPSILON);

        let mut levels = [1.0; 64];
        let count =
            unsafe { swifty_core_spectrum_levels(core, levels.as_mut_ptr(), levels.len()) };
        assert_eq!(count, 48);
        assert!(levels[..count].iter().all(|level| *level == 0.0));

        let mut frequencies = [0.0; 8];
        let count =
            unsafe { swifty_equalizer_frequencies(frequencies.as_mut_ptr(), frequencies.len()) };
        assert_eq!(
            &frequencies[..count],
            &[60.0, 150.0, 400.0, 1000.0, 2400.0, 15000.0]
        );
        unsafe { swifty_core_free(core) };
    }

    #[test]
    fn the_header_names_the_superseded_message() {
        // the front end tells a replaced request from a failed one by it
        let header = include_str!("../include/swifty_core.h");
        let define = format!("#define SWIFTY_CORE_SUPERSEDED \"{}\"", crate::SUPERSEDED);
        assert!(header.contains(&define));
    }

    #[test]
    fn null_handles_are_harmless() {
        assert_eq!(unsafe { swifty_core_completion_fd(ptr::null()) }, -1);
        let error = unsafe { swifty_core_pause(ptr::null()) };
        assert!(!error.is_null());
        unsafe { swifty_string_free(error) };
        unsafe { swifty_core_free(ptr::null_mut()) };
    }
}
