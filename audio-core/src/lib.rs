use std::{
    io,
    path::PathBuf,
    sync::{
        Arc, Mutex,
        atomic::{AtomicBool, AtomicU64, Ordering},
    },
    time::{Duration, Instant},
};

use rodio::Source;
use tokio::sync::watch;

mod dsp;
mod file_playback;
pub mod local;
mod output;
mod spectrum;

use dsp::{EQ_BAND_COUNT, SharedDsp};
use spectrum::{OutputHealth, SharedSpectrum};

/// the rate and channel count the analysis and the equalizer work at
pub(crate) const SAMPLE_RATE: u32 = 44_100;
pub(crate) const NUM_CHANNELS: u8 = 2;


/// the output files play through. replaceable, not a oncelock: ios stops the audio unit behind
/// this stream whenever another app interrupts us, and a stopped unit never restarts
/// itself.
static LOCAL_AUDIO_SINK: Mutex<Option<Arc<output::LocalOutput>>> = Mutex::new(None);

fn stop_cached_local_sink() {
    let Ok(mut cached) = LOCAL_AUDIO_SINK.lock() else {
        return;
    };
    let Some(sink) = cached.take() else {
        return;
    };
    sink.stop();
    sink.clear();
}


#[cfg(feature = "capi")]
pub mod capi;

/// the bands the equalizer is built from, so the ui can label its own sliders
/// from the filters that actually run rather than a copy of the list.
pub fn equalizer_frequencies() -> Vec<f32> {
    dsp::EQ_FREQUENCIES.to_vec()
}

pub fn core_version() -> String {
    env!("CARGO_PKG_VERSION").to_owned()
}

struct PlayerState {
    volume: f32,
    local_playback: Option<file_playback::LocalPlayback>,
    /// the output sink files play through
    local_sink: Option<Arc<output::LocalOutput>>,
}

impl Default for PlayerState {
    fn default() -> Self {
        Self {
            volume: 0.75,
            local_playback: None,
            local_sink: None,
        }
    }
}

#[derive(Default)]
struct PlaybackTimeline {
    position_ms: u32,
    duration_ms: u32,
    is_playing: bool,
    end_of_track: bool,
    observed_at: Option<Instant>,
}

impl PlaybackTimeline {
    /// how long the timeline will keep extrapolating between position updates.
    const STALL_TIMEOUT: Duration = Duration::from_secs(6);

    fn reset(&mut self) {
        *self = Self::default();
    }

    /// true once the player has claimed to be playing for longer than it can
    /// go without reporting a position.
    fn is_stalled(&self) -> bool {
        self.is_playing
            && self
                .observed_at
                .is_some_and(|observed_at| observed_at.elapsed() >= Self::STALL_TIMEOUT)
    }

    fn observe_position(&mut self, position_ms: u32, is_playing: Option<bool>) {
        self.position_ms = position_ms;
        if let Some(is_playing) = is_playing {
            self.is_playing = is_playing;
        }
        self.observed_at = Some(Instant::now());
    }

    fn current_position_ms(&self) -> u32 {
        let elapsed_ms = if self.is_playing {
            self.observed_at
                .map(|observed_at| {
                    observed_at.elapsed().min(Self::STALL_TIMEOUT).as_millis() as u32
                })
                .unwrap_or(0)
        } else {
            0
        };
        let position_ms = self.position_ms.saturating_add(elapsed_ms);

        if self.duration_ms == 0 {
            position_ms
        } else {
            position_ms.min(self.duration_ms)
        }
    }
}

/// what a play request that a newer one replaced ends with
/// (SWIFTIFY_CORE_SUPERSEDED in the c header). never shown: the front end has
/// withdrawn it already, or asks again for the song it still wants.
pub const SUPERSEDED: &str = "the request was replaced by a newer one";

pub struct AudioCore {
    state: Mutex<PlayerState>,
    /// bumped by every request that changes what plays: each song asked for,
    /// an output rebuilt. a request keeps the number it started with, and
    /// once that is no longer the latest it gives up without touching the
    /// output: requests overlap on the runtime's workers, and one that
    /// finished switching after a newer one had played its song over the
    /// newer one
    playback_generation: watch::Sender<u64>,
    /// held while a request switches the output over, so two never
    /// interleave
    switching: Mutex<()>,
    spectrum: SharedSpectrum,
    dsp: SharedDsp,
    timeline: Arc<Mutex<PlaybackTimeline>>,
    local_playback_active: Arc<AtomicBool>,
    output_health: OutputHealth,
}

impl Default for AudioCore {
    fn default() -> Self {
        Self::new()
    }
}

impl AudioCore {
    pub fn new() -> Self {
        Self {
            state: Mutex::new(PlayerState::default()),
            playback_generation: watch::Sender::new(0),
            switching: Mutex::new(()),
            spectrum: SharedSpectrum::new(),
            dsp: SharedDsp::new(),
            timeline: Arc::new(Mutex::new(PlaybackTimeline::default())),
            local_playback_active: Arc::new(AtomicBool::new(false)),
            output_health: OutputHealth::new(),
        }
    }

    /// stops what plays and lets go of the output
    pub fn stop(&self) {
        self.begin_playback_request();
        self.clear_local_playback();
        if let Ok(mut state) = self.lock_state() {
            state.local_sink = None;
        }
        self.spectrum.set_enabled(false);
        self.reset_timeline();
    }

    /// one gain per band, in decibels, in the order equalizer_frequencies
    /// reports. applied to streaming and file playback alike.
    pub fn set_equalizer(&self, gains_db: Vec<f32>) -> Result<(), String> {
        if gains_db.len() != EQ_BAND_COUNT {
            return Err(format!(
                "the equalizer takes {EQ_BAND_COUNT} gains, not {}",
                gains_db.len()
            ));
        }
        let mut settings = self.dsp.get();
        for (band, gain) in settings.gains_db.iter_mut().zip(gains_db) {
            *band = gain;
        }
        self.dsp.set(settings);
        Ok(())
    }

    /// tempo is a duration factor: 0.5 takes twice as long. preserves_pitch
    /// off is the slowed or nightcore sound, where the recording is simply
    /// played at another rate.
    pub fn set_playback_shaping(
        &self,
        tempo: f32,
        pitch_semitones: f32,
        preserves_pitch: bool,
    ) -> Result<(), String> {
        let mut settings = self.dsp.get();
        settings.tempo = tempo;
        settings.pitch_semitones = pitch_semitones;
        settings.preserves_pitch = preserves_pitch;
        self.dsp.set(settings);
        Ok(())
    }

    /// whether the audio device has stopped taking what we hand it.
    pub fn audio_output_stalled(&self) -> bool {
        if self.output_health.is_stalled() {
            return true;
        }
        // the system said the stream is broken
        if LOCAL_AUDIO_SINK
            .lock()
            .map(|cached| cached.as_ref().is_some_and(|sink| sink.failed()))
            .unwrap_or(false)
        {
            return true;
        }
        // file playback has its own clock, driven by what the device
        // actually consumed, so a stalled local sink shows up as a position
        // that stops moving rather than as a quiet timeline.
        if self.local_playback_active.load(Ordering::Acquire) {
            return false;
        }
        self.timeline
            .lock()
            .map(|timeline| timeline.is_stalled())
            .unwrap_or(false)
    }

    /// rebuilds the audio output. whatever was playing stops.
    pub async fn restart_audio_output(&self) -> Result<(), String> {
        // whatever was loading belongs to the output being thrown away
        self.begin_playback_request();
        self.output_health.clear();
        self.clear_local_playback();
        stop_cached_local_sink();
        self.lock_state()?.local_sink = None;
        self.reset_timeline();
        Ok(())
    }

    /// the number a new request that changes what plays starts with. taken
    /// as the request is made, in the order requests are made, rather than
    /// when its future first runs, which two workers may do the other way
    /// round
    pub fn begin_playback_request(&self) -> u64 {
        let mut ticket = 0;
        self.playback_generation.send_modify(|generation| {
            *generation += 1;
            ticket = *generation;
        });
        ticket
    }

    fn is_latest_request(&self, ticket: u64) -> bool {
        *self.playback_generation.borrow() == ticket
    }

    /// starts playing nothing but the file's own sound
    pub async fn play_file(&self, path: String) -> Result<(), String> {
        let ticket = self.begin_playback_request();
        self.play_file_as(path, ticket).await
    }

    pub fn play(&self) -> Result<(), String> {
        if !self.local_active()? {
            return Err("nothing is loaded to play".to_owned());
        }
        let sink = self.resolve_live_sink()?;
        if let Ok(mut timeline) = self.timeline.lock() {
            let position = timeline.position_ms;
            timeline.observe_position(position, Some(true));
        }
        sink.play();
        Ok(())
    }

    /// pausing with nothing playing is harmless, and not an error
    pub fn pause(&self) -> Result<(), String> {
        let local = self.local_active()?;
        let sink = self.lock_state()?.local_sink.clone();
        if local && let Ok(mut timeline) = self.timeline.lock() {
            let position = timeline.position_ms;
            timeline.observe_position(position, Some(false));
        }
        if let Some(sink) = sink {
            sink.pause();
        }
        self.spectrum.clear();
        Ok(())
    }

    pub fn seek(&self, position_ms: u32) -> Result<(), String> {
        if !self.local_active()? {
            return Err("nothing is loaded to seek in".to_owned());
        }
        self.local_seek(position_ms)
    }

    pub fn playback_position_ms(&self) -> u32 {
        if let Ok(state) = self.lock_state()
            && let Some(local) = &state.local_playback
        {
            let duration = local.duration_ms.min(u32::MAX as u64) as u32;
            return local.position_ms().min(duration);
        }
        self.timeline
            .lock()
            .map(|timeline| timeline.current_position_ms())
            .unwrap_or(0)
    }

    pub fn playback_duration_ms(&self) -> u32 {
        if let Ok(state) = self.lock_state()
            && let Some(local) = &state.local_playback
        {
            return local.duration_ms.min(u32::MAX as u64) as u32;
        }
        self.timeline
            .lock()
            .map(|timeline| timeline.duration_ms)
            .unwrap_or(0)
    }

    pub fn take_end_of_track(&self) -> bool {
        self.timeline
            .lock()
            .map(|mut timeline| std::mem::take(&mut timeline.end_of_track))
            .unwrap_or(false)
    }

    pub async fn play_file_as(&self, path: String, ticket: u64) -> Result<(), String> {
        if !self.is_latest_request(ticket) {
            return Err(SUPERSEDED.to_owned());
        }
        let path = PathBuf::from(path);
        if !path.is_file() {
            return Err(format!("{} is no longer there", path.display()));
        }
        self.play_path_as(path, ticket)
    }

    fn open_decoder(
        path: &std::path::Path,
    ) -> Result<rodio::Decoder<io::BufReader<std::fs::File>>, String> {
        let file = std::fs::File::open(path)
            .map_err(|error| format!("failed to open local track: {error}"))?;
        let length = file.metadata().map(|metadata| metadata.len()).ok();
        let mut builder = rodio::Decoder::builder()
            .with_data(io::BufReader::new(file))
            .with_seekable(true);
        // the length lets an mp4 be read from its index at the end, and the
        // extension spares the probe guessing
        if let Some(length) = length {
            builder = builder.with_byte_len(length);
        }
        if let Some(extension) = path.extension().and_then(|extension| extension.to_str()) {
            builder = builder.with_hint(extension);
        }
        builder
            .build()
            .map_err(|error| format!("failed to decode local track: {error}"))
    }

    fn play_path_as(&self, path: PathBuf, ticket: u64) -> Result<(), String> {
        // opened before the outputs are touched, so a request replaced while
        // it reads the file leaves the newer one playing undisturbed
        let decoder = Self::open_decoder(&path)?;

        let _switching = self.lock_switching();
        if !self.is_latest_request(ticket) {
            return Err(SUPERSEDED.to_owned());
        }
        self.clear_local_playback();
        // a file whose header does not say how long it is (a variable rate
        // mp3 without one) is measured, so it can still be seeked
        let duration_ms = decoder
            .total_duration()
            .map(|d| d.as_millis() as u64)
            .or_else(|| local::measure_duration_ms(&path))
            .unwrap_or(0);
        let channels = decoder.channels() as u32;
        let sample_rate = decoder.sample_rate();

        let pushed_frames = Arc::new(AtomicU64::new(0));
        let cancel = Arc::new(AtomicBool::new(false));
        let source_frames = Arc::clone(&pushed_frames);
        let spectrum = self.spectrum.clone();
        let source = file_playback::LocalTrackSource::new(
            decoder,
            spectrum,
            source_frames,
            Arc::clone(&cancel),
            self.dsp.clone(),
        );

        let sink = self.resolve_live_sink()?;
        sink.stop();
        sink.clear();

        // position is derived from the count of consumed samples. rodio consumes
        // at real time while playing, so no explicit clock is required.
        sink.append(source);
        sink.play();

        {
            let mut state = self.lock_state()?;
            state.local_playback = Some(file_playback::LocalPlayback {
                path: path.clone(),
                duration_ms,
                pushed_frames,
                base_position_ms: 0,
                channels,
                sample_rate,
                cancel: Arc::clone(&cancel),
            });
        }
        self.local_playback_active.store(true, Ordering::Release);
        self.reset_timeline();
        if let Ok(mut timeline) = self.timeline.lock() {
            timeline.duration_ms = duration_ms.min(u32::MAX as u64) as u32;
            timeline.observe_position(0, Some(true));
        }

        self.spawn_local_end_watcher(cancel);
        Ok(())
    }

    pub fn set_volume(&self, volume: f32) -> Result<(), String> {
        if !volume.is_finite() {
            return Err("volume must be a finite number".to_owned());
        }
        let volume = volume.clamp(0.0, 1.0);
        let mut state = self.lock_state()?;
        // remember the desired gain even before the audio output exists.
        state.volume = volume;
        if let Some(sink) = &state.local_sink {
            sink.set_volume(volume);
        }
        Ok(())
    }

    pub fn volume(&self) -> f32 {
        self.lock_state().map(|state| state.volume).unwrap_or(0.75)
    }

    pub fn spectrum_levels(&self) -> Vec<f32> {
        self.spectrum.levels()
    }

    /// `spectrum_levels` into a caller's buffer, so a per-frame poll allocates nothing.
    pub fn copy_spectrum_levels(&self, out: &mut [f32]) -> usize {
        self.spectrum.copy_levels(out)
    }

    pub fn set_spectrum_enabled(&self, enabled: bool) {
        self.spectrum.set_enabled(enabled);
    }

    /// the switch lock guards no data, so a panic while it was held leaves
    /// nothing inconsistent behind it
    fn lock_switching(&self) -> std::sync::MutexGuard<'_, ()> {
        self.switching
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
    }

    fn lock_state(&self) -> Result<std::sync::MutexGuard<'_, PlayerState>, String> {
        self.state
            .lock()
            .map_err(|_| "the player state is unavailable".to_owned())
    }

    fn local_active(&self) -> Result<bool, String> {
        Ok(self.lock_state()?.local_playback.is_some())
    }

    fn resolve_live_sink(&self) -> Result<Arc<output::LocalOutput>, String> {
        {
            let state = self.lock_state()?;
            if let Some(sink) = &state.local_sink {
                return Ok(Arc::clone(sink));
            }
        }
        let sink = {
            let mut cached = LOCAL_AUDIO_SINK
                .lock()
                .map_err(|_| "the audio output for files is unavailable".to_owned())?;
            match cached.as_ref() {
                Some(sink) => Arc::clone(sink),
                None => {
                    let created = Arc::new(output::LocalOutput::open().ok_or_else(|| {
                        self.output_health.mark_stalled();
                        "no audio output device is available".to_owned()
                    })?);
                    *cached = Some(Arc::clone(&created));
                    created
                }
            }
        };
        let mut state = self.lock_state()?;
        sink.set_volume(state.volume);
        state.local_sink = Some(Arc::clone(&sink));
        Ok(sink)
    }

    fn clear_local_playback(&self) {
        self.local_playback_active.store(false, Ordering::Release);
        let (local, sink) = match self.lock_state() {
            Ok(mut state) => (state.local_playback.take(), state.local_sink.clone()),
            Err(_) => return,
        };
        if let Some(local) = local {
            local.cancel.store(true, Ordering::Relaxed);
        }
        if let Some(sink) = sink {
            sink.stop();
            sink.clear();
        }
    }

    fn local_seek(&self, position_ms: u32) -> Result<(), String> {
        let playback = self
            .lock_state()?
            .local_playback
            .clone()
            .ok_or_else(|| "no local playback is active".to_owned())?;
        let duration = playback.duration_ms;
        let channels = playback.channels.max(1);
        let sample_rate = playback.sample_rate.max(1);
        let target = (position_ms as u64).min(duration);

        if let Ok(mut timeline) = self.timeline.lock() {
            timeline.end_of_track = false;
        }

        let sink = self.resolve_live_sink()?;
        let was_paused = sink.is_paused();
        playback.cancel.store(true, Ordering::Relaxed);
        sink.clear();
        playback.pushed_frames.store(0, Ordering::Relaxed);

        let path = playback.path.clone();

        // re-decode and fast-forward to the target position.
        let mut decoder = Self::open_decoder(&path)?;

        if decoder.try_seek(Duration::from_millis(target)).is_err() {
            let target_samples = target * sample_rate as u64 / 1_000 * channels as u64;
            let mut skipped: u64 = 0;
            while skipped < target_samples {
                match decoder.next() {
                    Some(_) => skipped += 1,
                    None => break,
                }
            }
        }

        let pushed_frames = Arc::new(AtomicU64::new(0));
        let cancel = Arc::new(AtomicBool::new(false));
        let spectrum = self.spectrum.clone();
        let source_frames = Arc::clone(&pushed_frames);
        let source = file_playback::LocalTrackSource::new(
            decoder,
            spectrum,
            source_frames,
            Arc::clone(&cancel),
            self.dsp.clone(),
        );

        sink.append(source);
        if was_paused {
            sink.pause();
        } else {
            sink.play();
        }

        let mut state = self.lock_state()?;
        state.local_playback = Some(file_playback::LocalPlayback {
            path,
            duration_ms: duration,
            pushed_frames,
            base_position_ms: target,
            channels,
            sample_rate,
            cancel: Arc::clone(&cancel),
        });
        drop(state);
        if let Ok(mut timeline) = self.timeline.lock() {
            timeline.end_of_track = false;
            timeline.observe_position(target as u32, Some(!was_paused));
        }
        self.spawn_local_end_watcher(cancel);
        Ok(())
    }

    fn spawn_local_end_watcher(&self, cancel: Arc<AtomicBool>) {
        let sink = match self.resolve_live_sink() {
            Ok(sink) => sink,
            Err(_) => return,
        };
        let timeline = Arc::clone(&self.timeline);
        std::thread::Builder::new()
            .name("swiftify-local-end".to_owned())
            .spawn(move || {
                while !cancel.load(Ordering::Relaxed) && !sink.empty() {
                    std::thread::sleep(Duration::from_millis(200));
                }
                if cancel.load(Ordering::Relaxed) {
                    return;
                }
                // a short grace period ensures any trailing source has been flushed.
                std::thread::sleep(Duration::from_millis(100));
                if cancel.load(Ordering::Relaxed) {
                    return;
                }
                if let Ok(mut timeline) = timeline.lock() {
                    timeline.end_of_track = true;
                    timeline.is_playing = false;
                    timeline.observed_at = Some(Instant::now());
                }
                // the device stops until something else plays
                sink.pause_if_ended();
            })
            .ok();
    }

    fn reset_timeline(&self) {
        if let Ok(mut timeline) = self.timeline.lock() {
            timeline.reset();
        }
    }

}

#[cfg(test)]
mod tests {
    use std::time::{Duration, Instant};

    use super::{AudioCore, PlaybackTimeline, SUPERSEDED};

    #[test]
    fn a_quiet_player_stops_inventing_progress_and_reports_the_stall() {
        let mut timeline = PlaybackTimeline {
            duration_ms: 300_000,
            ..PlaybackTimeline::default()
        };
        timeline.observe_position(10_000, Some(true));
        assert!(!timeline.is_stalled());

        // a position update every second keeps it honest.
        timeline.observed_at = Instant::now().checked_sub(Duration::from_secs(1));
        assert!(!timeline.is_stalled());
        assert!(timeline.current_position_ms() >= 11_000);

        // nothing for far longer than the update interval: the clock must
        // not run on without it.
        timeline.observed_at = Instant::now().checked_sub(Duration::from_secs(45));
        assert!(timeline.is_stalled());
        assert_eq!(
            timeline.current_position_ms(),
            10_000 + PlaybackTimeline::STALL_TIMEOUT.as_millis() as u32
        );
    }

    #[test]
    fn a_paused_player_is_never_reported_as_stalled() {
        let mut timeline = PlaybackTimeline::default();
        timeline.observe_position(10_000, Some(false));
        timeline.observed_at = Instant::now().checked_sub(Duration::from_secs(600));

        assert!(!timeline.is_stalled());
        assert_eq!(timeline.current_position_ms(), 10_000);
    }

    #[test]
    fn a_fresh_core_has_a_healthy_output() {
        let core = AudioCore::new();
        assert!(!core.audio_output_stalled());
    }

    #[test]
    fn volume_is_remembered_before_playing_and_after_stopping() {
        let core = AudioCore::new();
        core.set_volume(0.08).unwrap();
        assert_eq!(core.volume(), 0.08);
        core.stop();
        assert_eq!(core.volume(), 0.08);
        core.set_volume(0.0).unwrap();
        assert_eq!(core.volume(), 0.0);
        core.set_volume(2.0).unwrap();
        assert_eq!(core.volume(), 1.0);
        assert!(core.set_volume(f32::NAN).is_err());
        assert!(core.set_volume(f32::INFINITY).is_err());
        assert_eq!(core.volume(), 1.0);
    }

    #[test]
    fn transport_commands_need_a_loaded_file() {
        let core = AudioCore::new();

        assert_eq!(core.play().unwrap_err(), "nothing is loaded to play");
        assert_eq!(core.seek(1_000).unwrap_err(), "nothing is loaded to seek in");
        // pausing nothing is harmless
        assert!(core.pause().is_ok());
    }

    #[tokio::test]
    async fn a_file_that_is_not_there_is_reported() {
        let core = AudioCore::new();

        let error = core
            .play_file("/nowhere/song.flac".to_owned())
            .await
            .unwrap_err();
        assert!(error.contains("no longer there"), "{error}");
    }

    #[tokio::test]
    async fn a_replaced_request_gives_up_before_touching_the_output() {
        // two songs chosen in quick succession: the first one's request is
        // run after the second's was made, as two workers may run them
        let core = AudioCore::new();
        let first = core.begin_playback_request();
        let second = core.begin_playback_request();
        assert!(second > first);

        assert_eq!(
            core.play_file_as("/nowhere/song.flac".to_owned(), first)
                .await
                .unwrap_err(),
            SUPERSEDED
        );
        // the newer one is still the one that counts
        assert!(
            core.play_file_as("/nowhere/song.flac".to_owned(), second)
                .await
                .unwrap_err()
                .contains("no longer there")
        );
    }

    #[test]
    fn rebuilding_the_output_replaces_any_request() {
        let core = AudioCore::new();
        let request = core.begin_playback_request();
        core.stop();
        assert!(!core.is_latest_request(request));
    }
}
