/*
 * the c interface to the audio core, built with `--features capi`.
 *
 * strings are nul-terminated utf-8. a string returned from here is owned by the
 * caller and released with swifty_string_free. functions returning an error
 * string return null on success.
 *
 * async calls return immediately. their result is queued as a completion and
 * the descriptor from swifty_core_completion_fd becomes readable: drain it,
 * then call swifty_core_take_completion until it returns false.
 */
#ifndef SWIFTY_CORE_H
#define SWIFTY_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SwiftyCore SwiftyCore;

SwiftyCore *swifty_core_new(void);
void swifty_core_free(SwiftyCore *core);
void swifty_string_free(char *value);

int swifty_core_completion_fd(const SwiftyCore *core);
/* where there is no descriptor (windows: it is -1), callback(context) is
 * called from the finishing thread instead each time a completion is queued;
 * null takes it back. context lives until then. */
void swifty_core_set_completion_callback(const SwiftyCore *core, void (*callback)(void *),
                                           void *context);
bool swifty_core_take_completion(const SwiftyCore *core, uint64_t *request_id, bool *ok,
                                   char **payload);

/* async: each completes with `request_id`, with an empty payload on success.
 *
 * a play call made while an earlier one is still running replaces it, as does
 * an output restart: the earlier one then fails with
 * SWIFTY_CORE_SUPERSEDED, having started no sound. */
#define SWIFTY_CORE_SUPERSEDED "the request was replaced by a newer one"

void swifty_core_restart_audio_output(const SwiftyCore *core, uint64_t request_id);
/* plays a music file from anywhere on disk: mp3, flac,
 * ogg vorbis, wav, aiff, or aac or apple lossless in an m4a. */
void swifty_core_play_file(const SwiftyCore *core, uint64_t request_id, const char *path);

/* what a music file's tags say, as json: title, artist, album, albumArtist,
 * trackNumber, discNumber, date, genre (each null when the file does not
 * say), durationMs and artworkPath (an embedded cover written into
 * artwork_dir under a name taken from its contents, or a cover image kept
 * beside the file, or null). null with *error set if it cannot be read.
 * needs no core and may be called from any thread. */
char *swifty_read_local_tags(const char *path, const char *artwork_dir, char **error);
/* the lyrics a music file carries in its own tags, lrc or plain text, an empty
 * string when it has none. null with *error set if the file cannot be read.
 * needs no core and may be called from any thread. */
char *swifty_read_local_lyrics(const char *path, char **error);
/* writes edits into a music file's tags: json with any of title, artist,
 * album, albumArtist, genre, date, trackNumber, discNumber (each set, or
 * cleared when empty; one left out is kept) and removeArtwork, with
 * `artwork` (artwork_len bytes of a jpeg or png, or null) as the front
 * cover. returns null, or an error to free; the file is left as it was if
 * anything fails. needs no core and may be called from any thread. */
char *swifty_write_local_tags(const char *path, const char *edits, const uint8_t *artwork,
                                size_t artwork_len);

char *swifty_core_play(const SwiftyCore *core);
char *swifty_core_pause(const SwiftyCore *core);
char *swifty_core_seek(const SwiftyCore *core, uint32_t position_ms);
char *swifty_core_set_volume(const SwiftyCore *core, float volume);
char *swifty_core_set_playback_shaping(const SwiftyCore *core, float tempo,
                                         float pitch_semitones, bool preserves_pitch);
char *swifty_core_set_equalizer(const SwiftyCore *core, const float *gains_db, size_t count);

bool swifty_core_audio_output_stalled(const SwiftyCore *core);
uint32_t swifty_core_playback_position_ms(const SwiftyCore *core);
uint32_t swifty_core_playback_duration_ms(const SwiftyCore *core);
bool swifty_core_take_end_of_track(const SwiftyCore *core);
float swifty_core_volume(const SwiftyCore *core);

void swifty_core_set_spectrum_enabled(const SwiftyCore *core, bool enabled);
/* 48 bands in 0...1. allocates nothing; returns how many were written. */
size_t swifty_core_spectrum_levels(const SwiftyCore *core, float *out, size_t capacity);

/* the equalizer's centre frequencies in hz; returns how many were written. */
size_t swifty_equalizer_frequencies(float *out, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif
