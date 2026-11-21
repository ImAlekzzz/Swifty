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

#include "app/Settings.h"
#include "playback/Lyrics.h"
#include "playback/LyricsFetcher.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

using swifty::Lyrics;
using swifty::LyricsFetcher;
using swifty::Settings;
using swifty::Song;

namespace {

Song song(const QString &name, const QString &artists, int durationMs)
{
    Song result;
    result.name = name;
    result.artists = artists;
    result.durationMs = durationMs;
    return result;
}

QByteArray json(const QJsonValue &value)
{
    return value.isArray() ? QJsonDocument(value.toArray()).toJson() : QJsonDocument(value.toObject()).toJson();
}

QJsonObject lrclib(double seconds, const QString &synced, const QString &plain, bool instrumental = false)
{
    return {{QStringLiteral("duration"), seconds},
            {QStringLiteral("syncedLyrics"), synced.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(synced)},
            {QStringLiteral("plainLyrics"), plain},
            {QStringLiteral("instrumental"), instrumental}};
}

const QString kSynced = QStringLiteral("[00:01.00]one\n[00:02.50]two");

} // namespace

class LyricsTest : public QObject {
    Q_OBJECT

private slots:
    void lrclibSyncedLyricsAreTimed()
    {
        const auto found = LyricsFetcher::fromLrclib(json(lrclib(200.4, kSynced, QStringLiteral("one\ntwo"))), 200000);
        QVERIFY(found);
        QVERIFY(found->timed);
        QCOMPARE(found->provider, QStringLiteral("LRCLIB"));
        const auto lyrics = LyricsFetcher::toLyrics(*found);
        QVERIFY(lyrics);
        QVERIFY(lyrics->synchronized);
        QCOMPARE(lyrics->lines.size(), 2);
        QCOMPARE(lyrics->lines.at(1).startTimeMs, 2500u);
    }

    void lrclibPlainOnlyIsNotTimed()
    {
        const auto found = LyricsFetcher::fromLrclib(json(lrclib(200, QString(), QStringLiteral("one\ntwo"))), 200000);
        QVERIFY(found);
        QVERIFY(!found->timed);
    }

    void lrclibSearchTakesTheSyncedOneClosestInLength()
    {
        QJsonArray results;
        results.append(lrclib(260, kSynced, QStringLiteral("far")));        // another version of the song
        results.append(lrclib(201.5, QString(), QStringLiteral("plain")));  // right length, not synced
        results.append(lrclib(203, QStringLiteral("[00:09.00]late"), QString()));
        results.append(lrclib(200.2, kSynced, QString()));
        const auto found = LyricsFetcher::fromLrclib(json(results), 200000);
        QVERIFY(found);
        QVERIFY(found->timed);
        QVERIFY(found->text.contains(QStringLiteral("two")));
    }

    void lrclibSkipsInstrumentalsAndWrongLengths()
    {
        QJsonArray results;
        results.append(lrclib(200, kSynced, QString(), true));
        results.append(lrclib(300, kSynced, QString()));
        QVERIFY(!LyricsFetcher::fromLrclib(json(results), 200000));
        QVERIFY(!LyricsFetcher::fromLrclib(QByteArrayLiteral("{\"code\":404}"), 200000));
        QVERIFY(!LyricsFetcher::fromLrclib(QByteArrayLiteral("not json"), 200000));
    }

    void musicBrainzSearchPicksTheSameSong()
    {
        const auto entry = [](const QString &title, const QString &artist, qint64 length) {
            return QJsonObject {{QStringLiteral("title"), title},
                                {QStringLiteral("length"), length},
                                {QStringLiteral("artist-credit"),
                                 QJsonArray {QJsonObject {{QStringLiteral("name"), artist},
                                                          {QStringLiteral("artist"), QJsonObject {{QStringLiteral("name"), artist}}}}}}};
        };
        const QJsonObject search {{QStringLiteral("recordings"),
                                   QJsonArray {entry(QStringLiteral("Smooth Operator (Live)"), QStringLiteral("Sade"), 310000),
                                               entry(QStringLiteral("Smooth Operator"), QStringLiteral("Cover Band"), 258000),
                                               entry(QStringLiteral("Smooth Operator"), QStringLiteral("Sade"), 257000),
                                               entry(QStringLiteral("Other Song"), QStringLiteral("Sade"), 257000)}}};
        const Song wanted = song(QStringLiteral("Smooth Operator"), QStringLiteral("Sade"), 257500);
        const auto picked = LyricsFetcher::pickMusicBrainzRecording(json(search), wanted);
        QVERIFY(picked);
        QCOMPARE(picked->title, QStringLiteral("Smooth Operator"));
        QCOMPARE(picked->artist, QStringLiteral("Sade"));
        QCOMPARE(picked->lengthMs, 257000);
        // a different length is a different recording
        QVERIFY(!LyricsFetcher::pickMusicBrainzRecording(json(search), song(QStringLiteral("Smooth Operator"), QStringLiteral("Sade"), 100000)));
        QVERIFY(!LyricsFetcher::pickMusicBrainzRecording(QByteArrayLiteral("{}"), wanted));
    }

    void aSongIsRememberedByItsDetails()
    {
        const Song a = song(QStringLiteral("Name"), QStringLiteral("Artist"), 200000);
        Song b = a;
        QCOMPARE(LyricsFetcher::cacheKey(a), LyricsFetcher::cacheKey(b));
        b.durationMs = 230000;
        QVERIFY(LyricsFetcher::cacheKey(a) != LyricsFetcher::cacheKey(b));
    }

    void onlineLyricsAreOnByDefaultAndRemembered()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("swifty.conf"));
        {
            Settings settings(path);
            QCOMPARE(settings.onlineLyrics(), true);
            settings.setOnlineLyrics(false);
        }
        Settings reopened(path);
        QCOMPARE(reopened.onlineLyrics(), false);
    }

    void aFileWithNoLyricsHasNone()
    {
        QVERIFY(!Lyrics::embedded(QStringLiteral("/nowhere/at/all.flac")));
    }
};

QTEST_GUILESS_MAIN(LyricsTest)
#include "tst_lyrics.moc"
