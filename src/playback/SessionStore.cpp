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

#include "playback/SessionStore.h"

#include "app/Settings.h"
#include "playback/PlaybackController.h"

#include <QCoreApplication>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <optional>

namespace swifty {

namespace {
const QString kSessionKey = QStringLiteral("session");
constexpr int kQueueLimit = 500;
/// the settings file is written whole each time, so a place in a song is kept
/// every so often while it plays, and always on a pause, a change and a quit
constexpr int kSaveEveryMs = 45'000;
/// a song left in its last seconds starts over rather than ending at once
constexpr quint32 kNearEndMs = 5'000;
} // namespace

SessionStore::SessionStore(Settings *settings, PlaybackController *player, Catalog catalog, QObject *parent)
    : QObject(parent)
    , m_settings(settings)
    , m_player(player)
    , m_catalog(std::move(catalog))
{
    m_whilePlaying.setInterval(kSaveEveryMs);
    connect(&m_whilePlaying, &QTimer::timeout, this, &SessionStore::save);
    m_afterChange.setSingleShot(true);
    m_afterChange.setInterval(1000);
    connect(&m_afterChange, &QTimer::timeout, this, &SessionStore::save);

    connect(player, &PlaybackController::currentSongChanged, &m_afterChange, qOverload<>(&QTimer::start));
    connect(player, &PlaybackController::queueChanged, &m_afterChange, qOverload<>(&QTimer::start));
    connect(player, &PlaybackController::playingChanged, this, [this] {
        if (m_player->isPlaying()) {
            m_whilePlaying.start();
        } else {
            m_whilePlaying.stop();
            save();
        }
    });
    if (QCoreApplication::instance()) {
        connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, &SessionStore::save);
    }
}

void SessionStore::save()
{
    // before anything is playing, what was saved last time is kept
    if (!m_player->hasSong()) {
        return;
    }
    QJsonArray queue;
    for (const Song &song : m_player->queueSongs()) {
        if (song.isLocalFile() && queue.size() < kQueueLimit) {
            queue.append(song.uri);
        }
    }
    const Song current = m_player->currentSong();
    if (!current.isLocalFile()) {
        return;
    }
    const QJsonObject session {
        {QStringLiteral("current"), current.uri},
        {QStringLiteral("position"), int(m_player->livePositionMs())},
        {QStringLiteral("queue"), queue},
    };
    m_settings->setValue(kSessionKey, QJsonDocument(session).toJson(QJsonDocument::Compact));
}

bool SessionStore::restore()
{
    if (m_restored || m_player->hasSong()) {
        return false;
    }
    const QJsonObject session = QJsonDocument::fromJson(m_settings->value(kSessionKey).toByteArray()).object();
    const QString currentUri = session.value(QStringLiteral("current")).toString();
    if (currentUri.isEmpty()) {
        m_restored = true;
        return false;
    }
    const QList<Song> songs = m_catalog();
    if (songs.isEmpty()) {
        // the library has not been read yet
        return false;
    }
    m_restored = true;

    QHash<QString, Song> byUri;
    for (const Song &song : songs) {
        byUri.insert(song.uri, song);
    }
    const auto found = byUri.constFind(currentUri);
    if (found == byUri.cend()) {
        // the file is gone
        return false;
    }
    // each its own occurrence, as a queue made by play is
    QList<Song> queue;
    std::optional<Song> current;
    for (const QJsonValue &value : session.value(QStringLiteral("queue")).toArray()) {
        const auto item = byUri.constFind(value.toString());
        if (item == byUri.cend()) {
            continue;
        }
        const Song song = item->freshInstance();
        queue.append(song);
        if (!current && song.uri == currentUri) {
            current = song;
        }
    }
    if (!current) {
        current = found->freshInstance();
        queue = {*current};
    }
    quint32 position = quint32(std::max(session.value(QStringLiteral("position")).toInt(), 0));
    if (current->durationMs > 0 && position + kNearEndMs >= quint32(current->durationMs)) {
        position = 0;
    }
    m_player->restoreSession(*current, queue, position);
    return true;
}

} // namespace swifty
