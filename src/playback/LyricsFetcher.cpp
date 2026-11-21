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

#include "playback/LyricsFetcher.h"

#include <QCoroNetworkReply>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QUrlQuery>

#include <algorithm>
#include <cstdlib>

namespace swifty {

Q_LOGGING_CATEGORY(lcLyrics, "swifty.lyrics")

namespace {

constexpr int kTimeoutMs = 8000;
/// how far a result's length may be from the song's, for it to be the same song
constexpr int kLrclibToleranceMs = 4000;
constexpr int kMusicBrainzToleranceMs = 6000;

QString primaryArtist(const Song &song)
{
    return song.artists.split(QStringLiteral(", "), Qt::SkipEmptyParts).value(0).trimmed();
}

/// lowercase letters and digits only, to compare titles that differ in punctuation
QString plain(const QString &text)
{
    QString result;
    for (const QChar c : text) {
        if (c.isLetterOrNumber()) {
            result.append(c.toLower());
        }
    }
    return result;
}

bool hasTimes(const QString &text)
{
    const std::optional<Lyrics> lyrics = Lyrics::fromLrc(text);
    return lyrics && lyrics->synchronized;
}

} // namespace

LyricsFetcher::LyricsFetcher(QObject *parent)
    : QObject(parent)
{
}

QString LyricsFetcher::cacheKey(const Song &song)
{
    const QString key = song.name.toLower() + QChar(0x1f) + song.artists.toLower() + QChar(0x1f)
        + song.albumName.toLower() + QChar(0x1f) + QString::number(song.durationMs / 1000);
    return QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex());
}

std::optional<Lyrics> LyricsFetcher::toLyrics(const Found &found)
{
    std::optional<Lyrics> lyrics = Lyrics::fromLrc(found.text);
    if (lyrics) {
        lyrics->provider = found.provider;
    }
    return lyrics;
}

// --- parsing ---------------------------------------------------------------------------------

std::optional<LyricsFetcher::Found> LyricsFetcher::fromLrclib(const QByteArray &json, int durationMs)
{
    const QJsonDocument document = QJsonDocument::fromJson(json);
    QList<QJsonObject> entries;
    if (document.isArray()) {
        for (const QJsonValue &value : document.array()) {
            entries.append(value.toObject());
        }
    } else if (document.isObject()) {
        entries.append(document.object());
    }

    struct Candidate {
        QString text;
        bool timed;
        int distance;
    };
    QList<Candidate> candidates;
    for (const QJsonObject &entry : std::as_const(entries)) {
        if (entry.value(QStringLiteral("instrumental")).toBool()) {
            continue;
        }
        int distance = 0;
        const double seconds = entry.value(QStringLiteral("duration")).toDouble(-1);
        if (durationMs > 0 && seconds > 0) {
            distance = std::abs(int(seconds * 1000) - durationMs);
            if (distance > kLrclibToleranceMs) {
                continue;
            }
        }
        const QString synced = entry.value(QStringLiteral("syncedLyrics")).toString().trimmed();
        const QString unsynced = entry.value(QStringLiteral("plainLyrics")).toString().trimmed();
        if (!synced.isEmpty() && hasTimes(synced)) {
            candidates.append({synced, true, distance});
        } else if (!unsynced.isEmpty()) {
            candidates.append({unsynced, false, distance});
        }
    }
    if (candidates.isEmpty()) {
        return std::nullopt;
    }
    // synced first, then the closest in length
    const auto best = std::min_element(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) {
        return a.timed != b.timed ? a.timed : a.distance < b.distance;
    });
    return Found {QStringLiteral("LRCLIB"), best->text, best->timed};
}

std::optional<LyricsFetcher::Recording> LyricsFetcher::pickMusicBrainzRecording(const QByteArray &searchJson, const Song &song)
{
    const QJsonArray recordings = QJsonDocument::fromJson(searchJson).object().value(QStringLiteral("recordings")).toArray();
    const QString wantedName = plain(song.name);
    const QString wantedArtist = plain(primaryArtist(song));

    std::optional<Recording> best;
    qint64 bestScore = std::numeric_limits<qint64>::min();
    for (const QJsonValue &value : recordings) {
        const QJsonObject entry = value.toObject();
        const QString title = entry.value(QStringLiteral("title")).toString();
        const QString name = plain(title);
        int nameScore = 0;
        if (!wantedName.isEmpty() && name == wantedName) {
            nameScore = 2;
        } else if (!wantedName.isEmpty() && !name.isEmpty() && (name.contains(wantedName) || wantedName.contains(name))) {
            nameScore = 1;
        }
        if (nameScore == 0) {
            continue;
        }
        qint64 distance = 0;
        const qint64 length = entry.value(QStringLiteral("length")).toInteger();
        if (song.durationMs > 0 && length > 0) {
            distance = std::llabs(length - song.durationMs);
            if (distance > kMusicBrainzToleranceMs) {
                continue;
            }
        }
        QString artist;
        int artistScore = 0;
        for (const QJsonValue &credit : entry.value(QStringLiteral("artist-credit")).toArray()) {
            const QJsonObject object = credit.toObject();
            QString credited = object.value(QStringLiteral("artist")).toObject().value(QStringLiteral("name")).toString();
            if (credited.isEmpty()) {
                credited = object.value(QStringLiteral("name")).toString();
            }
            const QString named = plain(credited);
            if (!wantedArtist.isEmpty() && !named.isEmpty() && (named.contains(wantedArtist) || wantedArtist.contains(named))) {
                artistScore = 1;
                artist = credited;
                break;
            }
            if (artist.isEmpty()) {
                artist = credited;
            }
        }
        // a song by somebody else is not this song
        if (!wantedArtist.isEmpty() && artistScore == 0) {
            continue;
        }
        const qint64 score = nameScore * 10000 + artistScore * 5000 - distance;
        if (score > bestScore) {
            bestScore = score;
            best = Recording {title, artist, int(length)};
        }
    }
    return best;
}

// --- the network -------------------------------------------------------------------------------

QCoro::Task<QByteArray> LyricsFetcher::get(QUrl url)
{
    QNetworkRequest request(url);
    request.setTransferTimeout(kTimeoutMs);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("Swifty/%1 (https://github.com/ImAlekzzz/swifty)").arg(QStringLiteral(SWIFTY_VERSION)));
    QNetworkReply *reply = m_network.get(request);
    co_await reply;
    QByteArray body;
    if (reply->error() == QNetworkReply::NoError) {
        body = reply->readAll();
    } else {
        qCDebug(lcLyrics).noquote() << url.host() << ':' << reply->errorString();
    }
    reply->deleteLater();
    co_return body;
}

QCoro::Task<std::optional<LyricsFetcher::Found>> LyricsFetcher::lrclibExact(Song song)
{
    QUrl url(QStringLiteral("https://lrclib.net/api/get"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("track_name"), song.name);
    query.addQueryItem(QStringLiteral("artist_name"), primaryArtist(song));
    if (!song.albumName.isEmpty()) {
        query.addQueryItem(QStringLiteral("album_name"), song.albumName);
    }
    if (song.durationMs > 0) {
        query.addQueryItem(QStringLiteral("duration"), QString::number(qRound(song.durationMs / 1000.0)));
    }
    url.setQuery(query);
    co_return fromLrclib(co_await get(url), song.durationMs);
}

QCoro::Task<std::optional<LyricsFetcher::Found>> LyricsFetcher::lrclibSearch(Song song)
{
    QUrl url(QStringLiteral("https://lrclib.net/api/search"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("track_name"), song.name);
    if (!primaryArtist(song).isEmpty()) {
        query.addQueryItem(QStringLiteral("artist_name"), primaryArtist(song));
    }
    url.setQuery(query);
    co_return fromLrclib(co_await get(url), song.durationMs);
}

QCoro::Task<std::optional<LyricsFetcher::Found>> LyricsFetcher::musicBrainz(Song song)
{
    QUrl search(QStringLiteral("https://musicbrainz.org/ws/2/recording"));
    QString text = QStringLiteral("recording:\"%1\"").arg(QString(song.name).remove(QLatin1Char('"')));
    if (!primaryArtist(song).isEmpty()) {
        text += QStringLiteral(" AND artist:\"%1\"").arg(QString(primaryArtist(song)).remove(QLatin1Char('"')));
    }
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("query"), text);
    query.addQueryItem(QStringLiteral("fmt"), QStringLiteral("json"));
    query.addQueryItem(QStringLiteral("limit"), QStringLiteral("10"));
    search.setQuery(query);

    const std::optional<Recording> recording = pickMusicBrainzRecording(co_await get(search), song);
    if (!recording) {
        co_return std::nullopt;
    }
    // MusicBrainz has no lyrics: it only says what the song is really called,
    // so if that is no different from what LRCLIB was already asked, stop here
    Song corrected = song;
    corrected.name = recording->title;
    corrected.artists = recording->artist;
    if (recording->lengthMs > 0) {
        corrected.durationMs = recording->lengthMs;
    }
    if (plain(corrected.name) == plain(song.name) && plain(primaryArtist(corrected)) == plain(primaryArtist(song))
        && corrected.durationMs == song.durationMs) {
        co_return std::nullopt;
    }
    if (const std::optional<Found> found = co_await lrclibExact(corrected)) {
        co_return found;
    }
    co_return co_await lrclibSearch(corrected);
}

// --- the cache ---------------------------------------------------------------------------------------

namespace {

QString cacheFile(const QString &key)
{
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/lyrics/") + key
        + QStringLiteral(".json");
}

} // namespace

std::optional<LyricsFetcher::Found> LyricsFetcher::cached(const Song &song) const
{
    QFile file(cacheFile(cacheKey(song)));
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
    const QString text = object.value(QStringLiteral("text")).toString();
    if (text.isEmpty()) {
        return std::nullopt;
    }
    return Found {object.value(QStringLiteral("provider")).toString(), text, object.value(QStringLiteral("timed")).toBool()};
}

void LyricsFetcher::remember(const Song &song, const Found &found) const
{
    const QString path = cacheFile(cacheKey(song));
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return;
    }
    file.write(QJsonDocument(QJsonObject {
                                 {QStringLiteral("provider"), found.provider},
                                 {QStringLiteral("text"), found.text},
                                 {QStringLiteral("timed"), found.timed},
                             }).toJson(QJsonDocument::Compact));
}

// --- the lookup -----------------------------------------------------------------------------------------

QCoro::Task<std::optional<Lyrics>> LyricsFetcher::fetch(Song song)
{
    if (song.name.trimmed().isEmpty()) {
        co_return std::nullopt;
    }
    if (const std::optional<Found> hit = cached(song)) {
        co_return toLyrics(*hit);
    }
    const QString key = cacheKey(song);
    if (m_misses.contains(key)) {
        co_return std::nullopt;
    }

    // time-synced lyrics beat plain ones wherever they come from; a plain
    // answer is kept while the next service is tried for a synced one
    std::optional<Found> best;
    for (int step = 0; step < 3; ++step) {
        std::optional<Found> found;
        switch (step) {
        case 0: found = co_await lrclibExact(song); break;
        case 1: found = co_await lrclibSearch(song); break;
        default: found = co_await musicBrainz(song); break;
        }
        if (!found) {
            continue;
        }
        if (found->timed) {
            best = found;
            break;
        }
        if (!best) {
            best = found;
        }
    }
    if (!best) {
        m_misses.insert(key);
        co_return std::nullopt;
    }
    remember(song, *best);
    co_return toLyrics(*best);
}

} // namespace swifty
