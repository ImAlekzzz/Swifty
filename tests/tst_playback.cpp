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

#include "FakeCore.h"

#include "app/ErrorCenter.h"
#include "app/Settings.h"
#include "playback/InteractionState.h"
#include "playback/PlaybackController.h"
#include "playback/SessionStore.h"

#include <QFile>
#include <QNetworkAccessManager>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace swifty;
using swifty::testing::FakeCore;

namespace {

Song makeSong(const QString &name)
{
    Song song;
    song.name = name;
    song.artists = QStringLiteral("Artist");
    // a music file of one's own: it plays from disk, with no network, which
    // none of these rules depend on
    song.uri = QUrl::fromLocalFile(QStringLiteral("/music/") + name + QStringLiteral(".flac")).toString();
    song.durationMs = 200'000;
    return song;
}

QList<Song> makeQueue(int count)
{
    QList<Song> songs;
    for (int index = 0; index < count; ++index) {
        songs.append(makeSong(QStringLiteral("song%1").arg(index)));
    }
    return songs;
}

} // namespace

/// the player's rules, Appendix C of the plan: nothing but a person, or a queue
/// that is already playing, may start sound.
class PlaybackTest : public QObject {
    Q_OBJECT

private:
    struct Rig {
        QTemporaryDir directory;
        Settings settings {directory.filePath(QStringLiteral("swifty.conf"))};
        ErrorCenter errors {nullptr};
        QNetworkAccessManager network;
        FakeCore core;
        PlaybackController player {&core, &settings, &errors};

        /// lets the song the player asked for finish loading
        void finishLoading()
        {
            settle();
            const auto call = core.take(QStringLiteral("playFile"));
            QVERIFY(call);
            call->succeed();
            settle();
        }

        /// qcoro resumes a coroutine waiting on a signal from the event loop
        static void settle()
        {
            for (int pass = 0; pass < 5; ++pass) {
                QCoreApplication::processEvents();
            }
        }
    };

private slots:
    void playingASongLoadsItAndStartsIt()
    {
        Rig rig;
        const QList<Song> queue = makeQueue(3);
        rig.player.play(queue[1], queue);
        QVERIFY(rig.player.isLoading());
        QVERIFY(!rig.player.isPlaying());
        rig.finishLoading();
        QVERIFY(rig.player.isPlaying());
        QVERIFY(!rig.player.isLoading());
        QVERIFY(rig.core.sounding);
        QCOMPARE(rig.player.upNextSongs().size(), 1);
        QCOMPARE(rig.player.previouslyPlayedSongs().size(), 1);
    }

    void theQueueStepsOnWithoutAReset()
    {
        Rig rig;
        const QList<Song> queue = makeQueue(5);
        rig.player.play(queue[0], queue);
        rig.finishLoading();
        QCOMPARE(rig.player.upNext()->count(), 4);

        QSignalSpy resets(rig.player.upNext(), &QAbstractItemModel::modelReset);
        QSignalSpy removals(rig.player.upNext(), &QAbstractItemModel::rowsRemoved);
        rig.player.skipForward();
        rig.finishLoading();
        QCOMPARE(resets.count(), 0);
        QCOMPARE(removals.count(), 1);
        QCOMPARE(removals.first().at(1).toInt(), 0);
        QCOMPARE(rig.player.upNext()->count(), 3);
        QCOMPARE(rig.player.upNext()->at(0).uri, queue[2].uri);
    }

    void pauseWinsOverALoadStillInFlight()
    {
        Rig rig;
        const QList<Song> queue = makeQueue(2);
        rig.player.play(queue[0], queue);
        rig.player.pause();
        // the core finishes the load it was given and starts the track anyway
        rig.finishLoading();
        QVERIFY(!rig.player.isPlaying());
        QVERIFY(!rig.core.sounding);
        // play starts it properly from there
        rig.player.resume();
        rig.finishLoading();
        QVERIFY(rig.player.isPlaying());
    }

    void aFailedLoadLeavesTheCorePaused()
    {
        Rig rig;
        const QList<Song> queue = makeQueue(1);
        rig.player.play(queue[0], queue);
        rig.core.take(QStringLiteral("playFile"))->fail(QStringLiteral("no such track"));
        rig.settle();
        QVERIFY(!rig.player.isPlaying());
        QVERIFY(rig.core.log.contains(QStringLiteral("pause")));
        QCOMPARE(rig.errors.message(), QStringLiteral("no such track"));
    }

    void aReplacedLoadIsAskedForAgainWhileItsSongIsWanted()
    {
        Rig rig;
        const QList<Song> queue = makeQueue(2);
        rig.player.play(queue[0], queue);
        // something other than a song, a reconnect for a download, replaced
        // the request in the core before it made any sound
        rig.settle();
        rig.core.take(QStringLiteral("playFile"))->fail(QString::fromLatin1(SWIFTY_CORE_SUPERSEDED));
        rig.settle();
        QVERIFY(rig.player.isLoading());
        QVERIFY(rig.errors.message().isEmpty());
        // the song is still the one wanted, so it is asked for again
        rig.finishLoading();
        QVERIFY(rig.player.isPlaying());
        QCOMPARE(rig.player.currentSong().uri, queue[0].uri);
    }

    void seekingNeverStartsSound()
    {
        Rig rig;
        const QList<Song> queue = makeQueue(1);
        rig.player.play(queue[0], queue);
        // ignored while the song is still loading
        rig.player.seek(10'000);
        QVERIFY(!rig.core.log.contains(QStringLiteral("seek 10000")));
        rig.finishLoading();
        rig.player.pause();
        rig.core.duration = 200'000;
        rig.player.seek(30'000);
        QVERIFY(rig.core.log.contains(QStringLiteral("seek 30000")));
        QVERIFY(!rig.core.sounding);
        QVERIFY(!rig.player.isPlaying());
    }

    void theEndOfTheQueueStopsAndPlayStartsOver()
    {
        Rig rig;
        const QList<Song> queue = makeQueue(1);
        rig.player.play(queue[0], queue);
        rig.finishLoading();
        rig.core.duration = 200'000;
        rig.core.position = 199'500;
        rig.core.endOfTrack = true;
        // the player notices at its next poll
        QTRY_VERIFY_WITH_TIMEOUT(!rig.player.isPlaying(), 2000);
        QVERIFY(!rig.core.sounding);

        // scrubbing a finished song stays silent
        rig.player.seek(50'000);
        QVERIFY(!rig.core.sounding);
        // play loads it again from the start
        rig.player.resume();
        QVERIFY(rig.core.take(QStringLiteral("playFile")));
    }

    void aStaleEndOfTrackDoesNotSkipMidSong()
    {
        Rig rig;
        const QList<Song> queue = makeQueue(2);
        rig.player.play(queue[0], queue);
        rig.finishLoading();
        rig.core.duration = 200'000;
        rig.core.position = 60'000;
        rig.core.endOfTrack = true;
        QTest::qWait(400);
        QVERIFY(rig.player.currentSong().isSameInstance(queue[0]));
        QVERIFY(rig.player.isPlaying());
    }

    void aLostDeviceStopsAndRebuildsOnlyOnPlay()
    {
        Rig rig;
        const QList<Song> queue = makeQueue(1);
        rig.player.play(queue[0], queue);
        rig.finishLoading();
        rig.core.duration = 200'000;
        rig.core.position = 42'000;
        // the player has read where the song is, and then the sound server says the device is gone
        QTRY_COMPARE_WITH_TIMEOUT(rig.player.livePositionMs(), quint32(42'000), 2000);
        rig.player.invalidateAudioOutput();
        QVERIFY(!rig.player.isPlaying());
        QVERIFY(!rig.core.sounding);
        QVERIFY(rig.core.pending.isEmpty() || !rig.core.take(QStringLiteral("restartAudioOutput")));

        rig.player.resume();
        rig.core.take(QStringLiteral("restartAudioOutput"))->succeed();
        rig.settle();
        rig.finishLoading();
        QVERIFY(rig.core.log.contains(QStringLiteral("seek 42000")));
        QVERIFY(rig.player.isPlaying());
    }

    void aStreamThatOnlyBrokeIsOpenedAnewAndPlaysOn()
    {
#ifndef Q_OS_LINUX
        QSKIP("only on linux, where the sound server reports lost devices, is a broken stream opened anew");
#endif
        Rig rig;
        const QList<Song> queue = makeQueue(1);
        rig.player.play(queue[0], queue);
        rig.finishLoading();
        rig.core.duration = 200'000;
        rig.core.position = 42'000;
        // the output fails, and no device is reported lost
        rig.core.stalled = true;
        const auto restartAsked = [&rig] {
            for (const auto &call : rig.core.pending) {
                if (call->name == QStringLiteral("restartAudioOutput")) {
                    return true;
                }
            }
            return false;
        };
        QTRY_VERIFY_WITH_TIMEOUT(restartAsked(), 4000);
        rig.core.take(QStringLiteral("restartAudioOutput"))->succeed();
        rig.settle();
        rig.finishLoading();
        QVERIFY(rig.core.log.contains(QStringLiteral("seek 42000")));
        QVERIFY(rig.player.isPlaying());
    }

    void whatWasPlayingComesBackPausedWhereItWasLeft()
    {
        const QList<Song> queue = makeQueue(4);
        Rig first;
        first.player.play(queue[2], queue);
        first.finishLoading();
        first.core.duration = 200'000;
        first.core.position = 90'000;
        QTRY_COMPARE_WITH_TIMEOUT(first.player.livePositionMs(), quint32(90'000), 2000);
        SessionStore saved(&first.settings, &first.player, [&queue] { return queue; });
        saved.save();

        // another start of the app, with the same settings
        Rig second;
        second.settings.setValue(QStringLiteral("session"), first.settings.value(QStringLiteral("session")));
        SessionStore restoring(&second.settings, &second.player, [&queue] { return queue; });
        QVERIFY(restoring.restore());
        QVERIFY(second.player.hasSong());
        QCOMPARE(second.player.currentSong().uri, queue[2].uri);
        QCOMPARE(second.player.livePositionMs(), quint32(90'000));
        QCOMPARE(second.player.queueSongs().size(), 4);
        QCOMPARE(second.player.upNextSongs().size(), 1);
        // nothing sounds until play is pressed
        QVERIFY(!second.player.isPlaying());
        QVERIFY(second.core.pending.isEmpty());

        // and then it starts where it was
        second.player.togglePlayback();
        second.finishLoading();
        QVERIFY(second.player.isPlaying());
        QVERIFY(second.core.log.contains(QStringLiteral("seek 90000")));

        // nothing to restore twice, or over what is playing
        QVERIFY(!restoring.restore());
    }

    void aSongLeftInItsLastSecondsStartsOver()
    {
        const QList<Song> queue = makeQueue(2);
        Rig first;
        first.player.play(queue[0], queue);
        first.finishLoading();
        first.core.duration = 200'000;
        first.core.position = 198'000;
        QTRY_COMPARE_WITH_TIMEOUT(first.player.livePositionMs(), quint32(198'000), 2000);
        SessionStore saved(&first.settings, &first.player, [&queue] { return queue; });
        saved.save();

        Rig second;
        second.settings.setValue(QStringLiteral("session"), first.settings.value(QStringLiteral("session")));
        SessionStore restoring(&second.settings, &second.player, [&queue] { return queue; });
        QVERIFY(restoring.restore());
        QCOMPARE(second.player.livePositionMs(), quint32(0));
    }

    void sleepPausesAPlayingSong()
    {
        Rig rig;
        const QList<Song> queue = makeQueue(1);
        rig.player.play(queue[0], queue);
        rig.finishLoading();
        rig.player.prepareForSleep();
        QVERIFY(!rig.player.isPlaying());
        QVERIFY(!rig.core.sounding);
    }

    void aSongOutsideTheQueuePlaysAlone()
    {
        Rig rig;
        const QList<Song> queue = makeQueue(3);
        const Song stranger = makeSong(QStringLiteral("song4"));
        rig.player.play(stranger, queue);
        QCOMPARE(rig.player.activeQueue().size(), 1);
    }

    void shufflePutsTheCurrentSongFirstAndClearsHistory()
    {
        Rig rig;
        const QList<Song> queue = makeQueue(5);
        rig.player.play(queue[2], queue);
        rig.finishLoading();
        rig.player.skipForward();
        rig.finishLoading();
        QCOMPARE(rig.player.previouslyPlayedSongs().size(), 3);

        rig.player.toggleShuffle();
        QVERIFY(rig.player.activeQueue().first().isSameInstance(queue[3]));
        QCOMPARE(rig.player.activeQueue().size(), 5);
        QVERIFY(rig.player.previouslyPlayedSongs().isEmpty());
    }

    void playingFromHistoryTrimsIt()
    {
        Rig rig;
        const QList<Song> queue = makeQueue(4);
        rig.player.toggleShuffle();
        rig.player.play(queue[0], queue);
        rig.finishLoading();
        rig.player.skipForward();
        rig.finishLoading();
        rig.player.skipForward();
        rig.finishLoading();
        const QList<Song> history = rig.player.previouslyPlayedSongs();
        QCOMPARE(history.size(), 2);
        rig.player.playFromQueue(history.first());
        QVERIFY(rig.player.previouslyPlayedSongs().isEmpty() || !rig.player.previouslyPlayedSongs().first().isSameInstance(history.first()));
    }

    void playNextAndPlayLastDoNotDuplicate()
    {
        Rig rig;
        const QList<Song> queue = makeQueue(3);
        rig.player.play(queue[0], queue);
        rig.finishLoading();
        const Song again = queue[2].freshInstance();
        rig.player.playNext(again);
        QList<Song> upNext = rig.player.upNextSongs();
        QCOMPARE(upNext.size(), 2);
        QVERIFY(upNext.first().isSameInstance(again));
        rig.player.playLast(queue[1].freshInstance());
        upNext = rig.player.upNextSongs();
        QCOMPARE(upNext.size(), 2);
        QCOMPARE(upNext.last().uri, queue[1].uri);
    }

    void backRestartsAfterThreeSeconds()
    {
        Rig rig;
        const QList<Song> queue = makeQueue(2);
        rig.player.play(queue[1], queue);
        rig.finishLoading();
        rig.core.duration = 200'000;
        rig.core.position = 10'000;
        QTRY_COMPARE_WITH_TIMEOUT(rig.player.livePositionMs(), 10'000u, 2000);
        rig.player.skipBackward();
        QVERIFY(rig.core.log.contains(QStringLiteral("seek 0")));
        QVERIFY(rig.player.currentSong().isSameInstance(queue[1]));

        rig.core.position = 1'000;
        rig.player.seek(1'000);
        rig.player.skipBackward();
        QVERIFY(rig.player.currentSong().isSameInstance(queue[0]));
    }

    void muteRemembersTheVolume()
    {
        Rig rig;
        rig.player.setVolume(0.4);
        rig.player.toggleMute();
        QCOMPARE(rig.player.volume(), 0.0);
        QCOMPARE(rig.core.volumeValue, 0.0f);
        rig.player.toggleMute();
        QCOMPARE(rig.player.volume(), 0.4);
        QCOMPARE(rig.settings.value(QStringLiteral("playerVolume")).toDouble(), 0.4);
    }

    void theSleepTimerPausesAndRestoresTheVolume()
    {
        Rig rig;
        const QList<Song> queue = makeQueue(1);
        rig.player.setVolume(0.8);
        rig.player.play(queue[0], queue);
        rig.finishLoading();
        rig.player.startSleepTimer(1, QStringLiteral("entire"));
        QVERIFY(rig.player.isSleepTimerRunning());
        QVERIFY(rig.player.isVolumeManagedBySleepTimer());
        QTRY_VERIFY_WITH_TIMEOUT(!rig.player.isSleepTimerRunning(), 3000);
        QVERIFY(!rig.player.isPlaying());
        QCOMPARE(rig.player.volume(), 0.8);
    }

    void cancellingTheSleepTimerRestoresTheVolume()
    {
        Rig rig;
        rig.player.setVolume(0.6);
        rig.player.startSleepTimer(60, QStringLiteral("entire"));
        QTest::qWait(300);
        QVERIFY(rig.player.volume() < 0.6);
        rig.player.cancelSleepTimer();
        QCOMPARE(rig.player.volume(), 0.6);
    }

    void sleepFadeLevels()
    {
        QCOMPARE(sleepFadeLevel(SleepFade::None, 1, 60), 1.0);
        QCOMPARE(sleepFadeLevel(SleepFade::FinalSeconds, 20, 60), 1.0);
        QCOMPARE(sleepFadeLevel(SleepFade::FinalSeconds, 5, 60), 0.5);
        QCOMPARE(sleepFadeLevel(SleepFade::FinalSeconds, 2, 4), 0.5);
        QCOMPARE(sleepFadeLevel(SleepFade::Entire, 30, 60), 0.5);
        QCOMPARE(sleepFadeLevel(SleepFade::Entire, 0, 60), 0.0);
    }

    void lyricsFollowThePosition()
    {
        Rig rig;
        // a file with a .lrc beside it
        Song song = makeSong(QStringLiteral("sung"));
        song.uri = QUrl::fromLocalFile(rig.directory.filePath(QStringLiteral("sung.flac"))).toString();
        QFile lyrics(rig.directory.filePath(QStringLiteral("sung.lrc")));
        QVERIFY(lyrics.open(QIODevice::WriteOnly));
        lyrics.write("[00:01.00]one\n[00:05.00]two\n");
        lyrics.close();
        rig.player.play(song, {song});
        rig.settle();
        QVERIFY(rig.player.hasLyrics());
        QCOMPARE(rig.player.activeLyricIndex(), -1);
        rig.finishLoading();
        rig.core.duration = 200'000;
        rig.core.position = 5'500;
        QTRY_COMPARE_WITH_TIMEOUT(rig.player.activeLyricIndex(), 1, 2000);
    }

    void missingLyricsShowThePhrase()
    {
        Rig rig;
        const QList<Song> queue = makeQueue(1);
        rig.player.play(queue[0], queue);
        rig.settle();
        QVERIFY(!rig.player.hasLyrics());
        QCOMPARE(rig.player.lyricsMessage(), QStringLiteral("i mean i guess bro"));
    }

    void aScrubOnlyCommitsForTheSameSong()
    {
        ScrubSession session;
        session.begin(7);
        QVERIFY(session.acceptsChanges(7));
        QCOMPARE(session.finish(7, 0.5), 0.5);
        session.begin(7);
        QCOMPARE(session.finish(8, 0.5), -1.0);
        session.begin(7);
        session.cancel();
        QCOMPARE(session.finish(7, 0.5), -1.0);
        QVERIFY(!session.isEditing());
    }

    /// debugging: a song that loads for ever, for the visualizers' loading
    /// animation, asking the core for nothing
    void theLoadingTestLoadsUntilWithdrawn()
    {
        Rig rig;
        rig.player.playLoadingTest();
        QTest::qWait(700);
        QVERIFY(rig.player.hasSong());
        QCOMPARE(rig.player.currentSong().name, QStringLiteral("Loading Test"));
        QVERIFY(rig.player.isLoading());
        QVERIFY(!rig.player.isPlaying());
        QVERIFY(!rig.player.isLoadingLyrics() || !rig.core.take(QStringLiteral("lyrics")));
        for (const QString &call : {QStringLiteral("playTrack"), QStringLiteral("playFile"),
                                    QStringLiteral("connect"), QStringLiteral("lyrics")}) {
            QVERIFY2(!rig.core.take(call), qPrintable(call));
        }

        // paused, it stops loading; played again, it loads again
        rig.player.pause();
        QVERIFY(!rig.player.isLoading());
        rig.player.togglePlayback();
        Rig::settle();
        QVERIFY(rig.player.isLoading());

        // a real song takes its place
        const QList<Song> queue = makeQueue(1);
        rig.player.play(queue[0], queue);
        rig.finishLoading();
        QVERIFY(!rig.player.isLoading());
        QVERIFY(rig.player.isPlaying());
        QCOMPARE(rig.player.currentSong().name, queue[0].name);
    }
};

QTEST_GUILESS_MAIN(PlaybackTest)
#include "tst_playback.moc"
