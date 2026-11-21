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

#pragma once

#include <QList>
#include <QObject>
#include <QStringList>
#include <QCoroTask>

namespace swifty {

/// what the player asks of the audio core: the rust core in the app, a fake in
/// tests. calls that can fail throw PlaybackError with the core's own message;
/// async calls are cancelled (throw Cancelled) when the core goes away under them.
class PlaybackCore : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

    /// a music file, from anywhere on disk
    virtual QCoro::Task<> playFile(QString path) = 0;
    virtual QCoro::Task<> restartAudioOutput() = 0;

    virtual void play() = 0;
    virtual void pause() = 0;
    virtual void seek(quint32 positionMs) = 0;
    virtual void setVolume(float volume) = 0;
    virtual float volume() const = 0;
    virtual void setPlaybackShaping(float tempo, float pitchSemitones, bool preservesPitch) = 0;
    virtual void setEqualizer(const QList<float> &gainsDb) = 0;

    virtual bool audioOutputStalled() const = 0;
    virtual quint32 playbackPositionMs() const = 0;
    virtual quint32 playbackDurationMs() const = 0;
    virtual bool takeEndOfTrack() = 0;

    virtual void setSpectrumEnabled(bool enabled) = 0;
    /// copies up to `capacity` band levels, 0...1; returns how many
    virtual int spectrumLevels(float *out, int capacity) const = 0;

    static constexpr int kSpectrumBands = 48;
};

} // namespace swifty
