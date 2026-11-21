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
import QtQuick.Controls.Basic
import Swifty

// Albums: every album of the library, by name, as the playlists are laid out:
// an adaptive grid of 180-240 px tiles
GridView {
    WheelScroll {}
    id: root

    readonly property real margin: Appearance.sizes.pageMargin
    // whole columns at least 200 px wide that fill the width exactly: the
    // grid lays columns out by cellWidth, so tile plus gap has to divide it.
    // each cell's 20 px gap is split either side of its tile, and the grid
    // reaches 10 px past the margin so the tiles themselves line up with it
    readonly property real available: width - 2 * margin + 20
    readonly property int columns: Math.max(1, Math.floor(available / 200))
    readonly property real tile: available / columns - 20

    leftMargin: margin - 10
    rightMargin: margin - 10
    // under the top bar, which floats over the page, and a little more
    topMargin: Appearance.sizes.topBarHeight + 24
    bottomMargin: Appearance.sizes.playerBarClearance
    cellWidth: available / columns
    cellHeight: tile + 86
    boundsBehavior: Flickable.StopAtBounds
    reuseItems: true
    ScrollBar.vertical: StyledScrollBar { topPadding: 3 + Appearance.sizes.topBarHeight }

    model: Library.displayedAlbums.count

    header: Item {
        width: root.available
        height: 64
        // under the tiles, which the grid puts at 1
        z: 0.5

        Stickers {
            anchors.fill: parent
            anchors.topMargin: -24
            anchors.bottomMargin: -24
            key: "albums"
            count: 4
            maximumSize: 150
        }

        TitleText {
            x: 10
            anchors.verticalCenter: parent.verticalCenter
            text: "Albums"
            font.pixelSize: Appearance.fontSize.display
            font.weight: Font.Bold
        }
    }

    delegate: Item {
        id: cell

        required property int index
        readonly property album album: Library.displayedAlbums.at(index)

        width: root.cellWidth
        height: root.cellHeight

        MediaCard {
            x: 10
            artworkSize: root.tile
            artworkUrl: cell.album.artworkUrl
            placeholderSymbol: "album"
            title: cell.album.name
            subtitle: cell.album.artists
            onClicked: Library.selectAlbum(cell.album)
        }
    }

    EmptyState {
        anchors.centerIn: parent
        Reveal.shown: Library.displayedAlbums.count === 0
        symbol: Library.allAlbums.count === 0 ? "album" : "search_off"
        title: Library.allAlbums.count === 0 ? "No Albums Yet" : "No Results for “" + Library.playlistFilterQuery.trim() + "”"
        description: Library.allAlbums.count === 0 ? "Add music from the Songs page or the settings." : "Check the spelling or try a new search."
    }
}
