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

pragma ComponentBehavior: Bound
import QtQuick
import Swifty

// Songs: every song there is, once: the music in the folders and files added,
// and what the playlists hold (Library.allSongs)
SongListPage {
    id: root

    songs: Library.displayedAllSongs
    loading: LocalFiles.scanning && Library.allSongs.count === 0
    loadingText: "Looking through your music…"
    showsEmpty: Library.allSongs.count === 0 || Library.displayedAllSongs.count === 0
    emptySymbol: Library.allSongs.count === 0 ? "music_note" : "search_off"
    emptyTitle: Library.allSongs.count > 0 ? "No Results for “" + Library.playlistFilterQuery.trim() + "”"
                : LocalFiles.sources.length === 0 ? "No Music Yet" : "No Music Found"
    emptyDescription: Library.allSongs.count > 0 ? "Check the spelling or try a new search."
                      : LocalFiles.sources.length === 0 ? "Add a folder of music, or drop files onto the window." : "The folders you added hold no music Swifty can play."

    MusicPicker {
        id: picker
    }

    hero: CollectionHero {
        kind: "LIBRARY"
        title: "Songs"
        description: LocalFiles.sources.length === 1 ? LocalFiles.sources[0] : LocalFiles.sources.length > 1 ? LocalFiles.sources.length + " folders and files" : ""
        facts: Format.joined([Format.songCount(Library.allSongs.count), Format.totalDuration(Library.allSongs.totalDurationMs)], "  •  ")

        artwork: Rectangle {
            anchors.fill: parent
            radius: Appearance.sizes.artworkRadius
            gradient: Gradient {
                GradientStop {
                    position: 0
                    color: Appearance.colors.tertiaryContainer
                }
                GradientStop {
                    position: 1
                    color: Appearance.colors.primaryContainer
                }
            }

            MaterialSymbol {
                anchors.centerIn: parent
                text: "music_note"
                iconSize: 96
                fill: 1
                color: Appearance.colors.onPrimaryContainer
            }
        }

        actions: [
            TextButton {
                id: playButton
                // a little wider than its words, and as wide as shuffle
                // beside it, whose words are longer
                width: iconOnly ? compactSize : Math.max(naturalWidth + 28, shuffleButton.naturalWidth)
                iconOnly: parent ? parent.compactActions : false
                compactSize: parent ? parent.actionSize : 48
                implicitHeight: iconOnly ? compactSize : 48
                wavy: true
                symbol: "play_arrow"
                burst: "play"
                text: "Play"
                enabled: Library.allSongs.count > 0
                onClicked: Player.playFromModel(Library.allSongs, 0)
            },
            // play's twin in the primary's softer tone
            TextButton {
                id: shuffleButton
                width: playButton.width
                iconOnly: parent ? parent.compactActions : false
                compactSize: parent ? parent.actionSize : 48
                implicitHeight: iconOnly ? compactSize : 48
                wavy: true
                symbol: "shuffle"
                burst: "play"
                text: "Shuffle"
                colBackground: shuffleButton.scheme.primaryContainer
                colBackgroundHover: shuffleButton.scheme.primaryContainerHover
                colRipple: shuffleButton.scheme.primaryContainerActive
                labelColor: shuffleButton.scheme.onPrimaryContainer
                enabled: Library.allSongs.count > 0
                onClicked: Player.shufflePlay(Library.allSongs)
            },
            TextButton {
                iconOnly: parent ? parent.compactActions : false
                compactSize: parent ? parent.actionSize : 48
                implicitHeight: iconOnly ? compactSize : 48
                variant: TextButton.Outlined
                symbol: "create_new_folder"
                text: "Add Folder"
                onClicked: picker.pickFolder()
            },
            IconButton {
                size: parent ? parent.actionSize : 36
                symbol: "refresh"
                enabled: LocalFiles.sources.length > 0 && !LocalFiles.scanning
                tooltip: "Look Again"
                onClicked: LocalFiles.rescan()
            }
        ]
    }
}
