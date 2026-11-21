/*
 * Swiftify, a spotify player for desktop
 * Copyright (C) 2026  shred0s
 * Copyright (C) 2026  ImAlekz (modified for Swifty)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "playback/PlaybackController.h"

#include "core/Diagnostics.h"

#include "playback/LyricsFetcher.h"

#include "app/ErrorCenter.h"
#include "app/Settings.h"
#include "core/PlaybackCore.h"

#include <QLoggingCategory>
#include <QQmlEngine>
#include <QRandomGenerator>

#include <QCoroTimer>

#include <algorithm>
#include <chrono>
#include <QDateTime>
#include <QTimer>

#include <cmath>
#include <cstdio>
#include <iterator>
#include <utility>

namespace swifty {

Q_LOGGING_CATEGORY(lcPlayer, "swifty.player")

namespace {

PlaybackController *s_instance = nullptr;

const QString kCatchphrase = QStringLiteral("i mean i guess bro");
const QString kVolumeKey = QStringLiteral("playerVolume");
const QString kVolumeBeforeMuteKey = QStringLiteral("playerVolumeBeforeMute");

/// a restart is offered after this much of a song has played
constexpr quint32 kRestartThresholdMs = 3'000;

} // namespace

PlaybackController::PlaybackController(PlaybackCore *core, Settings *settings, ErrorCenter *errors, QObject *parent)
    : QObject(parent)
    , m_core(core)
    , m_settings(settings)
    , m_errors(errors)
    , m_upNext(new SongListModel(this))
    , m_previouslyPlayed(new SongListModel(this))
    , m_lyricsModel(new LyricsModel(this))
{
    if (!s_instance) {
        s_instance = this;
    }

    m_pollTimer.setInterval(250);
    connect(&m_pollTimer, &QTimer::timeout, this, &PlaybackController::pollPlayback);
    m_sleepTimer.setInterval(250);
    connect(&m_sleepTimer, &QTimer::timeout, this, &PlaybackController::sleepTimerTick);

    const double storedVolume = std::clamp(m_settings->value(kVolumeKey, 0.75).toDouble(), 0.0, 1.0);
    m_volume = storedVolume;
    m_volumeBeforeMute = std::clamp(
        m_settings->value(kVolumeBeforeMuteKey, storedVolume > 0.01 ? storedVolume : 0.75).toDouble(), 0.01, 1.0);
    try {
        m_core->setVolume(float(m_volume));
    } catch (const PlaybackError &) {
    }

    m_visualizersEnabled = m_settings->visualizersEnabled();
    connect(m_settings, &Settings::visualizersEnabledChanged, this, [this] {
        m_visualizersEnabled = m_settings->visualizersEnabled();
        updatePlaybackWorkers();
    });
}

PlaybackController::~PlaybackController()
{
    m_playbackRequest.cancel();
    m_lyricsRequest.cancel();
    if (s_instance == this) {
        s_instance = nullptr;
    }
}

PlaybackController *PlaybackController::instance()
{
    return s_instance;
}

PlaybackController *PlaybackController::create(QQmlEngine *, QJSEngine *)
{
    QJSEngine::setObjectOwnership(s_instance, QJSEngine::CppOwnership);
    return s_instance;
}

void PlaybackController::reportError(const QString &context, const QString &message)
{
    qCWarning(lcPlayer).noquote() << context << ':' << message;
    m_errors->show(message);
}

// --- state ---------------------------------------------------------------------------

void PlaybackController::setCurrentSong(const std::optional<Song> &song)
{
    const bool changed = song.has_value() != m_currentSong.has_value()
        || (song && m_currentSong && !song->isSameInstance(*m_currentSong));
    m_currentSong = song;
    if (changed) {
        emit currentSongChanged();
    }
}

void PlaybackController::setPlaying(bool playing)
{
    if (m_playing == playing) {
        return;
    }
    m_playing = playing;
    emit playingChanged();
    updatePlaybackWorkers();
}

void PlaybackController::setLoading(bool loading)
{
    if (m_loading != loading) {
        m_loading = loading;
        emit loadingChanged();
    }
}

void PlaybackController::setLivePosition(quint32 positionMs)
{
    const bool couldRestart = m_livePositionMs > kRestartThresholdMs;
    const bool liveChanged = m_livePositionMs != positionMs;
    m_livePositionMs = positionMs;
    if (liveChanged) {
        emit livePositionChanged();
    }
    // the clock and the bar only show whole seconds' worth of change. going
    // backwards is a seek or a new song, and always shows at once.
    if (positionMs / 1000 != m_publishedPositionMs / 1000 || positionMs < m_publishedPositionMs) {
        m_publishedPositionMs = positionMs;
        emit positionChanged();
    }
    if (couldRestart != (positionMs > kRestartThresholdMs)) {
        emit queueChanged();
    }
    updateActiveLyricLine();
}

void PlaybackController::setDuration(quint32 durationMs)
{
    if (m_durationMs != durationMs) {
        m_durationMs = durationMs;
        emit durationChanged();
    }
}

// --- queue ---------------------------------------------------------------------------

const QList<Song> &PlaybackController::activeQueue() const
{
    return m_shuffle && !m_shuffledQueue.isEmpty() ? m_shuffledQueue : m_queue;
}

int PlaybackController::currentQueueIndex() const
{
    if (!m_currentSong) {
        return -1;
    }
    const QList<Song> &queue = activeQueue();
    for (qsizetype index = 0; index < queue.size(); ++index) {
        if (queue.at(index).isSameInstance(*m_currentSong)) {
            return int(index);
        }
    }
    return -1;
}

QList<Song> PlaybackController::upNextSongs() const
{
    const QList<Song> &queue = activeQueue();
    const int index = currentQueueIndex();
    if (index < 0) {
        return queue;
    }
    return queue.mid(index + 1);
}

QList<Song> PlaybackController::previouslyPlayedSongs() const
{
    // shuffle keeps its own history, because the queue order no longer describes it
    if (m_shuffle) {
        return m_history;
    }
    const int index = currentQueueIndex();
    return index < 0 ? QList<Song> {} : activeQueue().mid(0, index);
}

std::optional<Song> PlaybackController::nextQueueSong(bool allowWrap) const
{
    const QList<Song> &queue = activeQueue();
    if (queue.isEmpty()) {
        return std::nullopt;
    }
    const int index = currentQueueIndex();
    if (!m_currentSong || index < 0) {
        return queue.first();
    }
    if (index < queue.size() - 1) {
        return queue.at(index + 1);
    }
    return allowWrap ? std::optional(queue.first()) : std::nullopt;
}

std::optional<Song> PlaybackController::upcomingSong() const
{
    return nextQueueSong(m_repeatMode == RepeatMode::All);
}

std::optional<Song> PlaybackController::precedingSong() const
{
    if (m_shuffle) {
        return m_history.isEmpty() ? std::nullopt : std::optional(m_history.last());
    }
    const QList<Song> &queue = activeQueue();
    const int index = currentQueueIndex();
    if (index < 0) {
        return std::nullopt;
    }
    if (index == 0) {
        return m_repeatMode == RepeatMode::All && !queue.isEmpty() ? std::optional(queue.last()) : std::nullopt;
    }
    return queue.at(index - 1);
}

bool PlaybackController::canSkipBackward() const
{
    if (!m_currentSong) {
        return false;
    }
    if (m_livePositionMs > kRestartThresholdMs) {
        return true;
    }
    if (m_shuffle) {
        return !m_history.isEmpty();
    }
    const int index = currentQueueIndex();
    return index >= 0 && (index > 0 || m_repeatMode == RepeatMode::All);
}

bool PlaybackController::canSkipForward() const
{
    const QList<Song> &queue = activeQueue();
    const int index = currentQueueIndex();
    if (!m_currentSong || queue.isEmpty() || index < 0) {
        return false;
    }
    return index < queue.size() - 1 || m_repeatMode == RepeatMode::All;
}

void PlaybackController::rebuildShuffledQueue(const std::optional<Song> &startingWith)
{
    QList<Song> remaining = m_queue;
    std::shuffle(remaining.begin(), remaining.end(), *QRandomGenerator::global());
    if (startingWith) {
        const auto found = std::find_if(remaining.begin(), remaining.end(),
                                         [&](const Song &song) { return song.isSameInstance(*startingWith); });
        if (found != remaining.end()) {
            const Song first = *found;
            remaining.erase(found);
            remaining.prepend(first);
        }
    }
    m_shuffledQueue = remaining;
}

void PlaybackController::refreshSongDetails(const QList<Song> &fresh)
{
    QHash<QString, const Song *> byUri;
    for (const Song &song : fresh) {
        byUri.insert(song.uri, &song);
    }
    const auto update = [&](Song &song) {
        const Song *found = song.isLocalFile() ? byUri.value(song.uri) : nullptr;
        if (!found
            || (found->name == song.name && found->artists == song.artists && found->albumName == song.albumName
                && found->artworkUrl == song.artworkUrl && found->durationMs == song.durationMs)) {
            return false;
        }
        const quint64 instance = song.instanceId;
        song = *found;
        song.instanceId = instance;
        return true;
    };
    bool queueChanged = false;
    for (QList<Song> *songs : {&m_queue, &m_shuffledQueue, &m_history}) {
        for (Song &song : *songs) {
            queueChanged = update(song) || queueChanged;
        }
    }
    if (queueChanged) {
        queueDidChange();
    }
    if (m_currentSong && update(*m_currentSong)) {
        emit currentSongChanged();
        syncMediaSession();
    }
}

void PlaybackController::queueDidChange()
{
    m_upNext->setSongs(upNextSongs());
    m_previouslyPlayed->setSongs(previouslyPlayedSongs());
    emit queueChanged();
}

void PlaybackController::syncMediaSession()
{
    emit mediaStateChanged();
}

// --- starting songs -------------------------------------------------------------------------

void PlaybackController::play(const Song &song, const QList<Song> &queue, bool fromSearch)
{
    beginPlayback(song, queue, fromSearch);
}

void PlaybackController::playFromModel(SongListModel *model, int index, bool fromSearch)
{
    if (!model || index < 0 || index >= model->count()) {
        return;
    }
    beginPlayback(model->songs().at(index), model->songs(), fromSearch);
}

void PlaybackController::shufflePlay(SongListModel *model)
{
    if (!model || model->count() == 0) {
        return;
    }
    if (!m_shuffle) {
        m_shuffle = true;
        emit shuffleChanged();
    }
    const int index = int(QRandomGenerator::global()->bounded(model->count()));
    beginPlayback(model->songs().at(index), model->songs(), false);
}

void PlaybackController::playSong(const Song &song)
{
    beginPlayback(song, {song}, false);
}

void PlaybackController::playLoadingTest()
{
    Song song;
    song.name = QStringLiteral("Loading Test");
    song.artists = QStringLiteral("Swifty");
    song.albumName = QStringLiteral("Debugging");
    song.uri = QString::fromLatin1(kLoadingTestUri);
    song.durationMs = 180'000;
    beginPlayback(song, {song}, false);
}

void PlaybackController::beginPlayback(const Song &song, const QList<Song> &queue, bool fromSearch)
{
    const bool inQueue = std::any_of(queue.begin(), queue.end(), [&](const Song &item) { return item.isSameInstance(song); });
    m_queue = inQueue ? queue : QList<Song> {song};
    m_history.clear();
    if (m_shuffle) {
        rebuildShuffledQueue(song);
    } else {
        m_shuffledQueue.clear();
    }
    startPlayback(song, true, fromSearch);
}

void PlaybackController::playNext(const Song &song)
{
    if (!m_currentSong || currentQueueIndex() < 0) {
        beginPlayback(song, {song}, false);
        return;
    }
    const Song current = *m_currentSong;
    if (song.isSameInstance(current)) {
        return;
    }

    m_queue.removeIf([&](const Song &item) { return item.uri == song.uri && !item.isSameInstance(current); });
    const auto at = std::find_if(m_queue.begin(), m_queue.end(), [&](const Song &item) { return item.isSameInstance(current); });
    if (at == m_queue.end()) {
        return;
    }
    m_queue.insert(std::distance(m_queue.begin(), at) + 1, song);

    if (m_shuffle) {
        m_shuffledQueue.removeIf([&](const Song &item) { return item.isSameInstance(song); });
        const auto shuffledAt = std::find_if(m_shuffledQueue.begin(), m_shuffledQueue.end(),
                                             [&](const Song &item) { return item.isSameInstance(current); });
        if (shuffledAt != m_shuffledQueue.end()) {
            m_shuffledQueue.insert(std::distance(m_shuffledQueue.begin(), shuffledAt) + 1, song);
        } else {
            rebuildShuffledQueue(current);
        }
    }
    queueDidChange();
    syncMediaSession();
}

void PlaybackController::playLast(const Song &song)
{
    if (!m_currentSong) {
        beginPlayback(song, {song}, false);
        return;
    }
    const Song current = *m_currentSong;
    if (song.isSameInstance(current)) {
        return;
    }
    m_queue.removeIf([&](const Song &item) { return item.uri == song.uri && !item.isSameInstance(current); });
    m_queue.append(song);
    if (m_shuffle) {
        m_shuffledQueue.removeIf([&](const Song &item) { return item.isSameInstance(song); });
        m_shuffledQueue.append(song);
    }
    queueDidChange();
    syncMediaSession();
}

void PlaybackController::playFromQueue(const Song &song)
{
    const auto matches = [&](const Song &item) { return item.isSameInstance(song); };
    const QList<Song> &queue = activeQueue();
    const bool known = std::any_of(queue.begin(), queue.end(), matches)
        || std::any_of(m_history.begin(), m_history.end(), matches);
    if (!known) {
        return;
    }
    // jumping back to a played song takes it out of the history it came from,
    // so stepping back afterwards does not land on it twice.
    const auto inHistory = std::find_if(m_history.begin(), m_history.end(), matches);
    if (inHistory != m_history.end()) {
        m_history.erase(inHistory, m_history.end());
    }
    startPlayback(song);
}

void PlaybackController::restoreSession(const Song &current, const QList<Song> &queue, quint32 positionMs)
{
    if (m_currentSong || !current.isValid()) {
        return;
    }
    const bool inQueue = std::any_of(queue.begin(), queue.end(), [&](const Song &item) { return item.isSameInstance(current); });
    m_queue = inQueue ? queue : QList<Song> {current};
    m_history.clear();
    if (m_shuffle) {
        rebuildShuffledQueue(current);
    } else {
        m_shuffledQueue.clear();
    }
    // play loads it, and then goes to where it was
    m_restoreUri = current.uri;
    m_restoreMs = positionMs;
    m_playbackNeedsLoad = true;
    setCurrentSong(current);
    setPlaying(false);
    setLoading(false);
    setLivePosition(positionMs);
    setDuration(quint32(std::max(current.durationMs, 0)));
    loadLyrics(current);
    queueDidChange();
    syncMediaSession();
}

void PlaybackController::startPlayback(const Song &song, bool recordHistory, bool fromSearch)
{
    // a place saved for one song is not for another
    if (song.uri != m_restoreUri) {
        m_restoreMs = 0;
    }
    if (recordHistory && m_currentSong && !m_currentSong->isSameInstance(song)) {
        m_history.append(*m_currentSong);
    }

    const CancelToken token = m_playbackRequest.renew();
    m_playbackNeedsLoad = true;
    setCurrentSong(song);
    setPlaying(false);
    setLoading(true);
    setLivePosition(0);
    setDuration(0);
    loadLyrics(song);
    queueDidChange();
    syncMediaSession();

    detach(runStartPlayback(song, token, fromSearch));
}

QCoro::Task<> PlaybackController::runStartPlayback(Song song, CancelToken token, bool fromSearch)
{
    QString failure;
    bool cancelled = false;
    try {
        m_errors->clear();
        if (song.uri == QLatin1StringView(kLoadingTestUri)) {
            // the debugging song: loading, and nothing else, until withdrawn.
            // what played before stops, as a real song taking over would
            m_core->pause();
            while (!isCancelled(token) && isCurrent(song)) {
                co_await QCoro::sleepFor(std::chrono::milliseconds(250));
            }
            throw Cancelled();
        }
        // the tap goes on before the core starts filling its output queue, so
        // the first spectrum frame lines up with what reaches the speakers.
        m_core->setSpectrumEnabled(m_visualizersEnabled && m_presentationActive);

        if (m_audioOutputNeedsRestart || m_core->audioOutputStalled()) {
            // whatever took the audio device left our output inert. a new song
            // would load onto it and play silently.
            co_await m_core->restartAudioOutput();
            throwIfCancelled(token);
            m_audioOutputNeedsRestart = false;
        }

        // the core gives up on a request that something newer replaced:
        // another song, or the audio output being rebuilt. this song is still
        // the one wanted, unless the request was withdrawn
        bool retry = false;
        try {
            co_await m_core->playFile(song.localPath());
        } catch (const PlaybackError &error) {
            if (error.kind() != PlaybackError::Kind::Superseded) {
                throw;
            }
            retry = true;
        }
        if (retry) {
            throwIfCancelled(token);
            co_await m_core->playFile(song.localPath());
        }
    } catch (const Cancelled &) {
        // the request was withdrawn.
        cancelled = true;
    } catch (const PlaybackError &error) {
        failure = error.message();
    }

    const bool withdrawn = cancelled || isCancelled(token) || !isCurrent(song);
    if (withdrawn) {
        // the core started the song anyway, because nobody told it the request
        // had been withdrawn.
        silenceWithdrawnPlayback();
    } else if (!failure.isNull()) {
        // a load that timed out here is still running in the core, and would
        // start the song whenever it finally arrived.
        try {
            m_core->pause();
        } catch (const PlaybackError &) {
        }
        m_core->setSpectrumEnabled(false);
        setLoading(false);
        setPlaying(false);
        syncMediaSession();
        reportError(QStringLiteral("Playing %1").arg(song.uri), failure);
    } else {
        m_playbackNeedsLoad = false;
        setLoading(false);
        if (m_restoreMs > 0 && m_restoreUri == song.uri) {
            // the song was left part way: it starts from there, with no sound
            // from the beginning in between
            const quint32 target = std::exchange(m_restoreMs, 0);
            try {
                m_core->pause();
                m_core->seek(target);
                setLivePosition(target);
                m_core->play();
            } catch (const PlaybackError &) {
            }
        }
        setPlaying(true);
        if (fromSearch) {
            emit playedFromSearch(song);
        }
        syncMediaSession();
    }
    m_playbackRequest.finish(token);
}

void PlaybackController::togglePlayback()
{
    if (!m_currentSong) {
        return;
    }
    if (m_playing) {
        pause();
    } else {
        resume();
    }
}

void PlaybackController::pause()
{
    // a play request may still be loading. withdrawing it stops its
    // completion from undoing this pause.
    cancelPlaybackRequest();
    setLoading(false);
    if (m_currentSong) {
    }
    setPlaying(false);
    syncMediaSession();
    try {
        m_core->pause();
    } catch (const PlaybackError &error) {
        qCInfo(lcPlayer).noquote() << "pausing playback:" << error.message();
    }
}

void PlaybackController::resume()
{
    if (!m_currentSong) {
        return;
    }
    // pause may have withdrawn the load before this song reached the core, or
    // the song has finished and there is nothing left to resume.
    if (m_playbackNeedsLoad) {
        startPlayback(*m_currentSong, false);
        return;
    }
    detach(runResume(m_playbackRequest.renew()));
}

QCoro::Task<> PlaybackController::runResume(CancelToken token)
{
    if (m_audioOutputNeedsRestart || m_core->audioOutputStalled()) {
        m_audioOutputNeedsRestart = true;
        co_await restartAudioOutput(token);
        m_playbackRequest.finish(token);
        co_return;
    }
    try {
        m_core->play();
        setPlaying(true);
        syncMediaSession();
    } catch (const PlaybackError &error) {
        if (!isCancelled(token)) {
            reportError(QStringLiteral("Resuming playback"), error.message());
        }
    }
    m_playbackRequest.finish(token);
}

void PlaybackController::cancelPlaybackRequest()
{
    m_playbackRequest.cancel();
}

void PlaybackController::silenceWithdrawnPlayback()
{
    if (m_playing || m_playbackRequest.isRunning()) {
        return;
    }
    try {
        m_core->pause();
    } catch (const PlaybackError &) {
    }
}

void PlaybackController::invalidateAudioOutput()
{
    cancelPlaybackRequest();
    m_audioOutputNeedsRestart = true;
    if (!m_playing) {
        return;
    }
    setPlaying(false);
    // the core is still convinced it is playing, and its own clock would keep
    // running. stop it, so a later resume starts from a known state.
    try {
        m_core->pause();
    } catch (const PlaybackError &) {
    }
    syncMediaSession();
}

void PlaybackController::prepareForSleep()
{
    // a machine that sleeps mid-song would wake up playing it. stop before it goes.
    if (m_playing) {
        pause();
    }
}

void PlaybackController::refreshAudioOutput()
{
    // a switch often comes as several changes at once: one rebuild, after them
    if (m_outputRefreshPending) {
        return;
    }
    m_outputRefreshPending = true;
    QTimer::singleShot(700, this, [this] {
        m_outputRefreshPending = false;
        if (!m_playing || !m_currentSong || m_playbackNeedsLoad) {
            // nothing is sounding; the next play opens the output anew if it needs to
            m_audioOutputNeedsRestart = true;
            return;
        }
        diagnostic("playback", QStringLiteral("the sound output changed under the song, so it is opened anew"));
        m_audioOutputNeedsRestart = true;
        detach(runResume(m_playbackRequest.renew()));
    });
}

QCoro::Task<> PlaybackController::restartAudioOutput(CancelToken token)
{
    if (!m_currentSong) {
        m_audioOutputNeedsRestart = false;
        co_return;
    }
    const Song song = *m_currentSong;
    const quint32 position = m_livePositionMs;
    QString failure;
    try {
        co_await m_core->restartAudioOutput();
        throwIfCancelled(token);
        m_audioOutputNeedsRestart = false;

        co_await m_core->playFile(song.localPath());
        throwIfCancelled(token);
        if (!isCurrent(song)) {
            co_return;
        }
        if (position > 0) {
            m_core->seek(position);
        }
        m_playbackNeedsLoad = false;
        setPlaying(true);
        m_errors->clear();
        syncMediaSession();
        co_return;
    } catch (const Cancelled &) {
        silenceWithdrawnPlayback();
        co_return;
    } catch (const PlaybackError &error) {
        failure = error.message();
    }

    if (isCancelled(token)) {
        silenceWithdrawnPlayback();
        co_return;
    }
    if (!isCurrent(song)) {
        co_return;
    }
    try {
        m_core->pause();
    } catch (const PlaybackError &) {
    }
    setPlaying(false);
    reportError(QStringLiteral("Restarting the audio output"), failure);
    syncMediaSession();
}

// --- skipping, seeking, volume ------------------------------------------------------------

void PlaybackController::skipBackward()
{
    // a finished song has nothing left in the core to rewind.
    if (m_livePositionMs > kRestartThresholdMs && !m_playbackNeedsLoad) {
        seek(0);
        return;
    }
    if (m_shuffle) {
        if (m_history.isEmpty()) {
            return;
        }
        startPlayback(m_history.takeLast(), false);
        return;
    }
    const int index = currentQueueIndex();
    if (index < 0) {
        return;
    }
    const QList<Song> queue = activeQueue();
    if (index > 0) {
        startPlayback(queue.at(index - 1), false);
    } else if (m_repeatMode == RepeatMode::All && !queue.isEmpty()) {
        startPlayback(queue.last(), false);
    }
}

void PlaybackController::skipToPreviousSong()
{
    const std::optional<Song> previous = precedingSong();
    if (!previous) {
        return;
    }
    if (m_shuffle && !m_history.isEmpty()) {
        m_history.removeLast();
    }
    startPlayback(*previous, false);
}

void PlaybackController::skipForward()
{
    if (const auto next = nextQueueSong(m_repeatMode == RepeatMode::All)) {
        startPlayback(*next);
    }
}

void PlaybackController::seek(int requestedMs)
{
    // until this song has loaded, or once it has finished, the core holds
    // something else: the previous song, or nothing it can resume.
    if (m_playbackNeedsLoad) {
        return;
    }
    quint32 positionMs = quint32(std::max(requestedMs, 0));
    if (m_durationMs > 0) {
        positionMs = std::min(positionMs, m_durationMs);
    }
    try {
        m_core->seek(positionMs);
        // whatever the core had queued about the old position is moot now,
        // including an end-of-track that would otherwise skip the song.
        m_core->takeEndOfTrack();
        setLivePosition(positionMs);
        // librespot reports loading while it re-buffers, which parks its
        // timeline as paused.
        if (m_playing) {
            m_core->play();
        } else if (!m_playbackRequest.isRunning()) {
            // a seek is never what starts the sound.
            m_core->pause();
        }
        syncMediaSession();
    } catch (const PlaybackError &error) {
        reportError(QStringLiteral("Seeking playback"), error.message());
    }
}

void PlaybackController::seekBy(int offsetMs)
{
    if (!m_currentSong || m_durationMs == 0) {
        return;
    }
    const qint64 destination = std::clamp<qint64>(qint64(m_livePositionMs) + offsetMs, 0, m_durationMs);
    seek(int(destination));
}

void PlaybackController::setVolume(double requested)
{
    if (!std::isfinite(requested)) {
        return;
    }
    const double volume = std::clamp(requested, 0.0, 1.0);
    if (volume == m_volume) {
        return;
    }
    try {
        m_core->setVolume(float(volume));
    } catch (const PlaybackError &error) {
        reportError(QStringLiteral("Changing volume"), error.message());
        return;
    }
    m_volume = volume;
    m_settings->setValue(kVolumeKey, volume);
    if (volume > 0.01) {
        m_volumeBeforeMute = volume;
        m_settings->setValue(kVolumeBeforeMuteKey, volume);
    }
    emit volumeChanged();
}

void PlaybackController::adjustVolume(double delta)
{
    setVolume(m_volume + delta);
}

void PlaybackController::toggleMute()
{
    setVolume(m_volume > 0.01 ? 0.0 : m_volumeBeforeMute);
}

void PlaybackController::toggleShuffle()
{
    m_shuffle = !m_shuffle;
    m_history.clear();
    if (m_shuffle) {
        rebuildShuffledQueue(m_currentSong);
    } else {
        m_shuffledQueue.clear();
    }
    emit shuffleChanged();
    queueDidChange();
    syncMediaSession();
}

void PlaybackController::cycleRepeatMode()
{
    switch (m_repeatMode) {
    case RepeatMode::Off: m_repeatMode = RepeatMode::All; break;
    case RepeatMode::All: m_repeatMode = RepeatMode::One; break;
    case RepeatMode::One: m_repeatMode = RepeatMode::Off; break;
    }
    emit repeatModeChanged();
    emit queueChanged();
    syncMediaSession();
}

// --- the playing song ---------------------------------------------------------------------

void PlaybackController::updatePlaybackWorkers()
{
    if (m_playing && m_currentSong) {
        if (!m_pollTimer.isActive()) {
            m_pollTimer.start();
        }
    } else {
        m_pollTimer.stop();
    }

    const bool analyse = m_visualizersEnabled && m_playing && m_presentationActive;
    m_core->setSpectrumEnabled(analyse);
    const bool pulseVisible = isLoadingPulseVisible();
    if (m_analysisActive != analyse || m_loadingPulseWasVisible != pulseVisible) {
        m_analysisActive = analyse;
        m_loadingPulseWasVisible = pulseVisible;
        emit analysisActiveChanged();
    }
}

void PlaybackController::setVisualizerPresentationActive(bool active)
{
    if (m_presentationActive == active) {
        return;
    }
    m_presentationActive = active;
    updatePlaybackWorkers();
}

void PlaybackController::pollPlayback()
{
    if (!m_currentSong) {
        m_pollTimer.stop();
        return;
    }
    syncPositionAndDuration();
    if (m_core->takeEndOfTrack() && isPlaybackAtEnd()) {
        handleEndOfTrack();
    }
    stopIfAudioOutputStalled();
}

void PlaybackController::syncPositionAndDuration()
{
    const quint32 position = m_core->playbackPositionMs();
    const quint32 duration = m_core->playbackDurationMs();
    if (position != m_livePositionMs) {
        setLivePosition(position);
    }
    setDuration(duration);
    if (std::abs(qint64(position) - qint64(m_lastSyncedMediaPosition)) >= 1000) {
        m_lastSyncedMediaPosition = position;
        syncMediaSession();
    }
}

void PlaybackController::stopIfAudioOutputStalled()
{
    // the device stopped taking what the core hands it: headphones pulled out,
    // a bluetooth device that left. rebuilding the output unasked would start
    // the song on whatever is now the default, usually the speaker the person
    // just walked away from.
    // not while the output is being opened anew, which can take a moment
    if (!m_playing || m_outputRepairPending || m_playbackRequest.isRunning() || !m_core->audioOutputStalled()) {
        return;
    }
#ifdef Q_OS_LINUX
    // here the sound server tells of a device that went away (AudioDeviceMonitor,
    // which pauses), so an output that fails with no such word means only the
    // stream broke, as when the server reconfigures itself as another program
    // starts. that is opened anew once; if it breaks again soon after, it is
    // the device, and playback pauses
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_lastOutputRepairAt == 0 || now - m_lastOutputRepairAt > 30000) {
        m_outputRepairPending = true;
        diagnostic("playback", QStringLiteral("the audio output failed; unless a device was lost, it is opened anew"));
        QTimer::singleShot(1500, this, [this] {
            m_outputRepairPending = false;
            // a lost device pauses the song by now, and then there is nothing to repair
            if (!m_playing || !m_core->audioOutputStalled()) {
                return;
            }
            m_lastOutputRepairAt = QDateTime::currentMSecsSinceEpoch();
            m_audioOutputNeedsRestart = true;
            detach(runResume(m_playbackRequest.renew()));
        });
        return;
    }
#endif
    diagnostic("playback", QStringLiteral("the audio output stopped taking sound, so playback pauses"));
    invalidateAudioOutput();
}

bool PlaybackController::isPlaybackAtEnd() const
{
    // the core only clears its end-of-track flag on the offline seek path, so
    // a stale one can survive a seek. a genuine end reports a position at the
    // duration on both paths.
    return m_durationMs == 0 || qint64(m_livePositionMs) + 2000 >= qint64(m_durationMs);
}

void PlaybackController::handleEndOfTrack()
{
    if (!m_currentSong) {
        return;
    }
    if (m_repeatMode == RepeatMode::One) {
        startPlayback(*m_currentSong, false);
        return;
    }
    if (const auto next = nextQueueSong(m_repeatMode == RepeatMode::All)) {
        startPlayback(*next);
        return;
    }
    // the queue is over, and the core is told so too: the offline output never
    // paused itself, and would play whatever it was handed next. neither
    // output can resume from the end, so play starts the song over.
    try {
        m_core->pause();
    } catch (const PlaybackError &) {
    }
    m_playbackNeedsLoad = true;
    setPlaying(false);
    syncMediaSession();
}

// --- lyrics -------------------------------------------------------------------------

void PlaybackController::clearLyrics()
{
    m_lyrics.reset();
    m_lyricsModel->setLines({});
    m_lyricsMessage.clear();
    m_loadingLyrics = false;
    emit lyricsChanged();
    updateActiveLyricLine();
}

void PlaybackController::loadLyrics(const Song &song)
{
    const CancelToken token = m_lyricsRequest.renew();
    m_lyrics.reset();
    m_lyricsModel->setLines({});
    m_lyricsMessage.clear();
    m_loadingLyrics = true;
    emit lyricsChanged();
    updateActiveLyricLine();
    detach(runLoadLyrics(song, token));
}

QCoro::Task<> PlaybackController::runLoadLyrics(Song song, CancelToken token)
{
    std::optional<Lyrics> lyrics;
    if (song.uri == QLatin1StringView(kLoadingTestUri)) {
        // nothing to ask anyone for
    } else if (song.isLocalFile()) {
        // the internet first, when allowed: it has synced lyrics for songs
        // whose files carry none. then a .lrc kept beside the file, then
        // whatever is written into the file's own tags
        if (m_lyricsFetcher && m_settings->onlineLyrics()) {
            lyrics = co_await m_lyricsFetcher->fetch(song);
        }
        if (!lyrics && !isCancelled(token) && isCurrent(song)) {
            lyrics = Lyrics::besideFile(song.localPath());
        }
        if (!lyrics && !isCancelled(token) && isCurrent(song)) {
            lyrics = Lyrics::embedded(song.localPath());
        }
    }
    if (isCancelled(token) || !isCurrent(song)) {
        co_return;
    }
    m_loadingLyrics = false;
    if (lyrics) {
        m_lyrics = std::move(lyrics);
        m_lyricsModel->setLines(m_lyrics->lines);
    } else {
        m_lyricsMessage = kCatchphrase;
    }
    m_lyricsRequest.finish(token);
    emit lyricsChanged();
    updateActiveLyricLine();
}

void PlaybackController::updateActiveLyricLine()
{
    const int index = m_lyrics ? m_lyrics->lineIndex(m_livePositionMs) : -1;
    if (index != m_activeLyricIndex) {
        m_activeLyricIndex = index;
        emit activeLyricIndexChanged();
    }
}

void PlaybackController::seekToLyricLine(int index)
{
    // unsynchronised lyrics have no times to seek to.
    if (!m_lyrics || !m_lyrics->synchronized || index < 0 || index >= m_lyrics->lines.size()) {
        return;
    }
    seek(int(m_lyrics->lines.at(index).startTimeMs));
}

void PlaybackController::setShowingLyrics(bool showing)
{
    if (m_showingLyrics == showing) {
        return;
    }
    m_showingLyrics = showing;
    if (showing) {
        m_showingQueue = false;
    }
    emit panelsChanged();
}

void PlaybackController::setShowingQueue(bool showing)
{
    if (m_showingQueue == showing) {
        return;
    }
    m_showingQueue = showing;
    if (showing) {
        m_showingLyrics = false;
    }
    emit panelsChanged();
}

void PlaybackController::toggleLyricsPanel()
{
    setShowingLyrics(!m_showingLyrics);
}

void PlaybackController::toggleQueuePanel()
{
    setShowingQueue(!m_showingQueue);
}

// --- sleep timer --------------------------------------------------------------------

bool PlaybackController::isVolumeManagedBySleepTimer() const
{
    if (!m_sleepTimerDeadline.isValid()) {
        return false;
    }
    switch (m_sleepFade) {
    case SleepFade::None:
        return false;
    case SleepFade::Entire:
        return true;
    case SleepFade::FinalSeconds: {
        const double window = std::min(10.0, m_sleepTimerDurationSeconds);
        return QDateTime::currentDateTimeUtc().msecsTo(m_sleepTimerDeadline) / 1000.0 <= window;
    }
    }
    return false;
}

void PlaybackController::startSleepTimer(int seconds, const QString &fade)
{
    cancelSleepTimer();
    if (seconds <= 0) {
        return;
    }
    m_sleepTimerDeadline = QDateTime::currentDateTimeUtc().addSecs(seconds);
    m_sleepTimerDurationSeconds = seconds;
    m_volumeBeforeSleepTimer = m_volume;
    m_sleepFade = sleepFadeFromString(fade);
    m_sleepTimer.start();
    emit sleepTimerChanged();
}

void PlaybackController::sleepTimerTick()
{
    const double remaining = QDateTime::currentDateTimeUtc().msecsTo(m_sleepTimerDeadline) / 1000.0;
    const double baseVolume = m_volumeBeforeSleepTimer;
    if (remaining > 0) {
        if (m_sleepFade != SleepFade::None) {
            const bool wasManaged = isVolumeManagedBySleepTimer();
            setVolume(baseVolume * sleepFadeLevel(m_sleepFade, remaining, m_sleepTimerDurationSeconds));
            if (wasManaged != isVolumeManagedBySleepTimer()) {
                emit sleepTimerChanged();
            }
        }
        return;
    }
    m_sleepTimer.stop();
    pause();
    setVolume(baseVolume);
    m_volumeBeforeSleepTimer = -1;
    m_sleepTimerDeadline = {};
    m_sleepFade = SleepFade::None;
    emit sleepTimerChanged();
}

void PlaybackController::cancelSleepTimer()
{
    m_sleepTimer.stop();
    if (m_volumeBeforeSleepTimer >= 0) {
        setVolume(m_volumeBeforeSleepTimer);
    }
    const bool wasRunning = m_sleepTimerDeadline.isValid();
    m_volumeBeforeSleepTimer = -1;
    m_sleepTimerDeadline = {};
    m_sleepFade = SleepFade::None;
    if (wasRunning) {
        emit sleepTimerChanged();
    }
}

// --- logout ------------------------------------------------------------------------

void PlaybackController::reset()
{
    m_playbackRequest.cancel();
    m_lyricsRequest.cancel();
    m_queue.clear();
    m_history.clear();
    m_shuffledQueue.clear();
    setCurrentSong(std::nullopt);
    m_playbackNeedsLoad = false;
    m_audioOutputNeedsRestart = false;
    setLoading(false);
    setPlaying(false);
    setLivePosition(0);
    setDuration(0);
    m_lastSyncedMediaPosition = std::numeric_limits<quint32>::max();
    if (m_shuffle) {
        m_shuffle = false;
        emit shuffleChanged();
    }
    if (m_repeatMode != RepeatMode::Off) {
        m_repeatMode = RepeatMode::Off;
        emit repeatModeChanged();
    }
    clearLyrics();
    m_showingLyrics = true;
    m_showingQueue = false;
    emit panelsChanged();
    // the new core starts at its own default volume
    try {
        m_core->setVolume(float(m_volume));
    } catch (const PlaybackError &) {
    }
    queueDidChange();
    syncMediaSession();
}

} // namespace swifty
