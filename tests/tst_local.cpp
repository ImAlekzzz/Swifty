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
#include "library/LibraryStore.h"
#include "library/LocalLibrary.h"
#include "playback/Lyrics.h"
#include "playback/PlaybackController.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

#include <utility>

using namespace swifty;

namespace {

/// half a second of silence as a 16-bit wav, its tags in a LIST INFO chunk
/// ahead of the data, where readers look for them
void writeTaggedWav(const QString &path, const QList<std::pair<QByteArray, QByteArray>> &tags)
{
    const auto le32 = [](quint32 value) {
        QByteArray bytes(4, '\0');
        qToLittleEndian(value, bytes.data());
        return bytes;
    };
    const auto le16 = [](quint16 value) {
        QByteArray bytes(2, '\0');
        qToLittleEndian(value, bytes.data());
        return bytes;
    };
    QByteArray info = "INFO";
    for (const auto &[id, text] : tags) {
        QByteArray value = text + '\0';
        if (value.size() % 2) {
            value += '\0';
        }
        info += id + le32(quint32(value.size())) + value;
    }
    const quint32 frames = 22'050;
    const QByteArray format = "fmt " + le32(16) + le16(1) + le16(2) + le32(44'100) + le32(44'100 * 4) + le16(4) + le16(16);
    const QByteArray list = "LIST" + le32(quint32(info.size())) + info;
    const QByteArray data = "data" + le32(frames * 4) + QByteArray(frames * 4, '\0');
    const QByteArray body = "WAVE" + format + list + data;
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("RIFF" + le32(quint32(body.size())) + body);
}

/// three tagged songs in a folder of their own, searched by `library`
QStringList addThreeSongs(LocalLibrary &library, const QTemporaryDir &music)
{
    QStringList uris;
    const char *names[] = {"Avalon", "Bluebird", "Comet"};
    for (int index = 0; index < 3; ++index) {
        const QString path = music.filePath(QStringLiteral("%1.wav").arg(index + 1));
        writeTaggedWav(path, {{"INAM", names[index]}, {"IART", "Someone"}, {"IPRD", "Album"},
                              {"IPRT", QByteArray::number(index + 1)}});
        uris.append(QUrl::fromLocalFile(QFileInfo(path).canonicalFilePath()).toString());
    }
    QSignalSpy scanned(&library, &LocalLibrary::scanned);
    library.addSources({QUrl::fromLocalFile(music.path())});
    // the search can be done before anyone waits for it: it is the signal that counts
    for (int waited = 0; scanned.isEmpty() && waited < 5000; waited += 20) {
        QTest::qWait(20);
    }
    if (scanned.isEmpty()) {
        return {};
    }
    return uris;
}

/// whether two lists hold the very same songs, each the same occurrence
bool sameInstances(const QList<Song> &left, const QList<Song> &right)
{
    if (left.size() != right.size()) {
        return false;
    }
    for (qsizetype index = 0; index < left.size(); ++index) {
        if (left.at(index).instanceId != right.at(index).instanceId) {
            return false;
        }
    }
    return true;
}

Song songWithUri(const LocalLibrary &library, const QString &uri)
{
    for (const Song &song : library.songs()->songs()) {
        if (song.uri == uri) {
            return song;
        }
    }
    return {};
}

QStringList namesOf(const QList<Song> &songs)
{
    QStringList names;
    for (const Song &song : songs) {
        names.append(song.name);
    }
    return names;
}

} // namespace

/// music files of one's own: found in the folders added, named by their own
/// tags (or their file when they have none), played from disk, with lyrics
/// from a .lrc beside them
class LocalTest : public QObject {
    Q_OBJECT

private slots:
    void searchFindsASongByItsGenre()
    {
        Song song;
        song.name = QStringLiteral("Track");
        song.artists = QStringLiteral("Someone");
        song.albumName = QStringLiteral("Record");
        song.genre = QStringLiteral("Hip-Hop/Rap");

        QVERIFY(LibraryStore::songMatches(song, QStringLiteral("Hip-Hop")));
        QVERIFY(LibraryStore::songMatches(song, QStringLiteral("hip-hop")));
        QVERIFY(LibraryStore::songMatches(song, QStringLiteral("HIP-HOP")));
        // only the punctuation differs
        QVERIFY(LibraryStore::songMatches(song, QStringLiteral("hip hop")));
        QVERIFY(LibraryStore::songMatches(song, QStringLiteral("hiphop")));
        QVERIFY(LibraryStore::songMatches(song, QStringLiteral("rap")));
        QVERIFY(!LibraryStore::songMatches(song, QStringLiteral("jazz")));
        // the other fields still answer
        QVERIFY(LibraryStore::songMatches(song, QStringLiteral("someone")));
        QVERIFY(LibraryStore::songMatches(song, QStringLiteral("RECORD")));

        // and the genre is kept with the song
        QCOMPARE(Song::fromJson(song.toJson()).genre, QStringLiteral("Hip-Hop/Rap"));
        Song plain;
        plain.name = QStringLiteral("No genre");
        QVERIFY(!LibraryStore::songMatches(plain, QStringLiteral("hip-hop")));
    }

    void aFolderIsSearchedAndEachSongTakesItsOwnTags()
    {
        QTemporaryDir music;
        QTemporaryDir storage;
        QDir(music.path()).mkpath(QStringLiteral("Diamond Life"));
        writeTaggedWav(music.filePath(QStringLiteral("Diamond Life/02 b.wav")),
                       {{"INAM", "Your Love Is King"}, {"IART", "Sade"}, {"IPRD", "Diamond Life"}, {"IPRT", "2"}});
        writeTaggedWav(music.filePath(QStringLiteral("Diamond Life/01 a.wav")),
                       {{"INAM", "Smooth Operator"}, {"IART", "Sade"}, {"IPRD", "Diamond Life"}, {"IPRT", "1"}});
        // no tags at all: named after its file, its album after its folder
        writeTaggedWav(music.filePath(QStringLiteral("Diamond Life/hidden track.wav")), {});
        // not music, and not looked at
        QFile notes(music.filePath(QStringLiteral("Diamond Life/notes.txt")));
        QVERIFY(notes.open(QIODevice::WriteOnly));
        notes.write("liner notes");
        notes.close();

        Settings settings(storage.filePath(QStringLiteral("swifty.conf")));
        LocalLibrary library(&settings, nullptr, storage.path());
        QSignalSpy scanned(&library, &LocalLibrary::scanned);
        QCOMPARE(library.addSources({QUrl::fromLocalFile(music.path())}), 1);
        QTRY_VERIFY_WITH_TIMEOUT(scanned.count() > 0, 5000);
        QCOMPARE(library.songs()->count(), 3);

        // by artist and album, in the album's own order
        const QList<Song> songs = library.songs()->songs();
        const auto byName = [&](const QString &name) {
            return *std::find_if(songs.begin(), songs.end(), [&](const Song &song) { return song.name == name; });
        };
        QCOMPARE(songs.at(0).name, QStringLiteral("Smooth Operator"));
        QCOMPARE(songs.at(1).name, QStringLiteral("Your Love Is King"));
        QCOMPARE(songs.at(0).artists, QStringLiteral("Sade"));
        QCOMPARE(songs.at(0).albumName, QStringLiteral("Diamond Life"));
        QVERIFY(songs.at(0).isLocalFile());
        QVERIFY(std::abs(songs.at(0).durationMs - 500) <= 10);
        QCOMPARE(songs.at(0).localPath(), QFileInfo(music.filePath(QStringLiteral("Diamond Life/01 a.wav"))).canonicalFilePath());
        const Song untagged = byName(QStringLiteral("hidden track"));
        QCOMPARE(untagged.artists, QStringLiteral("Unknown Artist"));
        QCOMPARE(untagged.albumName, QStringLiteral("Diamond Life"));

        // the same folder again is not added twice, nor a file inside it
        QCOMPARE(library.addSources({QUrl::fromLocalFile(music.path())}), 0);
        QCOMPARE(library.addSources({QUrl::fromLocalFile(music.filePath(QStringLiteral("Diamond Life/01 a.wav")))}), 0);

        // what the tags said is kept: a second library shows it at once
        LocalLibrary again(&settings, nullptr, storage.path());
        QCOMPARE(again.songs()->count(), 3);
        QVERIFY(again.isScanning());
        QSignalSpy againScanned(&again, &LocalLibrary::scanned);
        QTRY_VERIFY_WITH_TIMEOUT(againScanned.count() > 0, 5000);
        QCOMPARE(again.songs()->count(), 3);

        // and removing the folder takes its songs away
        library.removeSource(library.sources().first());
        QCOMPARE(library.songs()->count(), 0);
        QVERIFY(library.sources().isEmpty());
    }

    void playlistsHoldThePathsOfTheirSongsAndCopyNothing()
    {
        QTemporaryDir music;
        QTemporaryDir storage;
        QTemporaryDir elsewhere;
        Settings settings(storage.filePath(QStringLiteral("swifty.conf")));
        LocalLibrary library(&settings, nullptr, storage.path());
        const QStringList uris = addThreeSongs(library, music);
        QCOMPARE(uris.size(), 3);
        QSignalSpy changed(&library, &LocalLibrary::playlistsChanged);
        QSignalSpy added(&library, &LocalLibrary::tracksAdded);

        // something that is not a file cannot go in, only files
        Song notAFile;
        notAFile.uri = QStringLiteral("https://example.org/song.mp3");
        const Playlist made = library.createPlaylist(QStringLiteral("  Night Drive "), QStringLiteral("for the road"),
                                                     QByteArray("a cover"), {songWithUri(library, uris[0]), notAFile});
        QVERIFY(made.isValid());
        QVERIFY(made.isLocal());
        QCOMPARE(made.name, QStringLiteral("Night Drive"));
        QCOMPARE(made.ownerDisplayName(), QStringLiteral("Local Files"));
        QVERIFY(QFileInfo::exists(made.artworkUrl.toLocalFile()));
        QCOMPARE(library.playlists()->count(), 1);
        QCOMPARE(library.playlistChoices().size(), 1);
        QVERIFY(changed.count() > 0);
        QVERIFY(!library.createPlaylist(QStringLiteral("  "), {}, {}, {}).isValid());
        QCOMPARE(library.playlist(made.id).songCount, 1);

        // the song is the file it is, where it is, and nothing is copied
        const Song first = library.songsOf(made.id).value(0);
        QCOMPARE(first.name, QStringLiteral("Avalon"));
        QCOMPARE(first.uri, uris[0]);
        QVERIFY(!QFileInfo::exists(storage.filePath(QStringLiteral("local-playlist-tracks"))));

        // files from anywhere, and a folder's, in the order asked for; what is
        // not music is said so. they join the library, where songs are known from
        writeTaggedWav(elsewhere.filePath(QStringLiteral("Dawn.wav")), {{"INAM", "Dawn"}});
        QVERIFY(QDir(elsewhere.path()).mkpath(QStringLiteral("Album")));
        writeTaggedWav(elsewhere.filePath(QStringLiteral("Album/10.wav")), {{"INAM", "Foxtrot"}});
        writeTaggedWav(elsewhere.filePath(QStringLiteral("Album/2.wav")), {{"INAM", "Echo"}});
        QFile notes(elsewhere.filePath(QStringLiteral("notes.txt")));
        QVERIFY(notes.open(QIODevice::WriteOnly));
        notes.close();
        added.clear();
        library.addFiles(made.id, {QUrl::fromLocalFile(elsewhere.filePath(QStringLiteral("Dawn.wav"))),
                                   QUrl::fromLocalFile(elsewhere.filePath(QStringLiteral("Album"))),
                                   QUrl::fromLocalFile(elsewhere.filePath(QStringLiteral("notes.txt")))});
        // nothing is copied, so it is told at once
        QCOMPARE(added.count(), 1);
        QCOMPARE(added.first().at(1).toInt(), 3);
        QVERIFY(added.first().at(2).toString().contains(QStringLiteral("notes.txt")));
        QCOMPARE(library.sources().size(), 3);
        QTRY_COMPARE(namesOf(library.songsOf(made.id)), (QStringList {"Avalon", "Dawn", "Echo", "Foxtrot"}));
        QVERIFY(!QFileInfo::exists(storage.filePath(QStringLiteral("local-playlist-tracks"))));

        // the same song again goes in again, and two asked for at once land in the order asked
        QCOMPARE(library.addSongs(made.id, {songWithUri(library, uris[0])}), 1);
        QCOMPARE(library.addSongs(made.id, {songWithUri(library, uris[1])}), 1);
        QList<Song> songs = library.songsOf(made.id);
        QCOMPARE(namesOf(songs), (QStringList {"Avalon", "Dawn", "Echo", "Foxtrot", "Avalon", "Bluebird"}));
        QCOMPARE(songs[0].uri, songs[4].uri);
        // each its own occurrence
        QVERIFY(songs[0].instanceId != songs[4].instanceId);

        // a file that goes away leaves the playlist while it is gone, and is back with the file
        const QString dawn = elsewhere.filePath(QStringLiteral("Dawn.wav"));
        QVERIFY(QFile::rename(dawn, dawn + QStringLiteral(".away")));
        QSignalSpy scanned(&library, &LocalLibrary::scanned);
        library.rescan();
        QTRY_VERIFY_WITH_TIMEOUT(scanned.count() > 0, 5000);
        QCOMPARE(namesOf(library.songsOf(made.id)), (QStringList {"Avalon", "Echo", "Foxtrot", "Avalon", "Bluebird"}));
        QVERIFY(QFile::rename(dawn + QStringLiteral(".away"), dawn));
        scanned.clear();
        library.rescan();
        QTRY_VERIFY_WITH_TIMEOUT(scanned.count() > 0, 5000);
        QCOMPARE(namesOf(library.songsOf(made.id)),
                 (QStringList {"Avalon", "Dawn", "Echo", "Foxtrot", "Avalon", "Bluebird"}));

        QStringList held;
        for (const Song &song : library.songsOf(made.id)) {
            held.append(song.uri);
        }
        library.reorder(made.id, {held[3], held[0], held[1], held[2], held[4], held[5]});
        QCOMPARE(namesOf(library.songsOf(made.id)),
                 (QStringList {"Foxtrot", "Avalon", "Dawn", "Echo", "Avalon", "Bluebird"}));
        // both of a song go, and the file stays
        QCOMPARE(library.removeSong(made.id, held[0]), 2);
        QCOMPARE(namesOf(library.songsOf(made.id)), (QStringList {"Foxtrot", "Dawn", "Echo", "Bluebird"}));
        QVERIFY(QFileInfo::exists(QUrl(held[0]).toLocalFile()));

        // kept: another library made from the same place has it at once
        {
            LocalLibrary again(&settings, nullptr, storage.path());
            QCOMPARE(again.playlists()->count(), 1);
            QCOMPARE(namesOf(again.songsOf(made.id)), (QStringList {"Foxtrot", "Dawn", "Echo", "Bluebird"}));
            QCOMPARE(again.playlist(made.id).description, QStringLiteral("for the road"));
            QSignalSpy againScanned(&again, &LocalLibrary::scanned);
            QTRY_VERIFY_WITH_TIMEOUT(againScanned.count() > 0, 5000);
            QCOMPARE(namesOf(again.songsOf(made.id)), (QStringList {"Foxtrot", "Dawn", "Echo", "Bluebird"}));
        }

        // a new cover takes the old one's place, under a new name
        const QString oldCover = library.playlist(made.id).artworkUrl.toLocalFile();
        QVERIFY(library.updatePlaylist(made.id, QStringLiteral("Late Drive"), {}, QByteArray("another cover")));
        const Playlist updated = library.playlist(made.id);
        QCOMPARE(updated.name, QStringLiteral("Late Drive"));
        QVERIFY(updated.description.isEmpty());
        QVERIFY(updated.artworkUrl.toLocalFile() != oldCover);
        QVERIFY(!QFileInfo::exists(oldCover));
        QVERIFY(QFileInfo::exists(updated.artworkUrl.toLocalFile()));
        // and no cover leaves it alone
        QVERIFY(library.updatePlaylist(made.id, QStringLiteral("Late Drive"), {}, {}));
        QCOMPARE(library.playlist(made.id).artworkUrl, updated.artworkUrl);

        // deleted, the music is untouched
        QVERIFY(library.deletePlaylist(made.id));
        QCOMPARE(library.playlists()->count(), 0);
        QVERIFY(!QFileInfo::exists(updated.artworkUrl.toLocalFile()));
        QVERIFY(QFileInfo::exists(QUrl(uris[0]).toLocalFile()));
        LocalLibrary afterwards(&settings, nullptr, storage.path());
        QCOMPARE(afterwards.playlists()->count(), 0);
        QSignalSpy afterwardsScanned(&afterwards, &LocalLibrary::scanned);
        QTRY_VERIFY_WITH_TIMEOUT(afterwardsScanned.count() > 0, 5000);
    }

    /// a playlist is a list of paths, so one written as a list of paths works as it is
    void aListOfPathsIsAPlaylist()
    {
        QTemporaryDir music;
        QTemporaryDir storage;
        Settings settings(storage.filePath(QStringLiteral("swifty.conf")));
        QStringList uris;
        {
            LocalLibrary library(&settings, nullptr, storage.path());
            uris = addThreeSongs(library, music);
        }
        QCOMPARE(uris.size(), 3);
        QFile kept(storage.filePath(QStringLiteral("local-playlists.json")));
        QVERIFY(kept.open(QIODevice::WriteOnly));
        kept.write(QJsonDocument(QJsonObject {
                                     {QStringLiteral("version"), 1},
                                     {QStringLiteral("playlists"),
                                      QJsonArray {QJsonObject {{QStringLiteral("id"), QStringLiteral("local:old")},
                                                               {QStringLiteral("name"), QStringLiteral("Old")},
                                                               {QStringLiteral("songs"),
                                                                QJsonArray {uris[2], uris[0]}}}}},
                                 })
                       .toJson());
        kept.close();

        LocalLibrary library(&settings, nullptr, storage.path());
        const QList<Song> songs = library.songsOf(QStringLiteral("local:old"));
        QCOMPARE(namesOf(songs), (QStringList {"Comet", "Avalon"}));
        QCOMPARE(songs[0].uri, uris[2]);
        QCOMPARE(songs[1].uri, uris[0]);
        QVERIFY(!QFileInfo::exists(storage.filePath(QStringLiteral("local-playlist-tracks"))));
        QTRY_VERIFY(!library.isScanning());
    }

    /// playlists made by an older version held copies of their files: one whose
    /// original is still there goes back onto it and the copy is let go; one
    /// whose original is gone keeps its copy, and plays
    void playlistsMadeWithCopiesGoBackToTheirOriginals()
    {
        QTemporaryDir music;
        QTemporaryDir storage;
        Settings settings(storage.filePath(QStringLiteral("swifty.conf")));
        QStringList uris;
        {
            LocalLibrary library(&settings, nullptr, storage.path());
            uris = addThreeSongs(library, music);
        }
        QCOMPARE(uris.size(), 3);
        const QString store = storage.filePath(QStringLiteral("local-playlist-tracks"));
        const QString liveCopy = store + QStringLiteral("/aaaaaaaaaaaa/music/Avalon.wav");
        const QString goneCopy = store + QStringLiteral("/bbbbbbbbbbbb/music/Bluebird.wav");
        QVERIFY(QDir().mkpath(QFileInfo(liveCopy).absolutePath()));
        QVERIFY(QDir().mkpath(QFileInfo(goneCopy).absolutePath()));
        QVERIFY(QFile::copy(QUrl(uris[0]).toLocalFile(), liveCopy));
        QVERIFY(QFile::copy(QUrl(uris[1]).toLocalFile(), goneCopy));
        const auto playlist = [](const QString &id, const QString &name, const QString &copy) {
            return QJsonObject {{QStringLiteral("id"), id},
                                {QStringLiteral("name"), name},
                                {QStringLiteral("songs"), QJsonArray {QUrl::fromLocalFile(copy).toString()}}};
        };
        const auto origin = [](const QString &copy, const QString &source) {
            return QJsonObject {{QStringLiteral("copy"), copy},
                                {QStringLiteral("source"), source},
                                {QStringLiteral("size"), 0},
                                {QStringLiteral("modified"), 0}};
        };
        QFile kept(storage.filePath(QStringLiteral("local-playlists.json")));
        QVERIFY(kept.open(QIODevice::WriteOnly));
        kept.write(QJsonDocument(QJsonObject {
                                     {QStringLiteral("version"), 1},
                                     {QStringLiteral("playlists"),
                                      QJsonArray {playlist(QStringLiteral("local:live"), QStringLiteral("Live"), liveCopy),
                                                  playlist(QStringLiteral("local:gone"), QStringLiteral("Gone"), goneCopy)}},
                                     {QStringLiteral("copies"),
                                      QJsonArray {origin(liveCopy, QUrl(uris[0]).toLocalFile()),
                                                  origin(goneCopy, music.filePath(QStringLiteral("gone/Bluebird.wav")))}},
                                 })
                       .toJson());
        kept.close();

        LocalLibrary library(&settings, nullptr, storage.path());
        // the one with its original is on it again, and its copy is gone
        const QList<Song> live = library.songsOf(QStringLiteral("local:live"));
        QCOMPARE(namesOf(live), (QStringList {"Avalon"}));
        QCOMPARE(live[0].uri, uris[0]);
        QVERIFY(!QFileInfo::exists(liveCopy));
        // the one without keeps its copy, which plays
        QTRY_COMPARE(namesOf(library.songsOf(QStringLiteral("local:gone"))), (QStringList {"Bluebird"}));
        QVERIFY(QFileInfo::exists(goneCopy));
        QTRY_VERIFY(!library.isScanning());
    }

    /// a big folder is read on several threads at once: every song still comes out
    /// once, with its own tags, and a second search finds the same without reading
    void aBigFolderIsReadSideBySide()
    {
        QTemporaryDir music;
        QTemporaryDir storage;
        constexpr int kSongs = 60;
        for (int index = 0; index < kSongs; ++index) {
            // folders of ten, so there is more than one to walk
            const QString folder = music.filePath(QStringLiteral("disc%1").arg(index / 10));
            QVERIFY(QDir().mkpath(folder));
            writeTaggedWav(folder + QStringLiteral("/%1.wav").arg(index),
                           {{"INAM", QByteArray("Song ") + QByteArray::number(index)}, {"IART", "Someone"}, {"IPRD", "Album"},
                            {"IPRT", QByteArray::number(index)}});
        }
        Settings settings(storage.filePath(QStringLiteral("swifty.conf")));
        {
            LocalLibrary library(&settings, nullptr, storage.path());
            QSignalSpy scanned(&library, &LocalLibrary::scanned);
            library.addSources({QUrl::fromLocalFile(music.path())});
            QTRY_VERIFY_WITH_TIMEOUT(scanned.count() > 0, 20000);
            QCOMPARE(library.songs()->count(), kSongs);
            QStringList names = namesOf(library.songs()->songs());
            names.sort();
            QStringList wanted;
            for (int index = 0; index < kSongs; ++index) {
                wanted.append(QStringLiteral("Song %1").arg(index));
            }
            wanted.sort();
            QCOMPARE(names, wanted);
            for (const Song &song : library.songs()->songs()) {
                QCOMPARE(song.artists, QStringLiteral("Someone"));
                QCOMPARE(song.albumName, QStringLiteral("Album"));
            }
        }
        // the same folder again: nothing changed, nothing is lost, and what was
        // kept is not written again
        const QFileInfo cache(storage.filePath(QStringLiteral("local-files.db")));
        QVERIFY(cache.exists());
        const QDateTime written = cache.lastModified();
        QTest::qWait(1100);
        LocalLibrary again(&settings, nullptr, storage.path());
        QSignalSpy scannedAgain(&again, &LocalLibrary::scanned);
        again.rescan();
        QTRY_VERIFY_WITH_TIMEOUT(scannedAgain.count() > 0, 20000);
        QCOMPARE(again.songs()->count(), kSongs);
        QCOMPARE(QFileInfo(cache.filePath()).lastModified(), written);

        // a file that is new is read, and then it is written
        writeTaggedWav(music.filePath(QStringLiteral("disc0/new.wav")), {{"INAM", "New"}, {"IART", "Someone"}, {"IPRD", "Album"}});
        scannedAgain.clear();
        again.rescan();
        QTRY_VERIFY_WITH_TIMEOUT(scannedAgain.count() > 0, 20000);
        QCOMPARE(again.songs()->count(), kSongs + 1);
        QVERIFY(QFileInfo(cache.filePath()).lastModified() > written);
    }

    void aPlaylistMadeHereOpensAndChanges()
    {
        QTemporaryDir music;
        QTemporaryDir storage;
        Settings settings(storage.filePath(QStringLiteral("swifty.conf")));
        ErrorCenter errors {nullptr};
        QNetworkAccessManager network;
        testing::FakeCore core;
        LibraryStore store(&settings, &errors);
        LocalLibrary library(&settings, nullptr, storage.path());
        store.attachLocalFiles(&library);
        const QStringList uris = addThreeSongs(library, music);
        QCOMPARE(uris.size(), 3);

        // made from a song's menu: it opens, holding that song
        QSignalSpy saved(&store, &LibraryStore::playlistSaveFinished);
        store.saveLocalPlaylist(true, QStringLiteral("Mine"), {}, {}, QVariant::fromValue(songWithUri(library, uris[0])));
        QCOMPARE(saved.count(), 1);
        QCOMPARE(saved.first().first().toBool(), true);
        QCOMPARE(store.page(), LibraryStore::Page::Playlist);
        const Playlist mine = store.selectedPlaylist();
        QVERIFY(mine.isLocal());
        QCOMPARE(mine.name, QStringLiteral("Mine"));
        QVERIFY(store.isSelectedPlaylistEditable());
        QVERIFY(store.canReorderPlaylist());
        QTRY_COMPARE(namesOf(store.playlistSongs()->songs()), (QStringList {"Avalon"}));

        // no account of any kind is involved
        store.addToPlaylist(songWithUri(library, uris[1]), mine);
        store.addToPlaylist(songWithUri(library, uris[2]), mine);
        QTRY_COMPARE(namesOf(store.playlistSongs()->songs()), (QStringList {"Avalon", "Bluebird", "Comet"}));
        QCOMPARE(store.selectedPlaylist().songCount, 3);

        // a song in a playlist is still found once, and not twice
        store.setSearchQuery(QStringLiteral("Avalon"));
        QCOMPARE(store.searchSongs()->count(), 1);
        store.setSearchQuery(QString());

        // the Songs page holds every song, once, whatever is a favourite
        QTRY_COMPARE(store.allSongs()->count(), 3);
        // and the Albums page one album for them, by its tags
        QTRY_COMPARE(store.allAlbums()->count(), 1);
        QCOMPARE(store.allAlbums()->at(0).name, QStringLiteral("Album"));
        store.toggleSaved(songWithUri(library, uris[1]));
        QCOMPARE(namesOf(store.savedSongs()->songs()), (QStringList {"Bluebird"}));
        QCOMPARE(store.allSongs()->count(), 3);
        store.toggleSaved(songWithUri(library, uris[1]));
        QCOMPARE(store.savedSongs()->count(), 0);
        QCOMPARE(store.allSongs()->count(), 3);

        // the top bar's filter field narrows the list of the page that is open
        store.setPlaylistFilterQuery(QStringLiteral("Bluebird"));
        QCOMPARE(namesOf(store.displayedAllSongs()->songs()), (QStringList {"Bluebird"}));
        QCOMPARE(store.displayedAlbums()->count(), 0);
        QCOMPARE(store.displayedArtists()->count(), 0);
        store.setPlaylistFilterQuery(QStringLiteral("Album"));
        QCOMPARE(store.displayedAlbums()->count(), 1);
        // and is forgotten on moving to another page
        store.showAlbums();
        QCOMPARE(store.playlistFilterQuery(), QString());
        QCOMPARE(store.displayedAllSongs()->count(), 3);
        store.showArtists();
        QVERIFY(store.displayedArtists()->count() >= 1);
        store.goBack();
        store.goBack();

        // a song added to the library shows everywhere it should: the list of every song,
        // the search and the albums, though the library was worked out before
        const QString extra = music.filePath(QStringLiteral("4.wav"));
        writeTaggedWav(extra, {{"INAM", "Delta"}, {"IART", "Someone Else"}, {"IPRD", "Another Album"}, {"IPRT", "1"}});
        library.rescan();
        QTRY_COMPARE(store.allSongs()->count(), 4);
        QTRY_COMPARE(store.allAlbums()->count(), 2);
        store.setSearchQuery(QStringLiteral("Delta"));
        QTRY_COMPARE(store.searchSongs()->count(), 1);
        store.setSearchQuery(QString());
        // and a star does not touch the list of every song
        const QList<Song> beforeStar = store.allSongs()->songs();
        store.toggleSaved(songWithUri(library, uris[0]));
        QVERIFY(sameInstances(beforeStar, store.allSongs()->songs()));
        store.toggleSaved(songWithUri(library, uris[0]));

        // a song listened to is played; skipping and seeking through one is not
        const Song heard = songWithUri(library, uris[0]);
        store.notePlayProgress(heard, 0, 200'000, true);
        for (quint32 at = 100; at <= 40'000; at += 100) {
            store.notePlayProgress(heard, at, 200'000, true);
        }
        QCOMPARE(store.playCountOf(heard.uri), 1);
        QCOMPARE(store.mostPlayed()->count(), 0);  // once is not most played
        const Song skipped = songWithUri(library, uris[1]);
        store.notePlayProgress(skipped, 0, 200'000, true);
        store.notePlayProgress(skipped, 90'000, 200'000, true);
        store.notePlayProgress(skipped, 150'000, 200'000, true);
        QCOMPARE(store.playCountOf(skipped.uri), 0);
        // played over from the start, and listened to again
        store.notePlayProgress(heard, 1'000, 200'000, true);
        for (quint32 at = 1'100; at <= 35'000; at += 100) {
            store.notePlayProgress(heard, at, 200'000, true);
        }
        QCOMPARE(store.playCountOf(heard.uri), 2);
        QCOMPARE(namesOf(store.mostPlayed()->songs()), (QStringList {"Avalon"}));
        // the counts are kept
        QCOMPARE(settings.value(QStringLiteral("playCounts")).toByteArray().contains("2"), true);
        Song notAFile;
        notAFile.uri = QStringLiteral("https://example.org/song.mp3");
        store.addToPlaylist(notAFile, mine);
        QVERIFY(errors.message().contains(QStringLiteral("Only music files")));
        QCOMPARE(store.playlistSongs()->count(), 3);

        // a drag moves the rows where they are, and the order is kept
        const quint64 avalon = store.playlistSongs()->songs().at(0).instanceId;
        store.moveSongs(0, 1, 3);
        QCOMPARE(namesOf(store.playlistSongs()->songs()), (QStringList {"Bluebird", "Comet", "Avalon"}));
        QCOMPARE(store.playlistSongs()->songs().at(2).instanceId, avalon);
        QCOMPARE(namesOf(library.songsOf(mine.id)), (QStringList {"Bluebird", "Comet", "Avalon"}));

        // the row's own song: the playlist's copy
        store.removeFromSelectedPlaylist(store.playlistSongs()->songs().at(0));
        QCOMPARE(namesOf(store.playlistSongs()->songs()), (QStringList {"Comet", "Avalon"}));
        // the occurrences left are the ones that were shown
        QCOMPARE(store.playlistSongs()->songs().at(1).instanceId, avalon);

        store.saveLocalPlaylist(false, QStringLiteral("Still Mine"), QStringLiteral("two left"), {}, QVariant());
        QCOMPARE(saved.count(), 2);
        QCOMPARE(store.selectedPlaylist().name, QStringLiteral("Still Mine"));

        // pinned, it shows with the pinned playlists
        store.togglePinned(store.selectedPlaylist());
        QCOMPARE(store.pinnedPlaylists()->count(), 1);

        // away and back again
        store.showAlbums();
        store.showSongs();
        QCOMPARE(store.page(), LibraryStore::Page::Songs);
        store.goBack();
        QCOMPARE(store.page(), LibraryStore::Page::Albums);
        store.goBack();
        QCOMPARE(store.selectedPlaylistId(), mine.id);
        store.showAlbums();
        store.selectPlaylistById(mine.id);
        QCOMPARE(store.selectedPlaylistId(), mine.id);
        QCOMPARE(store.playlistSongs()->count(), 2);

        QSignalSpy deleted(&store, &LibraryStore::playlistDeleted);
        store.deletePlaylist(store.selectedPlaylist());
        QCOMPARE(deleted.count(), 1);
        QCOMPARE(store.page(), LibraryStore::Page::Playlists);
        QCOMPARE(store.pinnedPlaylists()->count(), 0);
        QCOMPARE(library.playlists()->count(), 0);
    }

    void aSongsTagsAreEditedInItsFile()
    {
        QTemporaryDir music;
        QTemporaryDir storage;
        Settings settings(storage.filePath(QStringLiteral("swifty.conf")));
        LocalLibrary library(&settings, nullptr, storage.path());
        const QStringList uris = addThreeSongs(library, music);
        QCOMPARE(uris.size(), 3);
        QCOMPARE(library.tagsOf(uris[0]).value(QStringLiteral("title")).toString(), QStringLiteral("Avalon"));
        QCOMPARE(library.tagsOf(uris[0]).value(QStringLiteral("trackNumber")).toString(), QStringLiteral("1"));

        // queued to play, as it is now
        ErrorCenter errors {nullptr};
        QNetworkAccessManager network;
        testing::FakeCore core;
        PlaybackController player(&core, &settings, &errors);
        const Song queued = songWithUri(library, uris[0]);
        player.play(queued, {queued});
        for (int pass = 0; pass < 5; ++pass) {
            QCoreApplication::processEvents();
        }

        QSignalSpy saved(&library, &LocalLibrary::tagsSaved);
        QSignalSpy scanned(&library, &LocalLibrary::scanned);
        const QByteArray cover("\xff\xd8\xff\xe0 a cover", 14);
        library.editTags(uris[0],
                         {{QStringLiteral("title"), QStringLiteral("Avalon (Live)")},
                          {QStringLiteral("artist"), QStringLiteral("Roxy Music")},
                          {QStringLiteral("trackNumber"), QStringLiteral("9")}},
                         cover, false);
        QTRY_VERIFY_WITH_TIMEOUT(saved.count() > 0, 5000);
        QCOMPARE(saved.first().at(1).toBool(), true);
        QTRY_VERIFY_WITH_TIMEOUT(scanned.count() > 0, 5000);
        const Song edited = songWithUri(library, uris[0]);
        QCOMPARE(edited.name, QStringLiteral("Avalon (Live)"));
        QCOMPARE(edited.artists, QStringLiteral("Roxy Music"));
        QVERIFY(!edited.artworkUrl.isEmpty());
        QCOMPARE(library.tagsOf(uris[0]).value(QStringLiteral("trackNumber")).toString(), QStringLiteral("9"));
        // the album was left as it was
        QCOMPARE(edited.albumName, QStringLiteral("Album"));

        // the copy playing takes it on, still the same occurrence
        player.refreshSongDetails(library.songs()->songs());
        QCOMPARE(player.currentSong().name, QStringLiteral("Avalon (Live)"));
        QCOMPARE(player.currentSong().instanceId, queued.instanceId);

        // and the cover can go again
        saved.clear();
        library.editTags(uris[0], {}, {}, true);
        QTRY_VERIFY_WITH_TIMEOUT(saved.count() > 0, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(scanned.count() > 0, 5000);
        QVERIFY(songWithUri(library, uris[0]).artworkUrl.isEmpty());

        // a file the library does not hold is not touched
        saved.clear();
        library.editTags(QStringLiteral("file:///nowhere/at/all.mp3"), {{QStringLiteral("title"), QStringLiteral("x")}}, {},
                         false);
        QCOMPARE(saved.count(), 1);
        QCOMPARE(saved.first().at(1).toBool(), false);
    }

    void lrcLinesAreTimedAndSorted()
    {
        const auto lyrics = Lyrics::fromLrc(QStringLiteral("[ar:Sade]\n[ti:Smooth Operator]\n"
                                                           "[00:21.50]Diamond life\n"
                                                           "[00:10.00][00:30.25]Lover boy\n"
                                                           "[00:25]Ooh\n"));
        QVERIFY(lyrics);
        QVERIFY(lyrics->synchronized);
        QCOMPARE(lyrics->lines.size(), 4);
        QCOMPARE(lyrics->lines.at(0).startTimeMs, 10'000u);
        QCOMPARE(lyrics->lines.at(0).words, QStringLiteral("Lover boy"));
        QCOMPARE(lyrics->lines.at(1).startTimeMs, 21'500u);
        QCOMPARE(lyrics->lines.at(1).endTimeMs, 25'000u);
        QCOMPARE(lyrics->lines.at(3).startTimeMs, 30'250u);
        QCOMPARE(lyrics->lineIndex(22'000), 1);

        // synced lines carry no end, so each runs to the next one's start
        const auto spotify = Lyrics::fromJson(R"({"syncType":"lineSynced","lines":[
            {"startTimeMs":1000,"endTimeMs":0,"words":"one"},
            {"startTimeMs":4000,"endTimeMs":0,"words":"two"}]})");
        QVERIFY(spotify);
        QCOMPARE(spotify->lines.at(0).endTimeMs, 4'000u);
        QCOMPARE(spotify->lines.at(1).endTimeMs, 9'000u);

        // without times, the lines are there to read along
        const auto plain = Lyrics::fromLrc(QStringLiteral("first line\nsecond line\n"));
        QVERIFY(plain);
        QVERIFY(!plain->synchronized);
        QCOMPARE(plain->lines.size(), 2);
        QVERIFY(!Lyrics::fromLrc(QStringLiteral("[ar:nobody]\n")));
    }

    void aFileOfOnesOwnPlaysFromDiskWithItsLyrics()
    {
        QTemporaryDir directory;
        Settings settings(directory.filePath(QStringLiteral("swifty.conf")));
        ErrorCenter errors {nullptr};
        QNetworkAccessManager network;
        testing::FakeCore core;
        PlaybackController player(&core, &settings, &errors);

        // brackets in a name, as "[HQ]" so often is, are no pattern
        const QString path = directory.filePath(QStringLiteral("Injury [HQ].mp3"));
        QFile lrc(directory.filePath(QStringLiteral("Injury [HQ].LRC")));
        QVERIFY(lrc.open(QIODevice::WriteOnly));
        lrc.write("[00:01.00]one\n[00:02.00]two\n");
        lrc.close();

        Song song;
        song.name = QStringLiteral("song");
        song.uri = QUrl::fromLocalFile(path).toString();
        player.play(song, {song});
        for (int pass = 0; pass < 5; ++pass) {
            QCoreApplication::processEvents();
        }
        // straight from disk: nothing else asked for
        const auto call = core.take(QStringLiteral("playFile"));
        QVERIFY(call);
        QCOMPARE(call->argument, path);
        QVERIFY(!core.take(QStringLiteral("playTrack")));
        call->succeed();
        for (int pass = 0; pass < 5; ++pass) {
            QCoreApplication::processEvents();
        }
        QVERIFY(player.isPlaying());
        QVERIFY(player.hasLyrics());
        QVERIFY(player.lyricsSynchronized());
        QCOMPARE(player.lyrics()->count(), 2);
    }
};

QTEST_MAIN(LocalTest)
#include "tst_local.moc"
