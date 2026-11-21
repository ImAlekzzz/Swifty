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

#include "core/RustCore.h"

#include "core/PlaybackError.h"

#include <swifty_core.h>

#include <QCoroSignal>

#ifndef Q_OS_WIN
#include <unistd.h>
#endif

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <utility>

namespace swifty {

/// one async call in flight
class CoreRequest : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

signals:
    void finished(bool ok, bool cancelled, const QString &payload);
};

namespace {

/// takes ownership of a string from the core
QString takeString(char *value)
{
    if (!value) {
        return {};
    }
    QString result = QString::fromUtf8(value);
    swifty_string_free(value);
    return result;
}

void throwIfError(char *error)
{
    if (error) {
        throw PlaybackError(PlaybackError::Kind::Core, takeString(error));
    }
}

} // namespace

RustCore::RustCore(QObject *parent)
    : PlaybackCore(parent)
{
    open();
}

RustCore::~RustCore()
{
    close();
}

QList<float> RustCore::equalizerFrequencies()
{
    float frequencies[16];
    const size_t count = swifty_equalizer_frequencies(frequencies, std::size(frequencies));
    return QList<float>(frequencies, frequencies + count);
}

void RustCore::open()
{
    m_core = swifty_core_new();
    if (!m_core) {
        qFatal("the audio core could not start");
    }
#ifdef Q_OS_WIN
    // no descriptor to watch on windows: the core calls back from whichever
    // thread finished, and the drain is queued onto this one
    swifty_core_set_completion_callback(m_core, [](void *context) {
        QMetaObject::invokeMethod(static_cast<RustCore *>(context), &RustCore::drainCompletions, Qt::QueuedConnection);
    }, this);
#else
    m_notifier = std::make_unique<QSocketNotifier>(swifty_core_completion_fd(m_core), QSocketNotifier::Read);
    connect(m_notifier.get(), &QSocketNotifier::activated, this, &RustCore::drainCompletions);
#endif
}

void RustCore::close()
{
#ifdef Q_OS_WIN
    // waits for a callback under way, so none reaches this after
    swifty_core_set_completion_callback(m_core, nullptr, nullptr);
#endif
    m_notifier.reset();
    // nothing will complete these now. waking them as cancelled is what lets
    // their callers unwind, as swift's task cancellation would.
    const auto pending = std::exchange(m_pending, {});
    for (const auto &request : pending) {
        emit request->finished(false, true, {});
    }
    swifty_core_free(m_core);
    m_core = nullptr;
}

void RustCore::drainCompletions()
{
#ifndef Q_OS_WIN
    char buffer[64];
    while (::read(swifty_core_completion_fd(m_core), buffer, sizeof buffer) > 0) {
    }
#endif

    uint64_t id = 0;
    bool ok = false;
    char *payload = nullptr;
    while (swifty_core_take_completion(m_core, &id, &ok, &payload)) {
        const QString text = takeString(payload);
        if (const auto request = m_pending.take(id)) {
            emit request->finished(ok, false, text);
        }
    }
}

template <typename Start>
QCoro::Task<QString> RustCore::request(Start start)
{
    const quint64 id = ++m_nextRequestId;
    const auto pending = std::make_shared<CoreRequest>();
    m_pending.insert(id, pending);
    start(id);
    const auto [ok, cancelled, payload] = co_await qCoro(pending.get(), &CoreRequest::finished);
    if (cancelled) {
        throw Cancelled();
    }
    if (!ok) {
        throw PlaybackError(payload == QLatin1StringView(SWIFTY_CORE_SUPERSEDED) ? PlaybackError::Kind::Superseded
                                                                                 : PlaybackError::Kind::Core,
                           payload);
    }
    co_return payload;
}

QCoro::Task<> RustCore::playFile(QString path)
{
    const QByteArray file = path.toUtf8();
    co_await request([&](quint64 id) { swifty_core_play_file(m_core, id, file.constData()); });
}

QCoro::Task<> RustCore::restartAudioOutput()
{
    co_await request([&](quint64 id) { swifty_core_restart_audio_output(m_core, id); });
}

void RustCore::play() { throwIfError(swifty_core_play(m_core)); }
void RustCore::pause() { throwIfError(swifty_core_pause(m_core)); }
void RustCore::seek(quint32 positionMs) { throwIfError(swifty_core_seek(m_core, positionMs)); }
void RustCore::setVolume(float volume) { throwIfError(swifty_core_set_volume(m_core, volume)); }
float RustCore::volume() const { return swifty_core_volume(m_core); }
void RustCore::setPlaybackShaping(float tempo, float pitchSemitones, bool preservesPitch)
{
    throwIfError(swifty_core_set_playback_shaping(m_core, tempo, pitchSemitones, preservesPitch));
}

void RustCore::setEqualizer(const QList<float> &gainsDb)
{
    throwIfError(swifty_core_set_equalizer(m_core, gainsDb.constData(), size_t(gainsDb.size())));
}

bool RustCore::audioOutputStalled() const { return swifty_core_audio_output_stalled(m_core); }
quint32 RustCore::playbackPositionMs() const { return swifty_core_playback_position_ms(m_core); }
quint32 RustCore::playbackDurationMs() const { return swifty_core_playback_duration_ms(m_core); }
bool RustCore::takeEndOfTrack() { return swifty_core_take_end_of_track(m_core); }

void RustCore::setSpectrumEnabled(bool enabled)
{
    swifty_core_set_spectrum_enabled(m_core, enabled);
}

int RustCore::spectrumLevels(float *out, int capacity) const
{
    return int(swifty_core_spectrum_levels(m_core, out, size_t(std::max(capacity, 0))));
}

} // namespace swifty

#include "RustCore.moc"
