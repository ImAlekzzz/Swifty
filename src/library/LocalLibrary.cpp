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

#include "library/LocalLibrary.h"

#include "core/Diagnostics.h"

#include "app/Paths.h"
#include "app/Settings.h"

#include <swifty_core.h>

#include <QCollator>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QThread>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QQmlEngine>
#include <QSaveFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSet>
#include <QThreadPool>
#include <QUuid>
#include <QtConcurrent>

#include <algorithm>
#include <cstdint>
#include <utility>

namespace swifty {

Q_LOGGING_CATEGORY(lcLocal, "swifty.local")

namespace {

LocalLibrary *s_instance = nullptr;

const QString kSourcesKey = QStringLiteral("localMusicSources");
// 2: songs carry their genre, which a cache of version 1 lacks
constexpr int kCacheVersion = 2;
constexpr int kPlaylistsVersion = 1;
const QString kLocalPrefix = QStringLiteral("local:");

/// what the core's decoder plays (audio-core's local::AUDIO_EXTENSIONS)
const QSet<QString> kMusicExtensions {
    QStringLiteral("mp3"), QStringLiteral("flac"), QStringLiteral("ogg"), QStringLiteral("oga"),
    QStringLiteral("m4a"), QStringLiteral("mp4"), QStringLiteral("aac"), QStringLiteral("alac"),
    QStringLiteral("wav"), QStringLiteral("aif"), QStringLiteral("aiff"),
};

QJsonObject readTags(const QString &path, const QString &artworkDirectory)
{
    char *error = nullptr;
    char *json = swifty_read_local_tags(path.toUtf8().constData(), artworkDirectory.toUtf8().constData(), &error);
    if (!json) {
        if (error) {
            qCDebug(lcLocal).noquote() << "skipping" << path << ':' << QString::fromUtf8(error);
            swifty_string_free(error);
        }
        return {};
    }
    const QByteArray bytes(json);
    swifty_string_free(json);
    return QJsonDocument::fromJson(bytes).object();
}

QString text(const QJsonObject &tags, const char *key)
{
    return tags.value(QLatin1StringView(key)).toString().trimmed();
}

/// a folder's own path, not a file or folder inside one already added
bool isWithin(const QString &path, const QString &folder)
{
    return path == folder || path.startsWith(folder.endsWith(u'/') ? folder : folder + u'/');
}

} // namespace

LocalLibrary::LocalLibrary(Settings *settings, QObject *parent, const QString &storageDirectory)
    : QObject(parent)
    , m_settings(settings)
    , m_songs(new SongListModel(this))
{
    if (!s_instance) {
        s_instance = this;
    }
    const QString storage = storageDirectory.isEmpty() ? paths::dataDirectory() : storageDirectory;
    QDir().mkpath(storage);
    m_cachePath = storage + QStringLiteral("/local-files.db");
    m_legacyCachePath = storage + QStringLiteral("/local-files.json");
    // a playlist's cover is the person's own, and cannot be taken out again
    m_playlistsPath = storage + QStringLiteral("/local-playlists.json");
    m_databasePath = storage + QStringLiteral("/local-playlists.db");
    m_databaseName = QStringLiteral("swifty-playlists-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_coverDirectory = storage + QStringLiteral("/local-playlist-covers");
    // copies of tracks that older versions made for playlists. nothing is copied now
    m_trackStore = storage + QStringLiteral("/local-playlist-tracks");
    m_playlists = new PlaylistListModel(this);
    // covers taken out of files can always be taken out again
    m_artworkDirectory = (storageDirectory.isEmpty() ? paths::cacheDirectory() : storageDirectory)
        + QStringLiteral("/local-artwork");

    m_sources = m_settings->value(kSourcesKey).toStringList();
    if (openDatabase()) {
        importLegacyJson();
    }
    loadPlaylists();
    moveBackToOriginals();
    sweepStore();
    publishPlaylists();
    // the cache is read on another thread: a search asked for now starts when it is in
    if (!m_sources.isEmpty() || QFileInfo::exists(m_trackStore)) {
        m_rescanQueued = true;
    }
    startLoadingCache();
}

LocalLibrary::~LocalLibrary()
{
    {
        QSqlDatabase database = QSqlDatabase::database(m_databaseName, false);
        if (database.isValid()) {
            database.close();
        }
    }
    QSqlDatabase::removeDatabase(m_databaseName);
    if (s_instance == this) {
        s_instance = nullptr;
    }
}

LocalLibrary *LocalLibrary::instance()
{
    return s_instance;
}

LocalLibrary *LocalLibrary::create(QQmlEngine *, QJSEngine *)
{
    QJSEngine::setObjectOwnership(s_instance, QJSEngine::CppOwnership);
    return s_instance;
}

bool LocalLibrary::isMusicFile(const QString &path)
{
    return kMusicExtensions.contains(QFileInfo(path).suffix().toLower());
}

int LocalLibrary::addSources(const QList<QUrl> &urls)
{
    QStringList sources = m_sources;
    int added = 0;
    for (const QUrl &url : urls) {
        if (!url.isLocalFile()) {
            continue;
        }
        const QFileInfo info(url.toLocalFile());
        const QString path = info.canonicalFilePath();
        if (path.isEmpty() || (info.isFile() && !isMusicFile(path)) || (!info.isFile() && !info.isDir())) {
            continue;
        }
        // already searched as part of a folder added before
        if (std::any_of(sources.begin(), sources.end(), [&](const QString &source) { return isWithin(path, source); })) {
            continue;
        }
        // a folder takes in what was added from inside it
        if (info.isDir()) {
            sources.removeIf([&](const QString &source) { return isWithin(source, path); });
        }
        sources.append(path);
        ++added;
    }
    if (added > 0) {
        setSources(sources);
        rescan();
    }
    return added;
}

void LocalLibrary::removeSource(const QString &path)
{
    QStringList sources = m_sources;
    if (sources.removeAll(path) == 0) {
        return;
    }
    setSources(sources);
    // gone at once, rather than when the search catches up
    QList<Song> kept;
    for (const Song &song : m_songs->songs()) {
        if (!isWithin(song.localPath(), path)) {
            kept.append(song);
        }
    }
    m_songs->setSongs(kept);
    publishPlaylists();
    rescan();
}

void LocalLibrary::setSources(const QStringList &sources)
{
    if (m_sources == sources) {
        return;
    }
    m_sources = sources;
    m_settings->setValue(kSourcesKey, m_sources);
    emit sourcesChanged();
}

void LocalLibrary::setScanning(bool scanning)
{
    if (m_scanning != scanning) {
        m_scanning = scanning;
        emit scanningChanged();
    }
}

void LocalLibrary::rescanIfStale()
{
    constexpr qint64 kStaleAfterMs = 5 * 60 * 1000;
    if (m_scanning || scanSources().isEmpty()) {
        diagnostic("library", QStringLiteral("back in the window; not searching (%1)").arg(m_scanning ? QStringLiteral("already searching") : QStringLiteral("no folders")));
        return;
    }
    const qint64 sinceMs = QDateTime::currentMSecsSinceEpoch() - m_lastScanAtMs;
    if (sinceMs < kStaleAfterMs) {
        diagnostic("library", QStringLiteral("back in the window; the last search was %1 s ago, so not yet").arg(sinceMs / 1000));
        return;
    }
    diagnostic("library", QStringLiteral("back in the window; searching the folders again"));
    rescan();
}

void LocalLibrary::rescan()
{
    if (!m_cacheLoaded) {
        // what was found last time is still being read: the search starts when it is in
        m_rescanQueued = true;
        return;
    }
    if (m_scanning) {
        // one search at a time; this one goes again once it is done
        m_rescanQueued = true;
        return;
    }
    const quint64 generation = ++m_scanGeneration;
    setScanning(true);
    QtConcurrent::run(QThreadPool::globalInstance(), &LocalLibrary::scan, scanSources(), m_entries, m_artworkDirectory,
                      m_cachePath)
        .then(this, [this, generation](const ScanResult &result) {
            if (generation != m_scanGeneration) {
                return;
            }
            m_entries = result.entries;
            // a song that is just as it was stays the same song: the one
            // playing stays marked, and the list keeps its rows
            QHash<QString, Song> before;
            for (const Song &song : m_songs->songs()) {
                before.insert(song.uri, song);
            }
            for (auto kept = m_storeSongs.cbegin(); kept != m_storeSongs.cend(); ++kept) {
                before.insert(kept.key(), *kept);
            }
            // the playlists' copies are songs of their own, not the folders'
            QList<Song> songs;
            QHash<QString, Song> keptSongs;
            for (Song song : result.songs) {
                const auto old = before.constFind(song.uri);
                if (old != before.cend() && old->name == song.name && old->artists == song.artists
                    && old->albumName == song.albumName && old->artworkUrl == song.artworkUrl
                    && old->durationMs == song.durationMs) {
                    song.instanceId = old->instanceId;
                }
                if (isKeptCopy(song.localPath())) {
                    keptSongs.insert(song.uri, song);
                } else {
                    songs.append(song);
                }
            }
            m_storeSongs = keptSongs;
            m_songs->setSongs(songs);
            publishPlaylists();
            setScanning(false);
            m_lastScanAtMs = QDateTime::currentMSecsSinceEpoch();
            emit scanned(m_songs->count());
            if (std::exchange(m_rescanQueued, false)) {
                rescan();
            }
        });
}

namespace {

/// the cache database of what the searches found: a row for each music file, with its
/// tags as json. its connection belongs to the thread that made it, and is let go with it
struct CacheConnection {
    explicit CacheConnection(const QString &path)
        : name(QStringLiteral("swifty-cache-") + QUuid::createUuid().toString(QUuid::WithoutBraces))
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        database.setDatabaseName(path);
        if (!database.open()) {
            qCWarning(lcLocal).noquote() << "could not open the cache" << path << ':' << database.lastError().text();
            return;
        }
        QSqlQuery query(database);
        // a cache of another version is not read: it is made again by the next search
        query.exec(QStringLiteral("PRAGMA user_version"));
        const int version = query.next() ? query.value(0).toInt() : 0;
        query.finish();
        if (version != kCacheVersion) {
            ok = query.exec(QStringLiteral("DROP TABLE IF EXISTS files"));
        } else {
            ok = true;
        }
        ok = ok
            && query.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS files ("
                                         "path TEXT PRIMARY KEY, size INTEGER NOT NULL, modified INTEGER NOT NULL, "
                                         "tags TEXT NOT NULL) WITHOUT ROWID"));
        if (ok && version != kCacheVersion) {
            ok = query.exec(QStringLiteral("PRAGMA user_version = %1").arg(kCacheVersion));
        }
        if (!ok) {
            qCWarning(lcLocal).noquote() << "could not set up the cache:" << query.lastError().text();
        }
    }
    ~CacheConnection()
    {
        {
            QSqlDatabase database = QSqlDatabase::database(name, false);
            if (database.isValid()) {
                database.close();
            }
        }
        QSqlDatabase::removeDatabase(name);
    }
    CacheConnection(const CacheConnection &) = delete;
    CacheConnection &operator=(const CacheConnection &) = delete;

    QSqlDatabase database() const { return QSqlDatabase::database(name, false); }

    QString name;
    bool ok = false;
};

} // namespace

QHash<QString, LocalLibrary::Entry> LocalLibrary::loadEntries(const QString &cachePath, const QString &legacyCachePath)
{
    QHash<QString, Entry> entries;
    CacheConnection connection(cachePath);
    if (!connection.ok) {
        return entries;
    }
    QSqlDatabase database = connection.database();
    QSqlQuery query(database);

    // a cache kept as json by an earlier version moves in, once
    if (QFileInfo::exists(legacyCachePath)) {
        query.exec(QStringLiteral("SELECT 1 FROM files LIMIT 1"));
        const bool empty = !query.next();
        query.finish();
        QFile file(legacyCachePath);
        if (empty && file.open(QIODevice::ReadOnly)) {
            const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
            file.close();
            if (root.value(QStringLiteral("version")).toInt() == kCacheVersion && database.transaction()) {
                QSqlQuery insert(database);
                insert.prepare(QStringLiteral("INSERT OR REPLACE INTO files (path, size, modified, tags) VALUES (?, ?, ?, ?)"));
                const QJsonObject files = root.value(QStringLiteral("files")).toObject();
                bool ok = true;
                for (auto it = files.begin(); ok && it != files.end(); ++it) {
                    const QJsonObject entry = it.value().toObject();
                    insert.addBindValue(it.key());
                    insert.addBindValue(entry.value(QStringLiteral("size")).toInteger());
                    insert.addBindValue(entry.value(QStringLiteral("modified")).toInteger());
                    insert.addBindValue(QString::fromUtf8(
                        QJsonDocument(entry.value(QStringLiteral("tags")).toObject()).toJson(QJsonDocument::Compact)));
                    ok = insert.exec();
                }
                if (!ok || !database.commit()) {
                    database.rollback();
                }
            }
        }
        // set aside either way: it is only a cache, and is not read twice
        QFile::remove(legacyCachePath + QStringLiteral(".imported"));
        QFile::rename(legacyCachePath, legacyCachePath + QStringLiteral(".imported"));
    }

    if (query.exec(QStringLiteral("SELECT path, size, modified, tags FROM files"))) {
        while (query.next()) {
            entries.insert(query.value(0).toString(),
                           {query.value(1).toLongLong(), query.value(2).toLongLong(),
                            QJsonDocument::fromJson(query.value(3).toString().toUtf8()).object()});
        }
    }
    return entries;
}

void LocalLibrary::startLoadingCache()
{
    QtConcurrent::run(QThreadPool::globalInstance(), &LocalLibrary::loadEntries, m_cachePath, m_legacyCachePath)
        .then(this, [this](const QHash<QString, Entry> &loaded) {
            m_entries = loaded;
            applyCache();
            m_cacheLoaded = true;
            if (std::exchange(m_rescanQueued, false)) {
                rescan();
            }
        });
}

void LocalLibrary::applyCache()
{
    // what was there last time shows at once; the search that follows
    // brings it up to date
    QHash<QString, Entry> shown;
    for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it) {
        if (isKeptCopy(it.key())) {
            if (!it->tags.isEmpty() && QFileInfo::exists(it.key())) {
                const Song song = songFor(it.key(), it->tags);
                m_storeSongs.insert(song.uri, song);
            }
        } else if (std::any_of(m_sources.begin(), m_sources.end(),
                               [&](const QString &source) { return isWithin(it.key(), source); })) {
            shown.insert(it.key(), *it);
        }
    }
    m_songs->setSongs(songsFrom(shown));
    publishPlaylists();
}

QStringList LocalLibrary::scanSources() const
{
    QStringList sources = m_sources;
    if (QFileInfo(m_trackStore).isDir()) {
        sources.append(m_trackStore);
    }
    return sources;
}

bool LocalLibrary::isKeptCopy(const QString &path) const
{
    return !path.isEmpty() && isWithin(path, m_trackStore) && path != m_trackStore;
}

QList<Song> LocalLibrary::allSongs() const
{
    QList<Song> songs = m_songs->songs();
    songs.append(m_storeSongs.values());
    return songs;
}

QList<Song> LocalLibrary::songsFrom(const QHash<QString, Entry> &entries)
{
    struct Found {
        Song song;
        QString albumArtist;
        int disc = 0;
        int track = 0;
    };
    QList<Found> found;
    for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
        if (it->tags.isEmpty()) {
            continue;
        }
        Found item {songFor(it.key(), it->tags), text(it->tags, "albumArtist"),
                    it->tags.value(QStringLiteral("discNumber")).toInt(),
                    it->tags.value(QStringLiteral("trackNumber")).toInt()};
        if (item.albumArtist.isEmpty()) {
            item.albumArtist = item.song.artists;
        }
        found.append(item);
    }

    // by artist and album, then in the album's own order
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(found.begin(), found.end(), [&](const Found &a, const Found &b) {
        if (const int artist = collator.compare(a.albumArtist, b.albumArtist); artist != 0) {
            return artist < 0;
        }
        if (const int album = collator.compare(a.song.albumName, b.song.albumName); album != 0) {
            return album < 0;
        }
        if (a.disc != b.disc) {
            return a.disc < b.disc;
        }
        if (a.track != b.track) {
            return a.track < b.track;
        }
        return collator.compare(a.song.localPath(), b.song.localPath()) < 0;
    });

    QList<Song> songs;
    songs.reserve(found.size());
    for (const Found &item : found) {
        songs.append(item.song);
    }
    return songs;
}

LocalLibrary::ScanResult LocalLibrary::scan(const QStringList &sources, QHash<QString, Entry> known,
                                            const QString &artworkDirectory, const QString &cachePath)
{
    QElapsedTimer clock;
    clock.start();
    QStringList files;
    QSet<QString> seen;
    const auto take = [&](const QFileInfo &info) {
        const QString path = info.absoluteFilePath();
        if (isMusicFile(path) && !seen.contains(path)) {
            seen.insert(path);
            files.append(path);
        }
    };
    for (const QString &source : sources) {
        const QFileInfo info(source);
        if (info.isFile()) {
            take(info);
            continue;
        }
        // hidden files are left out, and with them the "._" shadows macos
        // leaves on other disks
        QDirIterator walk(source, QDir::Files | QDir::Readable | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (walk.hasNext()) {
            walk.next();
            take(walk.fileInfo());
        }
    }

    // a file that has not changed keeps what the last search found in it
    QHash<QString, Entry> entries;
    entries.reserve(files.size());
    QStringList toRead;
    for (const QString &path : files) {
        const QFileInfo info(path);
        Entry entry {info.size(), info.lastModified().toMSecsSinceEpoch(), {}};
        const auto cached = known.constFind(path);
        if (cached != known.cend() && cached->size == entry.size && cached->modified == entry.modified) {
            entry.tags = cached->tags;
        } else {
            toRead.append(path);
        }
        entries.insert(path, entry);
    }
    if (!toRead.isEmpty()) {
        // the tags and cover of every new or changed file are read side by side:
        // on a first search of a big library that is all the time there is. one
        // core is left for the window and the sound, and the work is the last to
        // be given one. the covers are written aside and moved into place, so
        // two tracks of an album can race
        QThreadPool pool;
        pool.setMaxThreadCount(std::max(1, QThread::idealThreadCount() - 1));
        pool.setThreadPriority(QThread::LowPriority);
        const QList<QJsonObject> read = QtConcurrent::blockingMapped(&pool, toRead, [&artworkDirectory](const QString &path) {
            return readTags(path, artworkDirectory);
        });
        // an unreadable file is remembered as such, and not tried again until it changes
        for (qsizetype index = 0; index < toRead.size(); ++index) {
            entries[toRead.at(index)].tags = read.at(index);
        }
    }

    ScanResult result;
    result.songs = songsFrom(entries);
    result.entries = entries;

    // kept for next time: the files that were read go in, and the ones that went
    // come out. when nothing was read and no file came or went, nothing is written:
    // a search when the window comes back should not write for nothing
    QStringList removed;
    for (auto it = known.cbegin(); it != known.cend(); ++it) {
        if (!entries.contains(it.key())) {
            removed.append(it.key());
        }
    }
    if (toRead.isEmpty() && removed.isEmpty()) {
        diagnostic("library", QStringLiteral("searched %1 files, nothing changed, in %2 ms").arg(files.size()).arg(clock.elapsed()));
        return result;
    }
    {
        CacheConnection connection(cachePath);
        if (connection.ok) {
            QSqlDatabase database = connection.database();
            bool ok = database.transaction();
            QSqlQuery query(database);
            if (ok && !removed.isEmpty()) {
                QVariantList paths(removed.cbegin(), removed.cend());
                ok = query.prepare(QStringLiteral("DELETE FROM files WHERE path = ?"));
                query.addBindValue(paths);
                ok = ok && query.execBatch();
            }
            if (ok && !toRead.isEmpty()) {
                QVariantList paths;
                QVariantList sizes;
                QVariantList modifieds;
                QVariantList tagsList;
                for (const QString &path : std::as_const(toRead)) {
                    const Entry &entry = entries[path];
                    paths.append(path);
                    sizes.append(entry.size);
                    modifieds.append(entry.modified);
                    tagsList.append(QString::fromUtf8(QJsonDocument(entry.tags).toJson(QJsonDocument::Compact)));
                }
                ok = query.prepare(QStringLiteral("INSERT OR REPLACE INTO files (path, size, modified, tags) VALUES (?, ?, ?, ?)"));
                query.addBindValue(paths);
                query.addBindValue(sizes);
                query.addBindValue(modifieds);
                query.addBindValue(tagsList);
                ok = ok && query.execBatch();
            }
            if (!ok || !database.commit()) {
                qCWarning(lcLocal).noquote() << "could not keep what the search found:" << query.lastError().text();
                database.rollback();
            }
        }
    }
    diagnostic("library", QStringLiteral("searched %1 files, read %2, in %3 ms").arg(files.size()).arg(toRead.size()).arg(clock.elapsed()));
    return result;
}

Song LocalLibrary::songFor(const QString &path, const QJsonObject &tags)
{
    const QFileInfo info(path);
    Song song;
    song.uri = QUrl::fromLocalFile(info.absoluteFilePath()).toString();
    song.name = text(tags, "title");
    if (song.name.isEmpty()) {
        song.name = info.completeBaseName();
    }
    song.artists = text(tags, "artist");
    if (song.artists.isEmpty()) {
        song.artists = text(tags, "albumArtist");
    }
    if (song.artists.isEmpty()) {
        song.artists = QStringLiteral("Unknown Artist");
    }
    song.albumName = text(tags, "album");
    if (song.albumName.isEmpty()) {
        song.albumName = info.dir().dirName();
    }
    song.genre = text(tags, "genre");
    song.durationMs = tags.value(QStringLiteral("durationMs")).toInt();
    const QString artwork = text(tags, "artworkPath");
    if (!artwork.isEmpty()) {
        song.artworkUrl = QUrl::fromLocalFile(artwork);
    }
    return song;
}

// --- tags ------------------------------------------------------------------------------

namespace {

/// the keys an edit may change, as LocalTags and TagEdits name them
const QStringList kEditableTags {
    QStringLiteral("title"),  QStringLiteral("artist"), QStringLiteral("album"),
    QStringLiteral("albumArtist"), QStringLiteral("genre"), QStringLiteral("date"),
    QStringLiteral("trackNumber"), QStringLiteral("discNumber"),
};

} // namespace

QVariantMap LocalLibrary::tagsOf(const QString &uri) const
{
    const auto entry = m_entries.constFind(QUrl(uri).toLocalFile());
    if (entry == m_entries.cend()) {
        return {};
    }
    QVariantMap tags;
    for (const QString &key : kEditableTags) {
        const QJsonValue value = entry->tags.value(key);
        if (value.isString() && !value.toString().isEmpty()) {
            tags.insert(key, value.toString());
        } else if (value.isDouble()) {
            tags.insert(key, QString::number(value.toInteger()));
        }
    }
    const QString artwork = entry->tags.value(QStringLiteral("artworkPath")).toString();
    if (!artwork.isEmpty()) {
        tags.insert(QStringLiteral("artworkUrl"), QUrl::fromLocalFile(artwork));
    }
    return tags;
}

void LocalLibrary::editTags(const QString &uri, const QVariantMap &fields, const QByteArray &artworkJpeg,
                            bool removeArtwork)
{
    const QString path = QUrl(uri).toLocalFile();
    if (path.isEmpty() || !m_entries.contains(path)) {
        emit tagsSaved(uri, false, QStringLiteral("Only music files in Local Files can be edited."));
        return;
    }
    QJsonObject edits;
    for (const QString &key : kEditableTags) {
        if (fields.contains(key)) {
            edits.insert(key, fields.value(key).toString());
        }
    }
    edits.insert(QStringLiteral("removeArtwork"), removeArtwork);
    const QByteArray json = QJsonDocument(edits).toJson(QJsonDocument::Compact);
    // written away from the window: a large file is copied whole
    QtConcurrent::run(QThreadPool::globalInstance(), [path, json, artworkJpeg]() -> QString {
        char *error = swifty_write_local_tags(path.toUtf8().constData(), json.constData(),
                                                reinterpret_cast<const uint8_t *>(artworkJpeg.constData()),
                                                size_t(artworkJpeg.size()));
        if (!error) {
            return {};
        }
        const QString message = QString::fromUtf8(error);
        swifty_string_free(error);
        return message;
    }).then(this, [this, uri, path](const QString &error) {
        if (!error.isEmpty()) {
            qCWarning(lcLocal).noquote() << "editing" << path << ':' << error;
            emit tagsSaved(uri, false, error);
            return;
        }
        // read again however little its size and time moved
        m_entries.remove(path);
        rescan();
        emit tagsSaved(uri, true, {});
    });
}

// --- playlists ---------------------------------------------------------------------

size_t LocalLibrary::contentHash(const StoredPlaylist &stored)
{
    return qHashMulti(0, stored.id, stored.name, stored.description, stored.cover, stored.uris);
}

size_t LocalLibrary::copiesHash(const QHash<QString, Origin> &copies)
{
    // the same whatever order they are held in
    size_t total = 0;
    for (auto it = copies.cbegin(); it != copies.cend(); ++it) {
        total += qHashMulti(0, it.key(), it->source, it->size, it->modified);
    }
    return total;
}

bool LocalLibrary::openDatabase()
{
    if (!QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE"))) {
        qCWarning(lcLocal).noquote()
            << "Qt's SQLite driver (QSQLITE) is not installed, so the playlists are kept in" << m_playlistsPath
            << "instead. on Debian and Ubuntu it is the package libqt6sql6-sqlite. available drivers:"
            << QSqlDatabase::drivers();
        return false;
    }
    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_databaseName);
    database.setDatabaseName(m_databasePath);
    if (!database.open()) {
        qCWarning(lcLocal).noquote() << "could not open the playlists database" << m_databasePath << ':'
                                     << database.lastError().text();
        return false;
    }
    QSqlQuery query(database);
    const QStringList statements {
        QStringLiteral("PRAGMA foreign_keys = ON"),
        QStringLiteral("PRAGMA journal_mode = WAL"),
        QStringLiteral("PRAGMA synchronous = NORMAL"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS playlists ("
                       "id TEXT PRIMARY KEY, name TEXT NOT NULL, description TEXT NOT NULL DEFAULT '', "
                       "cover TEXT NOT NULL DEFAULT '', position INTEGER NOT NULL)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS playlist_songs ("
                       "playlist_id TEXT NOT NULL REFERENCES playlists(id) ON DELETE CASCADE, "
                       "position INTEGER NOT NULL, uri TEXT NOT NULL, PRIMARY KEY (playlist_id, position))"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS copies ("
                       "copy TEXT PRIMARY KEY, source TEXT NOT NULL, size INTEGER NOT NULL, modified INTEGER NOT NULL)"),
        QStringLiteral("PRAGMA user_version = 1"),
    };
    for (const QString &statement : statements) {
        if (!query.exec(statement)) {
            qCWarning(lcLocal).noquote() << "could not set up the playlists database:" << query.lastError().text();
            database.close();
            return false;
        }
    }
    return true;
}

/// playlists kept in local-playlists.json by an earlier version move into the
/// database, once: the file is set aside after, and ones already in the
/// database are left as they are
void LocalLibrary::importLegacyJson()
{
    QFile file(m_playlistsPath);
    if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
        return;
    }
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    if (root.value(QStringLiteral("version")).toInt() != kPlaylistsVersion) {
        qCWarning(lcLocal) << "playlists kept in a version this app does not read:" << m_playlistsPath;
        return;
    }
    QSqlDatabase database = QSqlDatabase::database(m_databaseName);
    if (!database.transaction()) {
        return;
    }
    QSqlQuery query(database);
    query.exec(QStringLiteral("SELECT COALESCE(MAX(position), -1) FROM playlists"));
    int position = query.next() ? query.value(0).toInt() : -1;
    bool ok = true;
    for (const QJsonValue &value : root.value(QStringLiteral("playlists")).toArray()) {
        const QJsonObject object = value.toObject();
        const QString id = object.value(QStringLiteral("id")).toString();
        if (!id.startsWith(kLocalPrefix)) {
            continue;
        }
        query.prepare(QStringLiteral("INSERT OR IGNORE INTO playlists (id, name, description, cover, position) "
                                     "VALUES (?, ?, ?, ?, ?)"));
        query.addBindValue(id);
        query.addBindValue(object.value(QStringLiteral("name")).toString());
        query.addBindValue(object.value(QStringLiteral("description")).toString());
        query.addBindValue(object.value(QStringLiteral("cover")).toString());
        query.addBindValue(++position);
        ok = ok && query.exec();
        if (!ok || query.numRowsAffected() < 1) {
            continue;
        }
        int row = 0;
        for (const QJsonValue &uri : object.value(QStringLiteral("songs")).toArray()) {
            query.prepare(QStringLiteral("INSERT INTO playlist_songs (playlist_id, position, uri) VALUES (?, ?, ?)"));
            query.addBindValue(id);
            query.addBindValue(row++);
            query.addBindValue(uri.toString());
            ok = ok && query.exec();
        }
    }
    for (const QJsonValue &value : root.value(QStringLiteral("copies")).toArray()) {
        const QJsonObject object = value.toObject();
        query.prepare(QStringLiteral("INSERT OR IGNORE INTO copies (copy, source, size, modified) VALUES (?, ?, ?, ?)"));
        query.addBindValue(object.value(QStringLiteral("copy")).toString());
        query.addBindValue(object.value(QStringLiteral("source")).toString());
        query.addBindValue(object.value(QStringLiteral("size")).toInteger());
        query.addBindValue(object.value(QStringLiteral("modified")).toInteger());
        ok = ok && query.exec();
    }
    if (!ok || !database.commit()) {
        qCWarning(lcLocal).noquote() << "could not move the playlists into the database:" << query.lastError().text();
        database.rollback();
        return;
    }
    // kept beside the database rather than deleted, in case anything is wrong
    QFile::remove(m_playlistsPath + QStringLiteral(".imported"));
    file.rename(m_playlistsPath + QStringLiteral(".imported"));
}

void LocalLibrary::readJsonPlaylists()
{
    QFile file(m_playlistsPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    if (root.value(QStringLiteral("version")).toInt() != kPlaylistsVersion) {
        return;
    }
    for (const QJsonValue &value : root.value(QStringLiteral("playlists")).toArray()) {
        const QJsonObject object = value.toObject();
        StoredPlaylist stored {object.value(QStringLiteral("id")).toString(), object.value(QStringLiteral("name")).toString(),
                               object.value(QStringLiteral("description")).toString(),
                               object.value(QStringLiteral("cover")).toString(), {}};
        if (!stored.id.startsWith(kLocalPrefix) || findPlaylist(stored.id)) {
            continue;
        }
        for (const QJsonValue &uri : object.value(QStringLiteral("songs")).toArray()) {
            stored.uris.append(uri.toString());
        }
        m_storedPlaylists.append(stored);
    }
    for (const QJsonValue &value : root.value(QStringLiteral("copies")).toArray()) {
        const QJsonObject object = value.toObject();
        const QString copy = object.value(QStringLiteral("copy")).toString();
        if (isKeptCopy(copy)) {
            m_copies.insert(copy, {object.value(QStringLiteral("source")).toString(), object.value(QStringLiteral("size")).toInteger(),
                                   object.value(QStringLiteral("modified")).toInteger()});
        }
    }
}

void LocalLibrary::writeJsonPlaylists() const
{
    QJsonArray playlists;
    for (const StoredPlaylist &stored : m_storedPlaylists) {
        playlists.append(QJsonObject {{QStringLiteral("id"), stored.id},
                                      {QStringLiteral("name"), stored.name},
                                      {QStringLiteral("description"), stored.description},
                                      {QStringLiteral("cover"), stored.cover},
                                      {QStringLiteral("songs"), QJsonArray::fromStringList(stored.uris)}});
    }
    QJsonArray copies;
    for (auto it = m_copies.cbegin(); it != m_copies.cend(); ++it) {
        copies.append(QJsonObject {{QStringLiteral("copy"), it.key()},
                                   {QStringLiteral("source"), it->source},
                                   {QStringLiteral("size"), it->size},
                                   {QStringLiteral("modified"), it->modified}});
    }
    QSaveFile file(m_playlistsPath);
    if (!file.open(QIODevice::WriteOnly)
        || file.write(QJsonDocument(QJsonObject {{QStringLiteral("version"), kPlaylistsVersion},
                                                 {QStringLiteral("playlists"), playlists},
                                                 {QStringLiteral("copies"), copies}})
                          .toJson(QJsonDocument::Compact))
            < 0
        || !file.commit()) {
        qCWarning(lcLocal).noquote() << "could not keep the playlists in" << m_playlistsPath << ':' << file.errorString();
    }
}

void LocalLibrary::loadPlaylists()
{
    QSqlDatabase database = QSqlDatabase::database(m_databaseName, false);
    if (!database.isOpen()) {
        readJsonPlaylists();
        return;
    }
    QSqlQuery query(database);
    if (query.exec(QStringLiteral("SELECT id, name, description, cover FROM playlists ORDER BY position"))) {
        while (query.next()) {
            StoredPlaylist stored {query.value(0).toString(), query.value(1).toString(), query.value(2).toString(),
                                   query.value(3).toString(), {}};
            if (!stored.id.startsWith(kLocalPrefix) || findPlaylist(stored.id)) {
                continue;
            }
            m_storedPlaylists.append(stored);
        }
    }
    QSqlQuery songs(database);
    if (songs.exec(QStringLiteral("SELECT playlist_id, uri FROM playlist_songs ORDER BY playlist_id, position"))) {
        while (songs.next()) {
            if (StoredPlaylist *stored = findPlaylist(songs.value(0).toString())) {
                stored->uris.append(songs.value(1).toString());
            }
        }
    }
    QSqlQuery copies(database);
    if (copies.exec(QStringLiteral("SELECT copy, source, size, modified FROM copies"))) {
        while (copies.next()) {
            const QString copy = copies.value(0).toString();
            if (isKeptCopy(copy)) {
                m_copies.insert(copy, {copies.value(1).toString(), copies.value(2).toLongLong(),
                                       copies.value(3).toLongLong()});
            }
        }
    }
    // what the database holds now: a save writes only what differs from it
    m_savedPlaylists.clear();
    m_savedOrder.clear();
    for (const StoredPlaylist &stored : std::as_const(m_storedPlaylists)) {
        m_savedPlaylists.insert(stored.id, contentHash(stored));
        m_savedOrder.append(stored.id);
    }
    m_savedCopies = copiesHash(m_copies);
    m_copiesSaved = true;
}

void LocalLibrary::savePlaylists() const
{
    QSqlDatabase database = QSqlDatabase::database(m_databaseName, false);
    if (!database.isOpen()) {
        writeJsonPlaylists();
        return;
    }
    // only what differs from what the database holds is written, and all of it
    // in one transaction: it is all there afterwards, or none of it was touched
    bool ok = database.transaction();
    QSqlQuery query(database);
    QStringList order;
    QSet<QString> present;
    for (const StoredPlaylist &stored : m_storedPlaylists) {
        order.append(stored.id);
        present.insert(stored.id);
    }
    QHash<QString, size_t> state;
    for (const QString &id : m_savedPlaylists.keys()) {
        if (!present.contains(id) && ok) {
            ok = query.prepare(QStringLiteral("DELETE FROM playlist_songs WHERE playlist_id = ?"));
            query.addBindValue(id);
            ok = ok && query.exec();
            ok = ok && query.prepare(QStringLiteral("DELETE FROM playlists WHERE id = ?"));
            query.addBindValue(id);
            ok = ok && query.exec();
        }
    }
    int position = 0;
    for (const StoredPlaylist &stored : m_storedPlaylists) {
        const size_t hash = contentHash(stored);
        state.insert(stored.id, hash);
        if (ok && m_savedPlaylists.value(stored.id, hash + 1) != hash) {
            ok = query.prepare(QStringLiteral(
                "INSERT INTO playlists (id, name, description, cover, position) VALUES (?, ?, ?, ?, ?) "
                "ON CONFLICT(id) DO UPDATE SET name = excluded.name, description = excluded.description, "
                "cover = excluded.cover, position = excluded.position"));
            query.addBindValue(stored.id);
            query.addBindValue(stored.name);
            query.addBindValue(stored.description);
            query.addBindValue(stored.cover);
            query.addBindValue(position);
            ok = ok && query.exec();
            ok = ok && query.prepare(QStringLiteral("DELETE FROM playlist_songs WHERE playlist_id = ?"));
            query.addBindValue(stored.id);
            ok = ok && query.exec();
            if (ok && !stored.uris.isEmpty()) {
                QVariantList ids;
                QVariantList positions;
                QVariantList uris;
                for (int row = 0; row < stored.uris.size(); ++row) {
                    ids.append(stored.id);
                    positions.append(row);
                    uris.append(stored.uris.at(row));
                }
                ok = query.prepare(QStringLiteral("INSERT INTO playlist_songs (playlist_id, position, uri) VALUES (?, ?, ?)"));
                query.addBindValue(ids);
                query.addBindValue(positions);
                query.addBindValue(uris);
                ok = ok && query.execBatch();
            }
        }
        ++position;
    }
    if (ok && order != m_savedOrder) {
        // a playlist made or taken away moves the others
        QVariantList positions;
        QVariantList ids;
        for (int index = 0; index < order.size(); ++index) {
            positions.append(index);
            ids.append(order.at(index));
        }
        if (!ids.isEmpty()) {
            ok = query.prepare(QStringLiteral("UPDATE playlists SET position = ? WHERE id = ?"));
            query.addBindValue(positions);
            query.addBindValue(ids);
            ok = ok && query.execBatch();
        }
    }
    const size_t copies = copiesHash(m_copies);
    const bool copiesChanged = !m_copiesSaved || copies != m_savedCopies;
    if (ok && copiesChanged) {
        ok = query.exec(QStringLiteral("DELETE FROM copies"));
        if (ok && !m_copies.isEmpty()) {
            QVariantList names;
            QVariantList sources;
            QVariantList sizes;
            QVariantList modifieds;
            for (auto it = m_copies.cbegin(); it != m_copies.cend(); ++it) {
                names.append(it.key());
                sources.append(it->source);
                sizes.append(it->size);
                modifieds.append(it->modified);
            }
            ok = query.prepare(QStringLiteral("INSERT INTO copies (copy, source, size, modified) VALUES (?, ?, ?, ?)"));
            query.addBindValue(names);
            query.addBindValue(sources);
            query.addBindValue(sizes);
            query.addBindValue(modifieds);
            ok = ok && query.execBatch();
        }
    }
    if (!ok || !database.commit()) {
        qCWarning(lcLocal).noquote() << "could not keep the playlists in" << m_databasePath << ':' << query.lastError().text();
        database.rollback();
        return;
    }
    m_savedPlaylists = state;
    m_savedOrder = order;
    m_savedCopies = copies;
    m_copiesSaved = true;
}

void LocalLibrary::publishPlaylists()
{
    const SongIndex index = indexSongs();
    QList<Playlist> playlists;
    playlists.reserve(m_storedPlaylists.size());
    for (const StoredPlaylist &stored : std::as_const(m_storedPlaylists)) {
        playlists.append(describe(stored, index));
    }
    m_playlists->setItems(playlists);
    emit playlistsChanged();
}

LocalLibrary::StoredPlaylist *LocalLibrary::findPlaylist(const QString &id)
{
    const auto found = std::find_if(m_storedPlaylists.begin(), m_storedPlaylists.end(),
                                    [&](const StoredPlaylist &stored) { return stored.id == id; });
    return found == m_storedPlaylists.end() ? nullptr : &*found;
}

const LocalLibrary::StoredPlaylist *LocalLibrary::findPlaylist(const QString &id) const
{
    return const_cast<LocalLibrary *>(this)->findPlaylist(id);
}

LocalLibrary::SongIndex LocalLibrary::indexSongs() const
{
    SongIndex index;
    const QList<Song> &library = m_songs->songs();
    index.reserve(library.size() + m_storeSongs.size());
    for (const Song &song : library) {
        index.insert(song.uri, &song);
    }
    for (auto kept = m_storeSongs.cbegin(); kept != m_storeSongs.cend(); ++kept) {
        index.insert(kept.key(), &kept.value());
    }
    return index;
}

QList<Song> LocalLibrary::resolve(const StoredPlaylist &stored, const SongIndex &index) const
{
    QList<Song> songs;
    songs.reserve(stored.uris.size());
    for (const QString &uri : stored.uris) {
        if (const Song *song = index.value(uri)) {
            songs.append(song->freshInstance());
        }
    }
    return songs;
}

Playlist LocalLibrary::describe(const StoredPlaylist &stored, const SongIndex &index) const
{
    Playlist playlist;
    playlist.id = stored.id;
    playlist.uri = stored.id;
    playlist.name = stored.name;
    playlist.description = stored.description;
    playlist.ownerName = QStringLiteral("Local Files");
    const QList<Song> songs = resolve(stored, index);
    playlist.songCount = int(songs.size());
    if (!stored.cover.isEmpty()) {
        playlist.artworkUrl = QUrl::fromLocalFile(m_coverDirectory + u'/' + stored.cover);
    } else {
        // the first cover among its songs, for a playlist without one of its own
        for (const Song &song : songs) {
            if (!song.artworkUrl.isEmpty()) {
                playlist.artworkUrl = song.artworkUrl;
                break;
            }
        }
    }
    return playlist;
}

Playlist LocalLibrary::playlist(const QString &id) const
{
    // as last published, which every change is
    for (const Playlist &playlist : m_playlists->items()) {
        if (playlist.id == id) {
            return playlist;
        }
    }
    return {};
}

QVariantList LocalLibrary::playlistChoices() const
{
    QVariantList choices;
    for (const Playlist &playlist : m_playlists->items()) {
        choices.append(QVariant::fromValue(playlist));
    }
    return choices;
}

QList<Song> LocalLibrary::songsOf(const QString &id) const
{
    const StoredPlaylist *stored = findPlaylist(id);
    return stored ? resolve(*stored, indexSongs()) : QList<Song> {};
}

QString LocalLibrary::writeCover(const StoredPlaylist &stored, const QByteArray &jpeg) const
{
    QDir().mkpath(m_coverDirectory);
    const QString name = stored.id.mid(kLocalPrefix.size()) + u'-'
        + QUuid::createUuid().toString(QUuid::Id128).left(8) + QStringLiteral(".jpg");
    QSaveFile file(m_coverDirectory + u'/' + name);
    if (!file.open(QIODevice::WriteOnly) || file.write(jpeg) != jpeg.size() || !file.commit()) {
        qCWarning(lcLocal).noquote() << "could not keep the cover for" << stored.name << ':' << file.errorString();
        return stored.cover;
    }
    if (!stored.cover.isEmpty()) {
        QFile::remove(m_coverDirectory + u'/' + stored.cover);
    }
    return name;
}

Playlist LocalLibrary::createPlaylist(const QString &name, const QString &description, const QByteArray &coverJpeg,
                                      const QList<Song> &songs)
{
    StoredPlaylist stored {kLocalPrefix + QUuid::createUuid().toString(QUuid::WithoutBraces), name.trimmed(),
                           description.trimmed(), {}, {}};
    if (stored.name.isEmpty()) {
        return {};
    }
    if (!coverJpeg.isEmpty()) {
        stored.cover = writeCover(stored, coverJpeg);
    }
    // newest first
    m_storedPlaylists.prepend(stored);
    savePlaylists();
    publishPlaylists();
    addSongs(stored.id, songs);
    return playlist(stored.id);
}

bool LocalLibrary::updatePlaylist(const QString &id, const QString &name, const QString &description,
                                  const QByteArray &coverJpeg)
{
    StoredPlaylist *stored = findPlaylist(id);
    if (!stored || name.trimmed().isEmpty()) {
        return false;
    }
    stored->name = name.trimmed();
    stored->description = description.trimmed();
    if (!coverJpeg.isEmpty()) {
        stored->cover = writeCover(*stored, coverJpeg);
    }
    savePlaylists();
    publishPlaylists();
    return true;
}

bool LocalLibrary::deletePlaylist(const QString &id)
{
    const StoredPlaylist *stored = findPlaylist(id);
    if (!stored) {
        return false;
    }
    if (!stored->cover.isEmpty()) {
        QFile::remove(m_coverDirectory + u'/' + stored->cover);
    }
    m_storedPlaylists.removeIf([&](const StoredPlaylist &item) { return item.id == id; });
    collectCopies();
    savePlaylists();
    publishPlaylists();
    return true;
}

int LocalLibrary::addSongs(const QString &id, const QList<Song> &songs)
{
    StoredPlaylist *stored = findPlaylist(id);
    if (!stored) {
        return 0;
    }
    // the playlist holds where each song is, and nothing is copied
    int added = 0;
    for (const Song &song : songs) {
        if (song.isLocalFile()) {
            stored->uris.append(song.uri);
            ++added;
        }
    }
    if (added > 0) {
        savePlaylists();
        publishPlaylists();
    }
    return added;
}

void LocalLibrary::addFiles(const QString &playlistId, const QList<QUrl> &urls)
{
    StoredPlaylist *stored = findPlaylist(playlistId);
    QList<QUrl> sources;
    QStringList paths;
    QStringList skipped;
    for (const QUrl &url : urls) {
        if (!url.isLocalFile()) {
            continue;
        }
        const QFileInfo info(url.toLocalFile());
        if (info.isFile() && isMusicFile(info.filePath())) {
            sources.append(url);
            paths.append(info.canonicalFilePath());
        } else if (info.isDir()) {
            // every music file inside, in the order of a listing
            QStringList inside;
            QDirIterator files(info.canonicalFilePath(), QDir::Files, QDirIterator::Subdirectories);
            while (files.hasNext()) {
                const QString path = files.next();
                if (isMusicFile(path)) {
                    inside.append(path);
                }
            }
            QCollator collator;
            collator.setNumericMode(true);
            std::sort(inside.begin(), inside.end(), [&](const QString &left, const QString &right) {
                return collator.compare(left, right) < 0;
            });
            if (!inside.isEmpty()) {
                sources.append(url);
                paths.append(inside);
            }
        } else {
            skipped.append(info.fileName());
        }
    }
    if (!stored || paths.isEmpty()) {
        emit tracksAdded(playlistId, 0, QStringLiteral("There was nothing there to add."));
        return;
    }
    // the library knows songs from its folders and files: one from elsewhere joins them
    addSources(sources);
    for (const QString &path : std::as_const(paths)) {
        stored->uris.append(QUrl::fromLocalFile(path).toString());
    }
    savePlaylists();
    publishPlaylists();
    QString problem;
    if (skipped.size() == 1) {
        problem = QStringLiteral("“%1” was not music, so it was left out.").arg(skipped.first());
    } else if (!skipped.isEmpty()) {
        problem = QStringLiteral("%1 files were not music, so they were left out.").arg(skipped.size());
    }
    emit tracksAdded(playlistId, int(paths.size()), problem);
}

int LocalLibrary::removeSong(const QString &id, const QString &uri)
{
    StoredPlaylist *stored = findPlaylist(id);
    const int removed = stored ? int(stored->uris.removeAll(uri)) : 0;
    if (removed > 0) {
        collectCopies();
        savePlaylists();
        publishPlaylists();
    }
    return removed;
}

// --- copies from older versions --------------------------------------------------------

void LocalLibrary::moveBackToOriginals()
{
    bool changed = false;
    QStringList sources = m_sources;
    for (StoredPlaylist &stored : m_storedPlaylists) {
        for (QString &uri : stored.uris) {
            const QString path = QUrl(uri).toLocalFile();
            const auto copy = isKeptCopy(path) ? m_copies.constFind(path) : m_copies.cend();
            if (copy == m_copies.cend() || copy->source.isEmpty() || !QFileInfo(copy->source).isFile()) {
                // not a copy, or one whose original is gone: it stays as it is
                continue;
            }
            uri = QUrl::fromLocalFile(copy->source).toString();
            changed = true;
            // songs are known from the library's folders, so the original joins them
            if (!std::any_of(sources.begin(), sources.end(), [&](const QString &source) { return isWithin(copy->source, source); })) {
                sources.append(copy->source);
            }
        }
    }
    if (!changed) {
        return;
    }
    setSources(sources);
    // the copies nothing holds now are deleted, with the lyrics and cover beside them
    collectCopies();
    savePlaylists();
}

void LocalLibrary::collectCopies()
{
    QSet<QString> held;
    for (const StoredPlaylist &stored : std::as_const(m_storedPlaylists)) {
        for (const QString &uri : stored.uris) {
            held.insert(QUrl(uri).toLocalFile());
        }
    }
    const QDir store(m_trackStore);
    for (auto it = m_copies.begin(); it != m_copies.end();) {
        if (held.contains(it.key())) {
            ++it;
            continue;
        }
        // the folder it was given goes whole, its lyrics and cover with it
        const QString folder = store.relativeFilePath(it.key()).section(u'/', 0, 0);
        if (!folder.isEmpty() && folder != QLatin1StringView("..")) {
            QDir(store.filePath(folder)).removeRecursively();
        }
        m_entries.remove(it.key());
        m_storeSongs.remove(QUrl::fromLocalFile(it.key()).toString());
        it = m_copies.erase(it);
    }
}

void LocalLibrary::sweepStore()
{
    // a folder a copy was being written into when the app stopped, or one
    // whose playlist went while it was away
    const QDir store(m_trackStore);
    QSet<QString> known;
    const auto folderOf = [&](const QString &path) {
        const QString relative = store.relativeFilePath(path);
        return relative.startsWith(QLatin1StringView("..")) ? QString() : relative.section(u'/', 0, 0);
    };
    for (auto it = m_copies.cbegin(); it != m_copies.cend(); ++it) {
        known.insert(folderOf(it.key()));
    }
    for (const StoredPlaylist &stored : std::as_const(m_storedPlaylists)) {
        for (const QString &uri : stored.uris) {
            known.insert(folderOf(QUrl(uri).toLocalFile()));
        }
    }
    for (const QString &folder : store.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (!known.contains(folder)) {
            QDir(store.filePath(folder)).removeRecursively();
        }
    }
}

void LocalLibrary::reorder(const QString &id, const QStringList &uris)
{
    StoredPlaylist *stored = findPlaylist(id);
    if (!stored) {
        return;
    }
    // only what it holds, as often as it holds it; what the library no
    // longer finds keeps its place after the rest
    QHash<QString, int> held;
    for (const QString &uri : std::as_const(stored->uris)) {
        ++held[uri];
    }
    QStringList ordered;
    ordered.reserve(stored->uris.size());
    for (const QString &uri : uris) {
        if (int &left = held[uri]; left > 0) {
            --left;
            ordered.append(uri);
        }
    }
    for (const QString &uri : std::as_const(stored->uris)) {
        if (int &left = held[uri]; left > 0) {
            --left;
            ordered.append(uri);
        }
    }
    if (ordered == stored->uris) {
        return;
    }
    stored->uris = ordered;
    savePlaylists();
    publishPlaylists();
}

} // namespace swifty
