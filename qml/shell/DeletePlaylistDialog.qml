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

StyledDialog {
    id: root

    property playlist playlist

    function ask(target: playlist) {
        playlist = target
        open()
    }

    headline: "Delete “" + playlist.name + "”?"
symbol: "delete"
iconShape: "ghostish"
    body: "This deletes the playlist from this computer. Its music files stay where they are."

    buttons: [
        TextButton {
            text: "Delete Playlist"
            colBackground: Appearance.colors.error
            colBackgroundHover: Appearance.colors.errorHover
            labelColor: Appearance.colors.onError
            onClicked: {
                Library.deletePlaylist(root.playlist)
                root.accept()
            }
        },
        TextButton {
            variant: TextButton.Text
            text: "Cancel"
            onClicked: root.close()
        }
    ]
}
