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

#include "core/MusicTypes.h"

#include <QJsonArray>

#include <atomic>

namespace swifty {

namespace {

quint64 nextInstanceId()
{
    static std::atomic<quint64> counter {0};
    return ++counter;
}

} // namespace

QJsonObject Artist::toJson() const
{
    return {
        {QStringLiteral("id"), id},
        {QStringLiteral("name"), name},
        {QStringLiteral("artworkURL"), artworkUrl.toString()},
        {QStringLiteral("uri"), uri},
    };
}

Artist Artist::fromJson(const QJsonObject &object)
{
    return {
        object.value(QStringLiteral("id")).toString(),
        object.value(QStringLiteral("name")).toString(),
        QUrl(object.value(QStringLiteral("artworkURL")).toString()),
        object.value(QStringLiteral("uri")).toString(),
    };
}

Song::Song()
    : instanceId(nextInstanceId())
{
}

Song Song::freshInstance() const
{
    Song copy = *this;
    copy.instanceId = nextInstanceId();
    return copy;
}

QJsonObject Song::toJson() const
{
    QJsonArray artistArray;
    for (const Artist &artist : artistItems) {
        artistArray.append(artist.toJson());
    }
    QJsonObject object {
        {QStringLiteral("name"), name},
        {QStringLiteral("artists"), artists},
        {QStringLiteral("uri"), uri},
        {QStringLiteral("isExplicit"), isExplicit},
    };
    if (!albumName.isEmpty()) object.insert(QStringLiteral("albumName"), albumName);
    if (!genre.isEmpty()) object.insert(QStringLiteral("genre"), genre);
    if (artworkUrl.isValid()) object.insert(QStringLiteral("artworkURL"), artworkUrl.toString());
    if (durationMs > 0) object.insert(QStringLiteral("durationMs"), durationMs);
    if (!albumId.isEmpty()) object.insert(QStringLiteral("albumID"), albumId);
    if (!albumUri.isEmpty()) object.insert(QStringLiteral("albumURI"), albumUri);
    if (!artistArray.isEmpty()) object.insert(QStringLiteral("artistItems"), artistArray);
    return object;
}

Song Song::fromJson(const QJsonObject &object)
{
    Song song;
    song.name = object.value(QStringLiteral("name")).toString();
    song.artists = object.value(QStringLiteral("artists")).toString();
    song.albumName = object.value(QStringLiteral("albumName")).toString();
    song.genre = object.value(QStringLiteral("genre")).toString();
    song.artworkUrl = QUrl(object.value(QStringLiteral("artworkURL")).toString());
    song.uri = object.value(QStringLiteral("uri")).toString();
    song.durationMs = object.value(QStringLiteral("durationMs")).toInt();
    song.isExplicit = object.value(QStringLiteral("isExplicit")).toBool();
    song.albumId = object.value(QStringLiteral("albumID")).toString();
    song.albumUri = object.value(QStringLiteral("albumURI")).toString();
    for (const QJsonValue &value : object.value(QStringLiteral("artistItems")).toArray()) {
        const Artist artist = Artist::fromJson(value.toObject());
        if (artist.isValid()) {
            song.artistItems.append(artist);
        }
    }
    return song;
}

} // namespace swifty
