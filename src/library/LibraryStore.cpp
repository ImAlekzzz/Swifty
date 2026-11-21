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

#include "library/LibraryStore.h"

#include "app/ErrorCenter.h"
#include "app/Settings.h"
#include "core/Diagnostics.h"
#include "library/ArtistImages.h"
#include "library/LocalLibrary.h"
#include "playback/PlaybackController.h"

#include <QCollator>
#include <QElapsedTimer>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QMutex>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QQmlEngine>

#include <algorithm>
#include <limits>
#include <utility>

namespace swifty {

Q_LOGGING_CATEGORY(lcLibrary, "swifty.library")

namespace {

LibraryStore *s_instance = nullptr;

const QString kPinnedKey = QStringLiteral("pinnedPlaylistIDs");
const QString kSearchHistoryKey = QStringLiteral("searchPlaybackHistory");
const QString kSavedKey = QStringLiteral("favoriteSongUris");
const QString kRecentKey = QStringLiteral("recentlyPlayedUris");
const QString kPlayCountsKey = QStringLiteral("playCounts");
constexpr int kMostPlayedLimit = 12;
const QString kCatchphrase = QStringLiteral("i mean i guess bro");
constexpr int kSearchHistoryLimit = 20;
constexpr int kRecentLimit = 30;
constexpr int kRecommendationLimit = 12;
constexpr int kSearchSongLimit = 60;
constexpr int kSearchGroupLimit = 12;
constexpr int kArtistSongLimit = 50;

/// a playlist made on this computer (LocalLibrary)
bool isLocalId(const QString &id)
{
    return id.startsWith(QLatin1StringView("local:"));
}

/// `next`, with each occurrence `previous` already had kept as it was, in
/// order: the rows stay where they are and the song playing stays marked
QList<Song> keepingInstances(const QList<Song> &previous, QList<Song> next)
{
    QHash<QString, QList<quint64>> held;
    for (const Song &song : previous) {
        held[song.uri].append(song.instanceId);
    }
    for (Song &song : next) {
        const auto found = held.find(song.uri);
        if (found != held.end() && !found->isEmpty()) {
            song.instanceId = found->takeFirst();
        }
    }
    return next;
}

bool sameSongs(const QList<Song> &left, const QList<Song> &right)
{
    return std::equal(left.begin(), left.end(), right.begin(), right.end(), [](const Song &a, const Song &b) {
        return a.uri == b.uri && a.name == b.name && a.artists == b.artists && a.albumName == b.albumName
            && a.artworkUrl == b.artworkUrl && a.durationMs == b.durationMs;
    });
}

/// the library's songs, each track once. a file added to a playlist is copied
/// for it, and the copy is as much a song of the library as the file it came
/// from: told apart by their tags and length, the first one found is kept,
/// and the scanned files come before the copies
QList<Song> uniqueSongs(const QList<Song> &songs)
{
    QSet<QString> seen;
    QList<Song> unique;
    unique.reserve(songs.size());
    for (const Song &song : songs) {
        const QString key = song.name.toLower() + QChar(0x1f) + song.artists.toLower() + QChar(0x1f)
            + song.albumName.toLower() + QChar(0x1f) + QString::number(song.durationMs / 1000);
        if (!seen.contains(key)) {
            seen.insert(key);
            unique.append(song);
        }
    }
    return unique;
}

QString hashedId(const QString &prefix, const QString &key)
{
    // the same names come up again on every refresh of the library, and a hash is
    // slow on some systems (openssl 3 and later look the algorithm up on every call:
    // thousands of them held the window for seconds). so each is made once. the id
    // itself must stay as it was: pinned artists are kept by it
    static QHash<QString, QString> known;
    static QMutex mutex;
    const QString cacheKey = prefix + QChar(0x1e) + key;
    {
        const QMutexLocker locker(&mutex);
        if (const auto found = known.constFind(cacheKey); found != known.cend()) {
            return *found;
        }
    }
    const QByteArray digest = QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1);
    const QString id = prefix + QString::fromLatin1(digest.toHex().left(16));
    const QMutexLocker locker(&mutex);
    known.insert(cacheKey, id);
    return id;
}

/// the names an artists line holds: "A, B" is two
QStringList artistNames(const Song &song)
{
    QStringList names;
    for (const QString &part : song.artists.split(QStringLiteral(", "), Qt::SkipEmptyParts)) {
        const QString name = part.trimmed();
        if (!name.isEmpty() && !names.contains(name, Qt::CaseInsensitive)) {
            names.append(name);
        }
    }
    return names;
}

QString primaryArtistName(const Song &song)
{
    const QStringList names = artistNames(song);
    return names.isEmpty() ? QString() : names.first();
}

Artist artistNamed(const QString &name, const QUrl &artwork = {})
{
    Artist artist;
    artist.id = hashedId(QStringLiteral("artist:"), name.toLower());
    artist.name = name;
    artist.artworkUrl = artwork;
    return artist;
}

QString albumIdOf(const Song &song)
{
    return hashedId(QStringLiteral("album:"), song.albumName.toLower() + QChar(0x1f) + primaryArtistName(song).toLower());
}

Album albumOf(const Song &song)
{
    if (song.albumName.isEmpty()) {
        return {};
    }
    Album album;
    album.id = albumIdOf(song);
    album.name = song.albumName;
    album.artists = primaryArtistName(song);
    album.artworkUrl = song.artworkUrl;
    return album;
}

using Catalog = LibraryCatalog;

Catalog buildCatalog(const QList<Song> &songs)
{
    QCollator collator;
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    collator.setNumericMode(true);

    Catalog catalog;
    QHash<QString, int> albumIndex;
    QHash<QString, int> artistIndex;
    for (const Song &song : songs) {
        if (const Album album = albumOf(song); album.isValid()) {
            auto found = albumIndex.find(album.id);
            if (found == albumIndex.end()) {
                found = albumIndex.insert(album.id, int(catalog.albums.size()));
                catalog.albums.append(album);
            }
            Album &entry = catalog.albums[found.value()];
            ++entry.songCount;
            if (entry.artworkUrl.isEmpty()) {
                entry.artworkUrl = song.artworkUrl;
            }
            catalog.songsOfAlbum[album.id].append(song);
        }
        for (const QString &name : artistNames(song)) {
            const Artist artist = artistNamed(name, song.artworkUrl);
            auto found = artistIndex.find(artist.id);
            if (found == artistIndex.end()) {
                found = artistIndex.insert(artist.id, int(catalog.artists.size()));
                catalog.artists.append(artist);
            }
            Artist &entry = catalog.artists[found.value()];
            if (entry.artworkUrl.isEmpty()) {
                entry.artworkUrl = song.artworkUrl;
            }
            catalog.songsOfArtist[artist.id].append(song);
        }
    }
    // a file name usually starts with the track number, so the path is the
    // closest thing to the album's own order
    for (auto it = catalog.songsOfAlbum.begin(); it != catalog.songsOfAlbum.end(); ++it) {
        std::stable_sort(it->begin(), it->end(), [&collator](const Song &left, const Song &right) {
            return collator.compare(left.uri, right.uri) < 0;
        });
    }
    for (auto it = catalog.songsOfArtist.begin(); it != catalog.songsOfArtist.end(); ++it) {
        std::stable_sort(it->begin(), it->end(), [&collator](const Song &left, const Song &right) {
            const int byAlbum = collator.compare(left.albumName, right.albumName);
            return byAlbum != 0 ? byAlbum < 0 : collator.compare(left.uri, right.uri) < 0;
        });
    }
    std::stable_sort(catalog.albums.begin(), catalog.albums.end(), [&collator](const Album &left, const Album &right) {
        return collator.compare(left.name, right.name) < 0;
    });
    std::stable_sort(catalog.artists.begin(), catalog.artists.end(), [&collator](const Artist &left, const Artist &right) {
        return collator.compare(left.name, right.name) < 0;
    });
    return catalog;
}

bool matches(const QString &text, const QString &query)
{
    return text.contains(query, Qt::CaseInsensitive);
}

/// letters and digits only, lowercase
QString flattened(const QString &text)
{
    QString result;
    for (const QChar c : text) {
        if (c.isLetterOrNumber()) {
            result.append(c.toLower());
        }
    }
    return result;
}

} // namespace

bool LibraryStore::songMatches(const Song &song, const QString &query)
{
    if (matches(song.name, query) || matches(song.artists, query) || matches(song.albumName, query)
        || matches(song.genre, query)) {
        return true;
    }
    // "hip hop" and "hiphop" are "Hip-Hop" too
    const QString flat = flattened(query);
    return flat.size() >= 3 && flattened(song.genre).contains(flat);
}

bool LibraryStore::Destination::operator==(const Destination &other) const
{
    if (page != other.page) {
        return false;
    }
    switch (page) {
    case Page::Playlist: return playlistId == other.playlistId;
    case Page::Album: return album.id == other.album.id;
    case Page::Artist: return artist.id == other.artist.id;
    default: return true;
    }
}

LibraryStore::LibraryStore(Settings *settings, ErrorCenter *errors, QObject *parent)
    : QObject(parent)
    , m_settings(settings)
    , m_errors(errors)
    , m_playlists(new PlaylistListModel(this))
    , m_pinnedPlaylists(new PlaylistListModel(this))
    , m_playlistSongs(new SongListModel(this))
    , m_displayedPlaylistSongs(new SongListModel(this))
    , m_displayedAllSongs(new SongListModel(this))
    , m_displayedSavedSongs(new SongListModel(this))
    , m_displayedAlbums(new AlbumListModel(this))
    , m_displayedArtists(new ArtistListModel(this))
    , m_savedSongs(new SongListModel(this))
    , m_allSongs(new SongListModel(this))
    , m_recentlyPlayed(new SongListModel(this))
    , m_mostPlayed(new SongListModel(this))
    , m_recommendedArtists(new ArtistListModel(this))
    , m_recommendedAlbums(new AlbumListModel(this))
    , m_allAlbums(new AlbumListModel(this))
    , m_albumSongs(new SongListModel(this))
    , m_artistTopSongs(new SongListModel(this))
    , m_artistAlbums(new AlbumListModel(this))
    , m_searchSongs(new SongListModel(this))
    , m_searchAlbums(new AlbumListModel(this))
    , m_searchArtists(new ArtistListModel(this))
    , m_searchPlaylists(new PlaylistListModel(this))
    , m_searchHistory(new SongListModel(this))
    , m_searchHistoryPreview(new SongListModel(this))
{
    if (!s_instance) {
        s_instance = this;
    }

    const QStringList pinned = m_settings->value(kPinnedKey).toStringList();
    m_pinnedIds = QSet<QString>(pinned.begin(), pinned.end());

    m_savedOrder = m_settings->value(kSavedKey).toStringList();
    m_savedUris = QSet<QString>(m_savedOrder.begin(), m_savedOrder.end());
    m_recentUris = m_settings->value(kRecentKey).toStringList();
    const QJsonObject counts = QJsonDocument::fromJson(m_settings->value(kPlayCountsKey).toByteArray()).object();
    for (auto it = counts.begin(); it != counts.end(); ++it) {
        if (it.value().toInt() > 0) {
            m_playCounts.insert(it.key(), it.value().toInt());
        }
    }

    QList<Song> history;
    for (const QJsonValue &value : QJsonDocument::fromJson(m_settings->value(kSearchHistoryKey).toByteArray()).array()) {
        const Song song = Song::fromJson(value.toObject());
        if (song.isValid()) {
            history.append(song);
        }
    }
    setSearchHistory(history);

    connect(m_savedSongs, &SongListModel::countChanged, this, &LibraryStore::savedSongTotalChanged);
    m_refreshTimer.setSingleShot(true);
    m_refreshTimer.setInterval(50);
    connect(&m_refreshTimer, &QTimer::timeout, this, &LibraryStore::libraryDidChange);
    // pictures arrive one after another while the app is left alone, and the library
    // is made again for each: at most one in two seconds
    m_imageRefreshTimer.setSingleShot(true);
    m_imageRefreshTimer.setInterval(2000);
    connect(&m_imageRefreshTimer, &QTimer::timeout, this, &LibraryStore::libraryDidChange);
}

LibraryStore::~LibraryStore()
{
    if (s_instance == this) {
        s_instance = nullptr;
    }
}

LibraryStore *LibraryStore::instance()
{
    return s_instance;
}

LibraryStore *LibraryStore::create(QQmlEngine *, QJSEngine *)
{
    QJSEngine::setObjectOwnership(s_instance, QJSEngine::CppOwnership);
    return s_instance;
}

void LibraryStore::attachPlayer(PlaybackController *player)
{
    connect(player, &PlaybackController::playedFromSearch, this, &LibraryStore::recordSearchPlayback);
    connect(player, &PlaybackController::currentSongChanged, this, [this, player] {
        if (player->hasSong()) {
            rememberPlayed(player->currentSong());
        }
    });
    connect(player, &PlaybackController::livePositionChanged, this, [this, player] {
        if (player->hasSong()) {
            notePlayProgress(player->currentSong(), player->livePositionMs(), quint32(std::max(player->durationMs(), 0)), player->isPlaying());
        }
    });
}

void LibraryStore::notePlayProgress(const Song &song, quint32 positionMs, quint32 durationMs, bool playing)
{
    if (!song.isLocalFile()) {
        return;
    }
    const quint32 before = std::exchange(m_lastProgressMs, positionMs);
    if (song.instanceId != m_countingInstance) {
        // another song: heard from here
        m_countingInstance = song.instanceId;
        m_listenedMs = 0;
        m_countedThisPlay = false;
        return;
    }
    if (positionMs < 3000 && before > positionMs + 3000) {
        // it went back to the start: played over again, or looping
        m_listenedMs = 0;
        m_countedThisPlay = false;
        return;
    }
    // only time that passed in small steps was listened to: a jump is a seek
    if (!playing || positionMs <= before || positionMs - before > 2500) {
        return;
    }
    m_listenedMs += positionMs - before;
    if (m_countedThisPlay) {
        return;
    }
    const quint32 enough = durationMs > 0 ? std::min<quint32>(30'000, std::max<quint32>(5'000, durationMs / 2)) : 30'000;
    if (m_listenedMs < enough) {
        return;
    }
    m_countedThisPlay = true;
    ++m_playCounts[song.uri];
    QJsonObject counts;
    for (auto it = m_playCounts.cbegin(); it != m_playCounts.cend(); ++it) {
        counts.insert(it.key(), it.value());
    }
    m_settings->setValue(kPlayCountsKey, QJsonDocument(counts).toJson(QJsonDocument::Compact));
    refreshMostPlayed();
}

void LibraryStore::attachLocalFiles(LocalLibrary *localFiles)
{
    m_localFiles = localFiles;
    connect(localFiles, &LocalLibrary::playlistsChanged, this, [this] {
        m_snapshot.reset();
        refreshPlaylists();
        refreshLocalPlaylist();
        m_refreshTimer.start();
    });
    // the files themselves: a scan, a tag edit, files coming or going. what was
    // worked out of the library is dropped at once, whoever asks before the refresh
    const auto changed = [this] {
        m_snapshot.reset();
        m_refreshTimer.start();
    };
    connect(localFiles, &LocalLibrary::scanned, this, changed);
    connect(localFiles, &LocalLibrary::tagsSaved, this, changed);
    connect(localFiles->songs(), &SongListModel::countChanged, this, changed);
    connect(localFiles, &LocalLibrary::sourcesChanged, this, [this] {
        if (m_artistImages) {
            m_artistImages->setLibraryFolders(m_localFiles->sources());
        }
        m_snapshot.reset();
        m_refreshTimer.start();
    });
    if (m_artistImages) {
        m_artistImages->setLibraryFolders(localFiles->sources());
    }
    refreshPlaylists();
    libraryDidChange();
}

void LibraryStore::setArtistImages(ArtistImages *images)
{
    m_artistImages = images;
    if (!images) {
        return;
    }
    // a picture arriving changes what the artists look like
    connect(images, &ArtistImages::imageChanged, this, [this] {
        if (!m_imageRefreshTimer.isActive()) {
            m_imageRefreshTimer.start();
        }
    });
    if (m_localFiles) {
        images->setLibraryFolders(m_localFiles->sources());
        m_refreshTimer.start();
    }
}

void LibraryStore::applyArtistPictures(QList<Artist> &artists, const QHash<QString, QList<Song>> &songsOfArtist) const
{
    if (!m_artistImages) {
        return;
    }
    for (Artist &artist : artists) {
        QStringList files;
        for (const Song &song : songsOfArtist.value(artist.id)) {
            if (song.isLocalFile()) {
                files.append(song.localPath());
                if (files.size() >= 8) {
                    break;
                }
            }
        }
        if (const QUrl picture = m_artistImages->imageFor(artist.name, files); picture.isValid()) {
            artist.artworkUrl = picture;
        }
    }
}

void LibraryStore::reportError(const QString &context, const QString &message)
{
    qCWarning(lcLibrary).noquote() << context << ':' << message;
    m_errors->show(message);
}

// --- derived from the music files -----------------------------------------------------------

void LibraryStore::libraryDidChange()
{
    QElapsedTimer clock;
    clock.start();
    // what was worked out of the old library is of no use now
    m_snapshot.reset();
    refreshAllSongs();
    refreshSaved();
    refreshRecents();
    refreshRecommendations();
    refreshAlbumPage();
    refreshArtistPage();
    refreshSearch();
    diagnostic("library", QStringLiteral("the lists were refreshed in %1 ms (%2 songs)").arg(clock.elapsed()).arg(snapshot()->songs.size()));
}

void LibraryStore::refreshPlaylists()
{
    if (!m_localFiles) {
        return;
    }
    m_playlists->setItems(m_localFiles->playlists()->items());
    refreshPinnedPlaylists();
    emit selectedPlaylistChanged();
}

void LibraryStore::refreshLocalPlaylist()
{
    const QString id = selectedPlaylistId();
    if (!isLocalId(id) || !m_localFiles) {
        return;
    }
    // its name, cover or count may be what changed
    emit selectedPlaylistChanged();
    const QList<Song> next = m_localFiles->songsOf(id);
    if (!sameSongs(next, m_songs)) {
        setSongs(keepingInstances(m_songs, next));
    }
}

std::shared_ptr<LibrarySnapshot> LibraryStore::snapshot()
{
    if (!m_snapshot) {
        auto library = std::make_shared<LibrarySnapshot>();
        if (m_localFiles) {
            library->songs = uniqueSongs(m_localFiles->allSongs());
            library->byUri.reserve(library->songs.size());
            for (const Song &song : std::as_const(library->songs)) {
                library->byUri.insert(song.uri, song);
            }
        }
        m_snapshot = std::move(library);
    }
    return m_snapshot;
}

const LibraryCatalog &LibraryStore::catalogOf(LibrarySnapshot &library)
{
    if (!library.catalog) {
        library.catalog = buildCatalog(library.songs);
    }
    return *library.catalog;
}

void LibraryStore::refreshDisplayedList(SongListModel *model, const QList<Song> &source)
{
    // the long lists keep their place when what they hold has not changed
    const QList<Song> next = keepingInstances(model->songs(), arranged(source));
    if (!sameSongs(next, model->songs())) {
        model->setSongs(next);
    }
}

void LibraryStore::refreshAllSongs()
{
    if (!m_localFiles) {
        return;
    }
    const auto library = snapshot();
    const QList<Song> every = keepingInstances(m_allSongs->songs(), library->songs);
    if (!sameSongs(every, m_allSongs->songs())) {
        m_allSongs->setSongs(every);
    }
    refreshDisplayedList(m_displayedAllSongs, m_allSongs->songs());
}

void LibraryStore::refreshSaved()
{
    if (!m_localFiles) {
        return;
    }
    const auto library = snapshot();
    // a file that is not there now (a drive that is away) stays a favorite
    QList<Song> songs;
    for (const QString &uri : std::as_const(m_savedOrder)) {
        if (const auto found = library->byUri.constFind(uri); found != library->byUri.cend()) {
            songs.append(*found);
        }
    }
    const QList<Song> next = keepingInstances(m_savedSongs->songs(), songs);
    if (!sameSongs(next, m_savedSongs->songs())) {
        m_savedSongs->setSongs(next);
    }
    // only the favourites' own page: a star changes nothing else
    refreshDisplayedList(m_displayedSavedSongs, m_savedSongs->songs());
}

void LibraryStore::refreshRecents()
{
    if (!m_localFiles) {
        return;
    }
    const auto library = snapshot();
    const QHash<QString, Song> &byUri = library->byUri;
    QList<Song> songs;
    for (const QString &uri : std::as_const(m_recentUris)) {
        if (const auto found = byUri.constFind(uri); found != byUri.cend()) {
            songs.append(*found);
        }
    }
    const QList<Song> next = keepingInstances(m_recentlyPlayed->songs(), songs);
    if (!sameSongs(next, m_recentlyPlayed->songs())) {
        m_recentlyPlayed->setSongs(next);
    }
    refreshMostPlayed();
}

void LibraryStore::refreshMostPlayed()
{
    if (!m_localFiles) {
        return;
    }
    const auto library = snapshot();
    const QHash<QString, Song> &byUri = library->byUri;
    // by how often, and among those as often, the one played last
    struct Entry {
        Song song;
        int count;
        qsizetype recency;
    };
    QList<Entry> entries;
    for (auto it = m_playCounts.cbegin(); it != m_playCounts.cend(); ++it) {
        const auto found = byUri.constFind(it.key());
        if (it.value() >= 2 && found != byUri.cend()) {
            const qsizetype recent = m_recentUris.indexOf(it.key());
            entries.append({*found, it.value(), recent < 0 ? m_recentUris.size() : recent});
        }
    }
    std::stable_sort(entries.begin(), entries.end(), [](const Entry &left, const Entry &right) {
        return left.count != right.count ? left.count > right.count : left.recency < right.recency;
    });
    QList<Song> songs;
    for (const Entry &entry : std::as_const(entries)) {
        if (songs.size() >= kMostPlayedLimit) {
            break;
        }
        songs.append(entry.song);
    }
    const QList<Song> next = keepingInstances(m_mostPlayed->songs(), songs);
    if (!sameSongs(next, m_mostPlayed->songs())) {
        m_mostPlayed->setSongs(next);
    }
}

void LibraryStore::refreshRecommendations()
{
    if (!m_localFiles) {
        return;
    }
    const auto library = snapshot();
    const Catalog &catalog = catalogOf(*library);
    QHash<QString, Album> albumsById;
    for (const Album &album : catalog.albums) {
        albumsById.insert(album.id, album);
    }
    QHash<QString, Artist> artistsById;
    for (const Artist &artist : catalog.artists) {
        artistsById.insert(artist.id, artist);
    }

    QList<Artist> artists;
    QList<Album> albums;
    QSet<QString> seenArtists;
    QSet<QString> seenAlbums;
    const auto addArtist = [&](const Artist &artist) {
        if (artist.isValid() && artists.size() < kRecommendationLimit && !seenArtists.contains(artist.id)) {
            seenArtists.insert(artist.id);
            artists.append(artist);
        }
    };
    const auto addAlbum = [&](const Album &album) {
        if (album.isValid() && albums.size() < kRecommendationLimit && !seenAlbums.contains(album.id)) {
            seenAlbums.insert(album.id);
            albums.append(album);
        }
    };
    // what was played lately first, then the rest of the library
    for (const Song &song : m_recentlyPlayed->songs()) {
        addArtist(artistsById.value(artistNamed(primaryArtistName(song)).id));
        addAlbum(albumsById.value(albumOf(song).id));
    }
    for (const Artist &artist : catalog.artists) {
        addArtist(artist);
    }
    for (const Album &album : catalog.albums) {
        addAlbum(album);
    }
    applyArtistPictures(artists, catalog.songsOfArtist);
    m_recommendedArtists->setItems(artists);
    m_recommendedAlbums->setItems(albums);

    // every album, by name
    QList<Album> everyAlbum;
    for (const Album &album : catalog.albums) {
        if (album.isValid()) {
            everyAlbum.append(album);
        }
    }
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::stable_sort(everyAlbum.begin(), everyAlbum.end(), [&](const Album &left, const Album &right) {
        return collator.compare(left.name, right.name) < 0;
    });
    m_allAlbums->setItems(everyAlbum);
    m_everyAlbum = everyAlbum;
    m_everyArtist = catalog.artists;
    m_songsOfArtist = catalog.songsOfArtist;
    refreshDisplayedCollections();
}

void LibraryStore::refreshAlbumPage()
{
    if (m_destination.page != Page::Album || !m_localFiles) {
        return;
    }
    const auto library = snapshot();
    const Catalog &catalog = catalogOf(*library);
    const QList<Song> songs = catalog.songsOfAlbum.value(m_destination.album.id);
    const QList<Song> next = keepingInstances(m_albumSongs->songs(), songs);
    if (!sameSongs(next, m_albumSongs->songs())) {
        m_albumSongs->setSongs(next);
    }
}

void LibraryStore::refreshArtistPage()
{
    if (m_destination.page != Page::Artist || !m_localFiles) {
        return;
    }
    const auto library = snapshot();
    const Catalog &catalog = catalogOf(*library);
    // the artist's own picture, now or when it arrives
    QList<Artist> self {m_destination.artist};
    applyArtistPictures(self, catalog.songsOfArtist);
    if (self.first().artworkUrl != m_destination.artist.artworkUrl) {
        m_destination.artist.artworkUrl = self.first().artworkUrl;
        emit destinationChanged();
    }
    QList<Song> songs = catalog.songsOfArtist.value(m_destination.artist.id);
    if (songs.size() > kArtistSongLimit) {
        songs.resize(kArtistSongLimit);
    }
    const QList<Song> next = keepingInstances(m_artistTopSongs->songs(), songs);
    if (!sameSongs(next, m_artistTopSongs->songs())) {
        m_artistTopSongs->setSongs(next);
    }
    // the albums this artist is on, as the artist of their first song names them
    QList<Album> albums;
    QSet<QString> seen;
    for (const Song &song : catalog.songsOfArtist.value(m_destination.artist.id)) {
        const Album album = albumOf(song);
        if (album.isValid() && !seen.contains(album.id)) {
            seen.insert(album.id);
            for (const Album &known : catalog.albums) {
                if (known.id == album.id) {
                    albums.append(known);
                    break;
                }
            }
        }
    }
    m_artistAlbums->setItems(albums);
}

void LibraryStore::refreshSearch()
{
    const QString query = m_searchQuery.trimmed();
    if (query.isEmpty() || searchIsCatchphrase() || !m_localFiles) {
        m_searchSongs->clear();
        m_searchAlbums->setItems({});
        m_searchArtists->setItems({});
        m_searchPlaylists->setItems({});
        return;
    }
    const auto library = snapshot();
    const QList<Song> &all = library->songs;
    QList<Song> songs;
    for (const Song &song : all) {
        if (songMatches(song, query)) {
            songs.append(song);
            if (songs.size() >= kSearchSongLimit) {
                break;
            }
        }
    }
    const Catalog &catalog = catalogOf(*library);
    QList<Album> albums;
    for (const Album &album : catalog.albums) {
        if (matches(album.name, query) || matches(album.artists, query)) {
            albums.append(album);
            if (albums.size() >= kSearchGroupLimit) {
                break;
            }
        }
    }
    QList<Artist> artists;
    for (const Artist &artist : catalog.artists) {
        if (matches(artist.name, query)) {
            artists.append(artist);
            if (artists.size() >= kSearchGroupLimit) {
                break;
            }
        }
    }
    QList<Playlist> playlists;
    for (const Playlist &playlist : m_playlists->items()) {
        if (matches(playlist.name, query) || matches(playlist.description, query)) {
            playlists.append(playlist);
        }
    }
    applyArtistPictures(artists, catalog.songsOfArtist);
    m_searchSongs->setSongs(keepingInstances(m_searchSongs->songs(), songs));
    m_searchAlbums->setItems(albums);
    m_searchArtists->setItems(artists);
    m_searchPlaylists->setItems(playlists);
}

void LibraryStore::rememberPlayed(const Song &song)
{
    if (!song.isLocalFile()) {
        return;
    }
    m_recentUris.removeAll(song.uri);
    m_recentUris.prepend(song.uri);
    while (m_recentUris.size() > kRecentLimit) {
        m_recentUris.removeLast();
    }
    m_settings->setValue(kRecentKey, m_recentUris);
    refreshRecents();
    refreshRecommendations();
}

// --- state -----------------------------------------------------------------------------

Playlist LibraryStore::selectedPlaylist() const
{
    if (m_destination.page != Page::Playlist) {
        return {};
    }
    const Playlist local = m_localFiles ? m_localFiles->playlist(m_destination.playlistId) : Playlist {};
    return local.isValid() ? local : m_destination.playlist;
}

bool LibraryStore::isSelectedPlaylistEditable() const
{
    return selectedPlaylist().isValid();
}

QVariantList LibraryStore::editablePlaylists() const
{
    QVariantList result;
    for (const Playlist &playlist : m_playlists->items()) {
        result.append(QVariant::fromValue(playlist));
    }
    return result;
}

void LibraryStore::refreshPinnedPlaylists()
{
    QList<Playlist> pinned;
    for (const Playlist &playlist : m_playlists->items()) {
        if (m_pinnedIds.contains(playlist.id)) {
            pinned.append(playlist);
        }
    }
    m_pinnedPlaylists->setItems(pinned);
}

void LibraryStore::persistPins()
{
    QStringList ids(m_pinnedIds.begin(), m_pinnedIds.end());
    ids.sort();
    m_settings->setValue(kPinnedKey, ids);
}

void LibraryStore::setSongs(const QList<Song> &songs)
{
    m_songs = songs;
    m_playlistSongs->setSongs(songs);
    refreshDisplayedSongs();
}

QList<Song> LibraryStore::arranged(const QList<Song> &source) const
{
    const QString query = m_playlistFilterQuery.trimmed();
    QList<Song> shown;
    if (query.isEmpty()) {
        shown = source;
    } else {
        for (const Song &song : source) {
            if (songMatches(song, query)) {
                shown.append(song);
            }
        }
    }

    QCollator collator;
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    collator.setNumericMode(true);
    const auto byText = [&collator](auto field) {
        return [&collator, field](const Song &left, const Song &right) {
            return collator.compare(field(left), field(right)) < 0;
        };
    };
    switch (m_sortOrder) {
    case SortOrder::Playlist:
        break;
    case SortOrder::Title:
        std::stable_sort(shown.begin(), shown.end(), byText([](const Song &song) { return song.name; }));
        break;
    case SortOrder::Artist:
        std::stable_sort(shown.begin(), shown.end(), byText([](const Song &song) { return song.artists; }));
        break;
    case SortOrder::Album:
        std::stable_sort(shown.begin(), shown.end(), byText([](const Song &song) { return song.albumName; }));
        break;
    case SortOrder::Duration:
        std::stable_sort(shown.begin(), shown.end(), [](const Song &left, const Song &right) {
            const int a = left.durationMs > 0 ? left.durationMs : std::numeric_limits<int>::max();
            const int b = right.durationMs > 0 ? right.durationMs : std::numeric_limits<int>::max();
            return a < b;
        });
        break;
    }
    return shown;
}

void LibraryStore::refreshDisplayedSongs()
{
    m_displayedPlaylistSongs->setSongs(arranged(m_songs));
    refreshDisplayedList(m_displayedAllSongs, m_allSongs->songs());
    refreshDisplayedList(m_displayedSavedSongs, m_savedSongs->songs());
    refreshDisplayedCollections();
}

void LibraryStore::refreshDisplayedCollections()
{
    const QString query = m_playlistFilterQuery.trimmed();
    const auto matches = [&query](const QString &text) {
        const QString flat = flattened(query);
        return text.contains(query, Qt::CaseInsensitive) || (!flat.isEmpty() && flattened(text).contains(flat));
    };
    QCollator collator;
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    collator.setNumericMode(true);

    QList<Album> albums;
    for (const Album &album : m_everyAlbum) {
        if (query.isEmpty() || matches(album.name) || matches(album.artists)) {
            albums.append(album);
        }
    }
    // the albums come by name; by artist, each artist's keep that order
    if (m_sortOrder == SortOrder::Artist) {
        std::stable_sort(albums.begin(), albums.end(), [&collator](const Album &left, const Album &right) {
            return collator.compare(left.artists, right.artists) < 0;
        });
    }
    m_displayedAlbums->setItems(albums);

    QList<Artist> artists;
    for (const Artist &artist : m_everyArtist) {
        if (query.isEmpty() || matches(artist.name)) {
            artists.append(artist);
        }
    }
    // pictures are looked for only for the artists that are being looked at
    if (m_destination.page == Page::Artists) {
        applyArtistPictures(artists, m_songsOfArtist);
    }
    m_displayedArtists->setItems(artists);
}

void LibraryStore::setPlaylistFilterQuery(const QString &query)
{
    if (m_playlistFilterQuery == query) {
        return;
    }
    m_playlistFilterQuery = query;
    refreshDisplayedSongs();
    emit playlistViewChanged();
}

void LibraryStore::setPlaylistSortOrder(SortOrder order)
{
    if (m_sortOrder == order) {
        return;
    }
    m_sortOrder = order;
    refreshDisplayedSongs();
    emit playlistViewChanged();
}

bool LibraryStore::canReorderPlaylist() const
{
    // dragging is offered only where it means something: on a playlist,
    // showing every song, in the playlist's own order.
    return isSelectedPlaylistEditable() && m_sortOrder == SortOrder::Playlist && m_playlistFilterQuery.trimmed().isEmpty();
}

// --- navigation ---------------------------------------------------------------------

void LibraryStore::navigate(const Destination &destination)
{
    const Destination previous = m_destination;
    m_destination = destination;

    // the filter field and the sort menu belong to the page they were used on
    if (destination.page != Page::Playlist && (!m_playlistFilterQuery.isEmpty() || m_sortOrder != SortOrder::Playlist)) {
        m_playlistFilterQuery.clear();
        m_sortOrder = SortOrder::Playlist;
        refreshDisplayedSongs();
        emit playlistViewChanged();
    } else if (destination.page == Page::Artists) {
        refreshDisplayedCollections();
    }

    // going back must not record the screen it came from, or back would
    // bounce between the last two destinations forever.
    bool record = true;
    if (m_restoreTarget) {
        const Destination target = *m_restoreTarget;
        m_restoreTarget.reset();
        if (target == destination) {
            record = false;
        }
    }
    if (record && !(previous == destination) && (m_history.isEmpty() || !(m_history.last() == previous))) {
        m_history.append(previous);
    }
    emit destinationChanged();
    emit selectedPlaylistChanged();
    emit playlistViewChanged();
}

void LibraryStore::goBack()
{
    if (m_history.isEmpty()) {
        return;
    }
    const Destination destination = m_history.takeLast();
    m_restoreTarget = destination;
    switch (destination.page) {
    case Page::Home: showHome(); break;
    case Page::Search: showSearch(); break;
    case Page::Songs: showSongs(); break;
    case Page::Favourites: showSavedSongs(); break;
    case Page::Playlists: showPlaylists(); break;
    case Page::PinnedPlaylists: showPinnedPlaylists(); break;
    case Page::Albums: showAlbums(); break;
    case Page::Artists: showArtists(); break;
    case Page::Playlist: {
        const Playlist local = m_localFiles ? m_localFiles->playlist(destination.playlistId) : Playlist {};
        if (local.isValid()) {
            selectPlaylist(local);
        } else {
            showPlaylists();
        }
        break;
    }
    case Page::Album: selectAlbum(destination.album); break;
    case Page::Artist: selectArtist(destination.artist); break;
    }
}

void LibraryStore::showHome()
{
    navigate({Page::Home});
}

void LibraryStore::showSearch()
{
    navigate({Page::Search});
}

void LibraryStore::showSongs()
{
    navigate({Page::Songs});
}

void LibraryStore::showSavedSongs()
{
    navigate({Page::Favourites});
}

void LibraryStore::showPlaylists()
{
    navigate({Page::Playlists});
}

void LibraryStore::showPinnedPlaylists()
{
    navigate({Page::PinnedPlaylists});
}

void LibraryStore::showAlbums()
{
    navigate({Page::Albums});
}

void LibraryStore::showArtists()
{
    navigate({Page::Artists});
}

void LibraryStore::selectPlaylistById(const QString &id)
{
    const Playlist local = m_localFiles ? m_localFiles->playlist(id) : Playlist {};
    if (local.isValid()) {
        selectPlaylist(local);
    }
}

void LibraryStore::selectPlaylist(const Playlist &playlist)
{
    m_playlistFilterQuery.clear();
    m_sortOrder = SortOrder::Playlist;
    m_errors->clear();
    navigate({Page::Playlist, playlist.id, {}, {}, playlist});
    // nothing to load: the files are already known
    setSongs(m_localFiles ? m_localFiles->songsOf(playlist.id) : QList<Song> {});
}

void LibraryStore::selectAlbum(const Album &album)
{
    m_albumSongs->clear();
    navigate({Page::Album, {}, album});
    refreshAlbumPage();
}

void LibraryStore::selectArtist(const Artist &artist)
{
    m_artistTopSongs->clear();
    m_artistAlbums->setItems({});
    navigate({Page::Artist, {}, {}, artist});
    refreshArtistPage();
}

void LibraryStore::goToAlbum(const Song &song)
{
    if (const Album album = albumOf(song); album.isValid()) {
        selectAlbum(album);
    }
}

void LibraryStore::goToArtist(const Song &song)
{
    const QString name = primaryArtistName(song);
    if (!name.isEmpty()) {
        selectArtist(artistNamed(name, song.artworkUrl));
    }
}

// --- search ---------------------------------------------------------------------------

bool LibraryStore::searchIsCatchphrase() const
{
    return m_searchQuery.trimmed().toLower() == kCatchphrase;
}

bool LibraryStore::hasSearchResults() const
{
    return m_searchSongs->count() > 0 || m_searchAlbums->count() > 0 || m_searchArtists->count() > 0
        || m_searchPlaylists->count() > 0;
}

void LibraryStore::setSearchQuery(const QString &query)
{
    if (m_searchQuery == query) {
        return;
    }
    m_searchQuery = query;
    refreshSearch();
    emit searchChanged();
}

void LibraryStore::recordSearchPlayback(const Song &song)
{
    QList<Song> history = m_searchHistory->songs();
    history.removeIf([&](const Song &item) { return item.uri == song.uri; });
    history.prepend(song);
    if (history.size() > kSearchHistoryLimit) {
        history.resize(kSearchHistoryLimit);
    }
    setSearchHistory(history);
    persistSearchHistory();
}

void LibraryStore::clearSearchHistory()
{
    setSearchHistory({});
    persistSearchHistory();
}

void LibraryStore::setSearchHistory(const QList<Song> &songs)
{
    m_searchHistory->setSongs(songs);
    m_searchHistoryPreview->setSongs(songs.mid(0, 8));
}

void LibraryStore::persistSearchHistory()
{
    QJsonArray array;
    for (const Song &song : m_searchHistory->songs()) {
        array.append(song.toJson());
    }
    m_settings->setValue(kSearchHistoryKey, QJsonDocument(array).toJson(QJsonDocument::Compact));
}

// --- favorites and pins ----------------------------------------------------------------

void LibraryStore::togglePinned(const Playlist &playlist)
{
    if (m_pinnedIds.contains(playlist.id)) {
        m_pinnedIds.remove(playlist.id);
    } else {
        m_pinnedIds.insert(playlist.id);
    }
    persistPins();
    refreshPinnedPlaylists();
    ++m_pinsRevision;
    emit pinsChanged();
}

void LibraryStore::persistSaved()
{
    m_settings->setValue(kSavedKey, m_savedOrder);
}

void LibraryStore::toggleSaved(const Song &song)
{
    if (!song.isLocalFile()) {
        return;
    }
    if (m_savedUris.contains(song.uri)) {
        m_savedUris.remove(song.uri);
        m_savedOrder.removeAll(song.uri);
    } else {
        m_savedUris.insert(song.uri);
        m_savedOrder.prepend(song.uri);
    }
    persistSaved();
    refreshSaved();
    ++m_savedRevision;
    emit savedChanged();
    emit savedSongTotalChanged();
}

// --- playlists ------------------------------------------------------------------------

void LibraryStore::addToPlaylist(const Song &song, const Playlist &playlist)
{
    // what changes shows once the library says so (refreshLocalPlaylist)
    if (!song.isLocalFile()) {
        m_errors->show(QStringLiteral("Only music files on this computer can go in “%1”.").arg(playlist.name));
    } else if (m_localFiles) {
        m_localFiles->addSongs(playlist.id, {song});
    }
}

void LibraryStore::removeFromSelectedPlaylist(const Song &song)
{
    removeFromPlaylist(song, selectedPlaylist());
}

void LibraryStore::removeFromPlaylist(const Song &song, const Playlist &playlist)
{
    if (m_localFiles && playlist.isValid()) {
        m_localFiles->removeSong(playlist.id, song.uri);
    }
}

void LibraryStore::moveSongs(int from, int count, int to)
{
    const Playlist playlist = selectedPlaylist();
    // every refusal below used to be silent, which looked exactly like a
    // broken drag: the row sprang back and nothing else happened.
    if (!playlist.isValid() || !m_localFiles) {
        m_errors->show(QStringLiteral("Open the playlist once before reordering it."));
        return;
    }
    if (count <= 0 || from < 0 || from + count > m_songs.size() || to < 0 || to > m_songs.size()) {
        return;
    }

    QList<Song> reordered = m_songs;
    const QList<Song> moving = reordered.mid(from, count);
    reordered.remove(from, count);
    const int insertAt = to > from ? to - count : to;
    for (int offset = 0; offset < count; ++offset) {
        reordered.insert(insertAt + offset, moving.at(offset));
    }
    QStringList uris;
    for (const Song &song : reordered) {
        uris.append(song.uri);
    }
    QStringList before;
    for (const Song &song : m_songs) {
        before.append(song.uri);
    }
    if (uris == before) {
        return;
    }

    // the rows move in place, so the drag lands without the list resetting.
    m_songs = reordered;
    m_playlistSongs->moveRows(from, count, to);
    m_displayedPlaylistSongs->moveRows(from, count, to);
    m_localFiles->reorder(playlist.id, uris);
}

void LibraryStore::deletePlaylist(const Playlist &playlist)
{
    if (!m_localFiles || !m_localFiles->deletePlaylist(playlist.id)) {
        return;
    }
    if (m_pinnedIds.remove(playlist.id)) {
        persistPins();
        refreshPinnedPlaylists();
        ++m_pinsRevision;
        emit pinsChanged();
    }
    if (selectedPlaylistId() == playlist.id) {
        setSongs({});
        m_playlistFilterQuery.clear();
        navigate({Page::Playlists});
    }
    emit playlistDeleted(playlist.id);
}

void LibraryStore::saveLocalPlaylist(bool isNew, const QString &name, const QString &description,
                                     const QByteArray &coverJpeg, const QVariant &firstSong)
{
    const Song song = firstSong.value<Song>();
    bool ok = false;
    if (name.trimmed().isEmpty()) {
        m_errors->show(QStringLiteral("Enter a playlist name."));
    } else if (m_localFiles && isNew) {
        const Playlist created = m_localFiles->createPlaylist(
            name, description, coverJpeg, song.isLocalFile() ? QList<Song> {song} : QList<Song> {});
        ok = created.isValid();
        // opened once made
        if (ok) {
            selectPlaylist(created);
        }
    } else if (m_localFiles) {
        const Playlist target = selectedPlaylist();
        ok = target.isValid() && m_localFiles->updatePlaylist(target.id, name, description, coverJpeg);
    }
    emit playlistSaveFinished(ok);
}

} // namespace swifty
