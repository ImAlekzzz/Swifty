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

#include "library/CollectionModels.h"
#include "library/SongListModel.h"

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <QUrl>
#include <QtQml/qqmlregistration.h>

#include <utility>

class QQmlEngine;
class QJSEngine;

namespace swifty {

class Settings;

/// music files of the person's own (LocalFiles in qml): the folders and
/// single files they added, searched for music, each song made from what its
/// own tags say: title, artists, album, track and disc, cover. tags are read
/// once and kept, by the file's size and time, so a library is read again
/// only where it changed. a file whose tags say nothing still gets a name
/// from its file and an album from its folder.
///
/// it also keeps playlists made on this computer: each a name, a
/// description, a cover and its tracks in order. a playlist holds copies of
/// its tracks of its own, taken from anywhere on disk and kept with the app,
/// so an original moved or deleted leaves it whole. a copy is kept once for
/// every playlist that holds it, and goes when none does.
class LocalLibrary : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(LocalFiles)
    QML_SINGLETON

    /// what was added: folders, searched all the way down, and single files
    Q_PROPERTY(QStringList sources READ sources NOTIFY sourcesChanged FINAL)
    Q_PROPERTY(swifty::SongListModel *songs READ songs CONSTANT FINAL)
    Q_PROPERTY(bool scanning READ isScanning NOTIFY scanningChanged FINAL)
    /// the playlists made here, newest first
    Q_PROPERTY(swifty::PlaylistListModel *playlists READ playlists CONSTANT FINAL)

public:
    /// `storageDirectory` holds what the tags said and the covers taken out
    /// of files; the app's own data folder by default
    explicit LocalLibrary(Settings *settings, QObject *parent = nullptr, const QString &storageDirectory = {});
    ~LocalLibrary() override;

    static LocalLibrary *instance();
    static LocalLibrary *create(QQmlEngine *, QJSEngine *);

    /// a file the player can play, going by its extension
    static bool isMusicFile(const QString &path);

    QStringList sources() const { return m_sources; }
    SongListModel *songs() const { return m_songs; }
    bool isScanning() const { return m_scanning; }
    PlaylistListModel *playlists() const { return m_playlists; }
    /// every song of one's own: the folders and files added, and the copies an older
    /// version made for playlists whose originals are gone
    QList<Song> allSongs() const;

    /// a playlist made here; invalid if there is none by that id
    Playlist playlist(const QString &id) const;
    /// the playlists made here, for a menu of where a song can go
    Q_INVOKABLE QVariantList playlistChoices() const;
    /// a playlist's songs as the library has them now, each its own
    /// occurrence
    QList<Song> songsOf(const QString &id) const;
    /// a new playlist, starting with the files among `songs`
    Playlist createPlaylist(const QString &name, const QString &description, const QByteArray &coverJpeg,
                            const QList<Song> &songs);
    /// a new name and description, and a new cover if one is given
    bool updatePlaylist(const QString &id, const QString &name, const QString &description, const QByteArray &coverJpeg);
    bool deletePlaylist(const QString &id);
    /// adds the files among `songs` at the end: the playlist holds the path of each,
    /// and nothing is copied. how many were added
    int addSongs(const QString &id, const QList<Song> &songs);
    /// adds music files, or every one in a folder, from anywhere at the end of
    /// the playlist. one outside the library's folders joins the library, which
    /// is where songs are known from. reports through tracksAdded
    Q_INVOKABLE void addFiles(const QString &playlistId, const QList<QUrl> &urls);
    /// takes a track out wherever it is in the playlist; how many times
    int removeSong(const QString &id, const QString &uri);
    /// puts the files it shows in this order, and any it no longer finds
    /// after them
    void reorder(const QString &id, const QStringList &uris);

    /// what a local song's tags say, as written in the file: title, artist,
    /// album, albumArtist, genre, date, trackNumber, discNumber and
    /// artworkUrl, each missing when the file does not say it
    Q_INVOKABLE QVariantMap tagsOf(const QString &uri) const;
    /// writes `fields` (any of those above, as text; an empty one clears it)
    /// into a local song's file, with `artworkJpeg` as its cover when given
    /// or no cover at all with `removeArtwork`. the file is read again once
    /// written. reports through tagsSaved
    Q_INVOKABLE void editTags(const QString &uri, const QVariantMap &fields, const QByteArray &artworkJpeg,
                              bool removeArtwork);

    /// adds folders and music files, dropped or picked, and searches them.
    /// returns how many were new
    Q_INVOKABLE int addSources(const QList<QUrl> &urls);
    Q_INVOKABLE void removeSource(const QString &path);
    /// searches every source again, for files added, changed or gone
    Q_INVOKABLE void rescan();
    /// searches again only if the last search was a while ago, and none is going
    /// on: for when the window comes back into focus, which is when files
    /// were likely added
    Q_INVOKABLE void rescanIfStale();

signals:
    void sourcesChanged();
    void scanningChanged();
    /// a search finished, with how many songs there are now
    void scanned(int songCount);
    /// a playlist was made, changed or deleted, or the files it holds were
    void playlistsChanged();
    /// tracks asked for were copied into a playlist: how many went in, and
    /// what could not, if anything
    void tracksAdded(const QString &playlistId, int count, const QString &problem);
    /// an edit of a song's tags was written, or failed with `error`
    void tagsSaved(const QString &uri, bool ok, const QString &error);

private:
    struct Entry {
        qint64 size = 0;
        qint64 modified = 0;
        QJsonObject tags;
    };
    struct ScanResult {
        QList<Song> songs;
        QHash<QString, Entry> entries;
    };

    static ScanResult scan(const QStringList &sources, QHash<QString, Entry> known, const QString &artworkDirectory,
                           const QString &cachePath);
    /// what the last searches found, from the cache database; a local-files.json
    /// left by an earlier version is read into it once. runs off the window's thread
    static QHash<QString, Entry> loadEntries(const QString &cachePath, const QString &legacyCachePath);
    static Song songFor(const QString &path, const QJsonObject &tags);
    /// the songs among `entries`, by artist and album, then track
    static QList<Song> songsFrom(const QHash<QString, Entry> &entries);
    struct StoredPlaylist {
        QString id;
        QString name;
        QString description;
        /// the cover's file in the covers folder; empty for none
        QString cover;
        QStringList uris;
    };
    /// the file a copy was taken from, as it was then
    struct Origin {
        QString source;
        qint64 size = 0;
        qint64 modified = 0;
    };
    /// playlists made by an older version held copies of their files: the
    /// ones whose original is still there are put back on it, and let go
    void moveBackToOriginals();
    /// lets go of copies no playlist holds
    void collectCopies();
    /// at launch, before anything is copied: folders in the store nobody knows
    void sweepStore();
    bool isKeptCopy(const QString &path) const;
    QStringList scanSources() const;

    /// reads the cache on another thread; what it held shows when it is done, and
    /// a search asked for before then waits for it
    void startLoadingCache();
    void applyCache();
    void setSources(const QStringList &sources);
    void setScanning(bool scanning);
    /// the playlists live in a SQLite database; a local-playlists.json left by an
    /// earlier version is read into it once, and set aside
    bool openDatabase();
    void importLegacyJson();
    void loadPlaylists();
    /// writes what changed since the last save, in one transaction
    void savePlaylists() const;
    /// with no SQLite driver to be had the playlists are kept in the JSON file as
    /// before, so none are lost; they move into the database once there is one
    void readJsonPlaylists();
    void writeJsonPlaylists() const;
    static size_t contentHash(const StoredPlaylist &stored);
    static size_t copiesHash(const QHash<QString, Origin> &copies);
    /// the playlists as they show now, after a change to them or their files
    void publishPlaylists();
    using SongIndex = QHash<QString, const Song *>;
    /// the library's songs by uri, to look a playlist's files up in
    SongIndex indexSongs() const;
    QList<Song> resolve(const StoredPlaylist &stored, const SongIndex &index) const;
    Playlist describe(const StoredPlaylist &stored, const SongIndex &index) const;
    StoredPlaylist *findPlaylist(const QString &id);
    const StoredPlaylist *findPlaylist(const QString &id) const;
    /// writes a cover under a name of its own, so nothing cached shows the
    /// old one, and lets the old one go
    QString writeCover(const StoredPlaylist &stored, const QByteArray &jpeg) const;

    Settings *m_settings;
    /// the cache database, and the JSON file earlier versions kept it in
    QString m_cachePath;
    QString m_legacyCachePath;
    bool m_cacheLoaded = false;
    QString m_artworkDirectory;
    QStringList m_sources;
    SongListModel *m_songs;
    QHash<QString, Entry> m_entries;
    bool m_scanning = false;
    /// the JSON file earlier versions kept the playlists in, read only to move them
    QString m_playlistsPath;
    QString m_databasePath;
    QString m_databaseName;
    /// what the database holds of the playlists, so a save writes only what changed
    mutable QHash<QString, size_t> m_savedPlaylists;
    mutable QStringList m_savedOrder;
    mutable size_t m_savedCopies = 0;
    mutable bool m_copiesSaved = false;
    QString m_coverDirectory;
    /// where copies made by older versions live, each in a folder of its own
    QString m_trackStore;
    /// those copies, by their path, with the file each was made from
    QHash<QString, Origin> m_copies;
    /// the copies' songs, by uri
    QHash<QString, Song> m_storeSongs;
    QList<StoredPlaylist> m_storedPlaylists;
    PlaylistListModel *m_playlists;
    /// a search started after another one makes that one's result stale
    quint64 m_scanGeneration = 0;
    bool m_rescanQueued = false;
    qint64 m_lastScanAtMs = 0;
};

} // namespace swifty
