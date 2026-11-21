/*
 * Swifty, a fork of Swiftify
 * Copyright (C) 2026  ImAlekz
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

#include "core/MusicTypes.h"
#include "playback/Lyrics.h"

#include <QCoroTask>
#include <QNetworkAccessManager>
#include <QObject>
#include <QSet>

#include <optional>

namespace swifty {

/// looks a song's lyrics up on the internet: LRCLIB first, then LRCLIB again
/// with the title, artist and length MusicBrainz knows the song by, taking
/// time-synced lyrics over plain text wherever they are found. MusicBrainz holds
/// no lyrics itself, it only corrects badly tagged songs. what is found is kept
/// on disk, so a song is asked about once.
///
/// both services are free and may change or go away.
class LyricsFetcher : public QObject {
    Q_OBJECT

public:
    /// lyrics as the services give them: lrc text, or plain text
    struct Found {
        QString provider;
        QString text;
        bool timed = false;
    };

    /// a recording as MusicBrainz knows it
    struct Recording {
        QString title;
        QString artist;
        int lengthMs = 0;
    };

    explicit LyricsFetcher(QObject *parent = nullptr);

    /// empty when neither service knows the song, or nothing could be reached
    QCoro::Task<std::optional<Lyrics>> fetch(Song song);

    /// LRCLIB's answer to /api/get (one song) or /api/search (several): the
    /// best entry for a song `durationMs` long, synced ones first
    static std::optional<Found> fromLrclib(const QByteArray &json, int durationMs);
    /// the recording in MusicBrainz's /ws/2/recording search that is the song
    /// asked for, if any
    static std::optional<Recording> pickMusicBrainzRecording(const QByteArray &searchJson, const Song &song);
    /// what a song is remembered by
    static QString cacheKey(const Song &song);
    static std::optional<Lyrics> toLyrics(const Found &found);

private:
    QCoro::Task<QByteArray> get(QUrl url);
    QCoro::Task<std::optional<Found>> lrclibExact(Song song);
    QCoro::Task<std::optional<Found>> lrclibSearch(Song song);
    /// asks MusicBrainz what the song is really called, then asks LRCLIB again
    QCoro::Task<std::optional<Found>> musicBrainz(Song song);

    std::optional<Found> cached(const Song &song) const;
    void remember(const Song &song, const Found &found) const;

    QNetworkAccessManager m_network;
    /// songs nobody knew this time, so skipping back and forth does not ask again
    QSet<QString> m_misses;
};

} // namespace swifty
