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

#include "core/MusicTypes.h"
#include "library/CollectionModels.h"
#include "library/SongListModel.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

#include <optional>

class QQmlEngine;
class QJSEngine;

#include <memory>
#include <optional>

namespace swifty {

class ArtistImages;
class ErrorCenter;
class LocalLibrary;
class PlaybackController;
class Settings;

/// what the person has on this computer and where they are in it: playlists,
/// favorites, recent plays, search, albums, artists, and the page on screen.
/// everything is built from the music files LocalLibrary knows.
/// the albums and artists the music files make up
struct LibraryCatalog {
    QList<Album> albums;
    QList<Artist> artists;
    QHash<QString, QList<Song>> songsOfAlbum;
    QHash<QString, QList<Song>> songsOfArtist;
};

/// the library as it is now, each track once, worked out once for everything
/// that asks, and thrown away when the library changes
struct LibrarySnapshot {
    QList<Song> songs;
    QHash<QString, Song> byUri;
    /// built when first asked for
    std::optional<LibraryCatalog> catalog;
};

class LibraryStore : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(Library)
    Q_CLASSINFO("RegisterEnumClassesUnscoped", "false")
    QML_SINGLETON

    Q_PROPERTY(Page page READ page NOTIFY destinationChanged FINAL)
    Q_PROPERTY(QString selectedPlaylistId READ selectedPlaylistId NOTIFY destinationChanged FINAL)
    Q_PROPERTY(swifty::Playlist selectedPlaylist READ selectedPlaylist NOTIFY selectedPlaylistChanged FINAL)
    Q_PROPERTY(bool selectedPlaylistEditable READ isSelectedPlaylistEditable NOTIFY selectedPlaylistChanged FINAL)
    Q_PROPERTY(swifty::Album currentAlbum READ currentAlbum NOTIFY destinationChanged FINAL)
    Q_PROPERTY(swifty::Artist currentArtist READ currentArtist NOTIFY destinationChanged FINAL)
    Q_PROPERTY(bool canGoBack READ canGoBack NOTIFY destinationChanged FINAL)
    /// how many screens back goes through, for a transition that follows
    /// where the person is going
    Q_PROPERTY(int historyDepth READ historyDepth NOTIFY destinationChanged FINAL)

    Q_PROPERTY(swifty::PlaylistListModel *playlists READ playlists CONSTANT FINAL)
    Q_PROPERTY(swifty::PlaylistListModel *pinnedPlaylists READ pinnedPlaylists CONSTANT FINAL)
    /// bumped when a playlist is pinned or unpinned, so a binding on isPinned() re-evaluates
    Q_PROPERTY(int pinsRevision READ pinsRevision NOTIFY pinsChanged FINAL)

    Q_PROPERTY(swifty::SongListModel *playlistSongs READ playlistSongs CONSTANT FINAL)
    Q_PROPERTY(swifty::SongListModel *displayedPlaylistSongs READ displayedPlaylistSongs CONSTANT FINAL)
    /// what the pages show of their lists once the filter field and the sort
    /// menu of the top bar have had their say. both are forgotten on leaving a
    /// page (playlistFilterQuery and playlistSortOrder are the field's and
    /// the menu's, whichever page is open)
    Q_PROPERTY(swifty::SongListModel *displayedAllSongs READ displayedAllSongs CONSTANT FINAL)
    Q_PROPERTY(swifty::SongListModel *displayedSavedSongs READ displayedSavedSongs CONSTANT FINAL)
    Q_PROPERTY(swifty::AlbumListModel *displayedAlbums READ displayedAlbums CONSTANT FINAL)
    Q_PROPERTY(swifty::ArtistListModel *displayedArtists READ displayedArtists CONSTANT FINAL)
    Q_PROPERTY(QString playlistFilterQuery READ playlistFilterQuery WRITE setPlaylistFilterQuery NOTIFY playlistViewChanged FINAL)
    Q_PROPERTY(SortOrder playlistSortOrder READ playlistSortOrder WRITE setPlaylistSortOrder NOTIFY playlistViewChanged FINAL)
    Q_PROPERTY(bool canReorderPlaylist READ canReorderPlaylist NOTIFY playlistViewChanged FINAL)

    Q_PROPERTY(swifty::SongListModel *savedSongs READ savedSongs CONSTANT FINAL)
    /// every song there is: the music in the folders and files added, and what
    /// the playlists hold, each once
    Q_PROPERTY(swifty::SongListModel *allSongs READ allSongs CONSTANT FINAL)
    Q_PROPERTY(swifty::SongListModel *recentlyPlayed READ recentlyPlayed CONSTANT FINAL)
    /// the songs played most, at least twice each, most first
    Q_PROPERTY(swifty::SongListModel *mostPlayed READ mostPlayed CONSTANT FINAL)
    Q_PROPERTY(swifty::ArtistListModel *recommendedArtists READ recommendedArtists CONSTANT FINAL)
    Q_PROPERTY(swifty::AlbumListModel *recommendedAlbums READ recommendedAlbums CONSTANT FINAL)
    /// every album of the library, by name
    Q_PROPERTY(swifty::AlbumListModel *allAlbums READ allAlbums CONSTANT FINAL)
    Q_PROPERTY(swifty::SongListModel *albumSongs READ albumSongs CONSTANT FINAL)
    Q_PROPERTY(swifty::SongListModel *artistTopSongs READ artistTopSongs CONSTANT FINAL)
    Q_PROPERTY(swifty::AlbumListModel *artistAlbums READ artistAlbums CONSTANT FINAL)

    Q_PROPERTY(QString searchQuery READ searchQuery WRITE setSearchQuery NOTIFY searchChanged FINAL)
    Q_PROPERTY(bool searchIsCatchphrase READ searchIsCatchphrase NOTIFY searchChanged FINAL)
    Q_PROPERTY(swifty::SongListModel *searchSongs READ searchSongs CONSTANT FINAL)
    Q_PROPERTY(swifty::AlbumListModel *searchAlbums READ searchAlbums CONSTANT FINAL)
    Q_PROPERTY(swifty::ArtistListModel *searchArtists READ searchArtists CONSTANT FINAL)
    Q_PROPERTY(swifty::PlaylistListModel *searchPlaylists READ searchPlaylists CONSTANT FINAL)
    Q_PROPERTY(bool hasSearchResults READ hasSearchResults NOTIFY searchChanged FINAL)
    /// the songs played from a search, newest first
    Q_PROPERTY(swifty::SongListModel *searchHistory READ searchHistory CONSTANT FINAL)
    /// the first few of searchHistory
    Q_PROPERTY(swifty::SongListModel *searchHistoryPreview READ searchHistoryPreview CONSTANT FINAL)

    /// bumped when favorites change, so a binding on isSaved() re-evaluates
    Q_PROPERTY(int savedRevision READ savedRevision NOTIFY savedChanged FINAL)
    Q_PROPERTY(int savedSongTotal READ savedSongTotal NOTIFY savedSongTotalChanged FINAL)

public:
    enum class Page { Home, Search, Songs, Playlists, PinnedPlaylists, Playlist, Album, Artist, Favourites, Albums, Artists };
    Q_ENUM(Page)
    enum class SortOrder { Playlist, Title, Artist, Album, Duration };
    Q_ENUM(SortOrder)

    LibraryStore(Settings *settings, ErrorCenter *errors, QObject *parent = nullptr);
    ~LibraryStore() override;

    static LibraryStore *instance();
    static LibraryStore *create(QQmlEngine *, QJSEngine *);

    /// what plays is remembered as recently played, and a song played from
    /// search is added to the search history
    void attachPlayer(PlaybackController *player);
    /// the music files everything here is built from
    void attachLocalFiles(LocalLibrary *localFiles);
    /// where artists' pictures come from. without it an artist shows a cover.
    void setArtistImages(ArtistImages *images);

    Page page() const { return m_destination.page; }
    QString selectedPlaylistId() const { return m_destination.page == Page::Playlist ? m_destination.playlistId : QString(); }
    Playlist selectedPlaylist() const;
    bool isSelectedPlaylistEditable() const;
    Album currentAlbum() const { return m_destination.album; }
    Artist currentArtist() const { return m_destination.artist; }
    bool canGoBack() const { return !m_history.isEmpty(); }
    int historyDepth() const { return int(m_history.size()); }

    PlaylistListModel *playlists() const { return m_playlists; }
    PlaylistListModel *pinnedPlaylists() const { return m_pinnedPlaylists; }
    int pinsRevision() const { return m_pinsRevision; }
    SongListModel *playlistSongs() const { return m_playlistSongs; }
    SongListModel *displayedPlaylistSongs() const { return m_displayedPlaylistSongs; }
    SongListModel *displayedAllSongs() const { return m_displayedAllSongs; }
    SongListModel *displayedSavedSongs() const { return m_displayedSavedSongs; }
    AlbumListModel *displayedAlbums() const { return m_displayedAlbums; }
    ArtistListModel *displayedArtists() const { return m_displayedArtists; }
    QString playlistFilterQuery() const { return m_playlistFilterQuery; }
    void setPlaylistFilterQuery(const QString &query);
    SortOrder playlistSortOrder() const { return m_sortOrder; }
    void setPlaylistSortOrder(SortOrder order);
    bool canReorderPlaylist() const;

    SongListModel *savedSongs() const { return m_savedSongs; }
    SongListModel *allSongs() const { return m_allSongs; }
    SongListModel *recentlyPlayed() const { return m_recentlyPlayed; }
    SongListModel *mostPlayed() const { return m_mostPlayed; }
    /// how many times a song has been played: listened to for half a minute, or
    /// half of a short song. skipping through it does not count
    Q_INVOKABLE int playCountOf(const QString &uri) const { return m_playCounts.value(uri, 0); }
    /// what the player reports as it plays; counts a play once enough was heard.
    /// public for the player's signals, and for tests
    void notePlayProgress(const Song &song, quint32 positionMs, quint32 durationMs, bool playing);
    ArtistListModel *recommendedArtists() const { return m_recommendedArtists; }
    AlbumListModel *recommendedAlbums() const { return m_recommendedAlbums; }
    AlbumListModel *allAlbums() const { return m_allAlbums; }
    SongListModel *albumSongs() const { return m_albumSongs; }
    SongListModel *artistTopSongs() const { return m_artistTopSongs; }
    AlbumListModel *artistAlbums() const { return m_artistAlbums; }

    QString searchQuery() const { return m_searchQuery; }
    void setSearchQuery(const QString &query);
    bool searchIsCatchphrase() const;
    SongListModel *searchSongs() const { return m_searchSongs; }
    AlbumListModel *searchAlbums() const { return m_searchAlbums; }
    ArtistListModel *searchArtists() const { return m_searchArtists; }
    PlaylistListModel *searchPlaylists() const { return m_searchPlaylists; }
    bool hasSearchResults() const;
    SongListModel *searchHistory() const { return m_searchHistory; }
    SongListModel *searchHistoryPreview() const { return m_searchHistoryPreview; }

    int savedRevision() const { return m_savedRevision; }
    int savedSongTotal() const { return m_savedSongs->count(); }

    // --- navigation ---------------------------------------------------------------
    Q_INVOKABLE void showHome();
    Q_INVOKABLE void showSearch();
    /// every song
    Q_INVOKABLE void showSongs();
    /// the favourites, a collection of the playlists
    Q_INVOKABLE void showSavedSongs();
    Q_INVOKABLE void showPlaylists();
    Q_INVOKABLE void showPinnedPlaylists();
    /// the person's playlists of music files
    /// every album, as a grid of covers
    Q_INVOKABLE void showAlbums();
    /// every artist, as a grid
    Q_INVOKABLE void showArtists();
    /// every music file as a list
    Q_INVOKABLE void selectPlaylist(const swifty::Playlist &playlist);
    Q_INVOKABLE void selectPlaylistById(const QString &id);
    Q_INVOKABLE void selectAlbum(const swifty::Album &album);
    Q_INVOKABLE void selectArtist(const swifty::Artist &artist);
    Q_INVOKABLE void goToAlbum(const swifty::Song &song);
    Q_INVOKABLE void goToArtist(const swifty::Song &song);
    Q_INVOKABLE void goBack();

    // --- actions --------------------------------------------------------------------
    Q_INVOKABLE bool isPinned(const QString &playlistId) const { return m_pinnedIds.contains(playlistId); }
    Q_INVOKABLE void togglePinned(const swifty::Playlist &playlist);
    Q_INVOKABLE bool isSaved(const QString &uri) const { return m_savedUris.contains(uri); }
    Q_INVOKABLE void toggleSaved(const swifty::Song &song);
    Q_INVOKABLE QVariantList editablePlaylists() const;
    Q_INVOKABLE void clearSearchHistory();
    Q_INVOKABLE void addToPlaylist(const swifty::Song &song, const swifty::Playlist &playlist);
    Q_INVOKABLE void removeFromPlaylist(const swifty::Song &song, const swifty::Playlist &playlist);
    Q_INVOKABLE void removeFromSelectedPlaylist(const swifty::Song &song);
    /// moves `count` rows starting at `from` to before row `to`, in the open
    /// playlist's own order. contiguous ranges only.
    Q_INVOKABLE void moveSongs(int from, int count, int to);
    Q_INVOKABLE void deletePlaylist(const swifty::Playlist &playlist);
    /// creates a playlist holding `firstSong` when that is one (a song, or
    /// null), or updates the open one. reports through playlistSaveFinished.
    Q_INVOKABLE void saveLocalPlaylist(bool isNew, const QString &name, const QString &description,
                                       const QByteArray &coverJpeg, const QVariant &firstSong);

    void recordSearchPlayback(const Song &song);

    /// whether a song answers a search: its title, artists, album or genre
    /// holds the text, in any capitalisation. a genre also matches when only
    /// the punctuation differs ("hip hop" finds "Hip-Hop").
    static bool songMatches(const Song &song, const QString &query);

signals:
    void destinationChanged();
    void selectedPlaylistChanged();
    void pinsChanged();
    void playlistViewChanged();
    void searchChanged();
    void savedChanged();
    void savedSongTotalChanged();
    void playlistSaveFinished(bool ok);
    void playlistDeleted(const QString &id);

private:
    struct Destination {
        Page page = Page::Home;
        QString playlistId;
        Album album;
        Artist artist;
        /// as opened
        Playlist playlist;

        bool operator==(const Destination &other) const;
    };

    void navigate(const Destination &destination);
    void refreshPinnedPlaylists();
    void persistPins();
    void setSongs(const QList<Song> &songs);
    void refreshDisplayedSongs();
    void refreshDisplayedCollections();
    QList<Song> arranged(const QList<Song> &source) const;
    void refreshLocalPlaylist();
    /// everything derived from the music files, after they or their tags changed
    void libraryDidChange();
    void refreshPlaylists();
    void refreshSaved();
    /// the whole list of songs, which only a change of the library changes
    void refreshAllSongs();
    /// a page's list of songs once the filter and the sort have had their say
    void refreshDisplayedList(SongListModel *model, const QList<Song> &source);
    /// the library now. it stays valid for whoever holds it, whatever happens to the cache
    std::shared_ptr<LibrarySnapshot> snapshot();
    static const LibraryCatalog &catalogOf(LibrarySnapshot &library);
    void refreshRecents();
    void refreshMostPlayed();
    void refreshRecommendations();
    void refreshAlbumPage();
    void refreshArtistPage();
    void refreshSearch();
    /// the picture of each artist: one found online or kept in their folder,
    /// and the cover they were given otherwise
    void applyArtistPictures(QList<Artist> &artists, const QHash<QString, QList<Song>> &songsOfArtist) const;
    void rememberPlayed(const Song &song);
    void setSearchHistory(const QList<Song> &songs);
    void persistSearchHistory();
    void persistSaved();
    void reportError(const QString &context, const QString &message);

    Settings *m_settings;
    ErrorCenter *m_errors;
    LocalLibrary *m_localFiles = nullptr;
    ArtistImages *m_artistImages = nullptr;

    Destination m_destination;
    QList<Destination> m_history;
    std::optional<Destination> m_restoreTarget;

    PlaylistListModel *m_playlists;
    PlaylistListModel *m_pinnedPlaylists;
    QSet<QString> m_pinnedIds;
    int m_pinsRevision = 0;

    QList<Song> m_songs;
    SongListModel *m_playlistSongs;
    SongListModel *m_displayedPlaylistSongs;
    SongListModel *m_displayedAllSongs;
    SongListModel *m_displayedSavedSongs;
    AlbumListModel *m_displayedAlbums;
    ArtistListModel *m_displayedArtists;
    QList<Album> m_everyAlbum;
    QList<Artist> m_everyArtist;
    QHash<QString, QList<Song>> m_songsOfArtist;
    QString m_playlistFilterQuery;
    SortOrder m_sortOrder = SortOrder::Playlist;

    SongListModel *m_savedSongs;
    std::shared_ptr<LibrarySnapshot> m_snapshot;
    SongListModel *m_allSongs;
    QSet<QString> m_savedUris;
    QStringList m_savedOrder;
    int m_savedRevision = 0;
    SongListModel *m_recentlyPlayed;
    SongListModel *m_mostPlayed;
    QHash<QString, int> m_playCounts;
    quint64 m_countingInstance = 0;
    quint32 m_lastProgressMs = 0;
    quint32 m_listenedMs = 0;
    bool m_countedThisPlay = false;
    QStringList m_recentUris;
    ArtistListModel *m_recommendedArtists;
    AlbumListModel *m_recommendedAlbums;
    AlbumListModel *m_allAlbums;
    SongListModel *m_albumSongs;
    SongListModel *m_artistTopSongs;
    AlbumListModel *m_artistAlbums;

    QString m_searchQuery;
    SongListModel *m_searchSongs;
    AlbumListModel *m_searchAlbums;
    ArtistListModel *m_searchArtists;
    PlaylistListModel *m_searchPlaylists;
    SongListModel *m_searchHistory;
    SongListModel *m_searchHistoryPreview;

    /// coalesces the several signals one scan or tag edit sends
    QTimer m_refreshTimer;
    QTimer m_imageRefreshTimer;
};

} // namespace swifty
