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

pragma ComponentBehavior: Bound
import QtQuick
import Swifty

// Favourite Songs (SavedSongsView)
SongListPage {
    id: root

    songs: Library.displayedSavedSongs
    showsEmpty: Library.savedSongs.count === 0 || Library.displayedSavedSongs.count === 0
    emptySymbol: Library.savedSongs.count === 0 ? "star" : "search_off"
    emptyTitle: Library.savedSongs.count > 0 ? "No Results for “" + Library.playlistFilterQuery.trim() + "”" : "No Favourites Yet"
    emptyDescription: Library.savedSongs.count > 0 ? "Check the spelling or try a new search." : "Tap the star on a song to add it here."

    hero: CollectionHero {
        kind: "PLAYLIST"
        title: "Favourite Songs"
        facts: Format.joined([Format.songCount(Library.savedSongs.count), Format.totalDuration(Library.savedSongs.totalDurationMs)], "  •  ")

        artwork: Rectangle {
            anchors.fill: parent
            radius: Appearance.sizes.artworkRadius
            // in the selected theme's colours, as everything without a cover
            gradient: Gradient {
                GradientStop {
                    position: 0
                    color: Appearance.colors.primary
                }
                GradientStop {
                    position: 1
                    color: Appearance.colors.tertiary
                }
            }

            MaterialSymbol {
                anchors.centerIn: parent
                text: "favorite"
                iconSize: 96
                fill: 1
                color: Appearance.colors.onPrimary
            }
        }

        actions: [
            TextButton {
                iconOnly: parent ? parent.compactActions : false
                compactSize: parent ? parent.actionSize : 48
                implicitHeight: iconOnly ? compactSize : 48
                wavy: true
                symbol: "play_arrow"
                burst: "play"
                text: "Play"
                enabled: Library.savedSongs.count > 0
                onClicked: Player.playFromModel(Library.savedSongs, 0)
            }
        ]
    }
}
