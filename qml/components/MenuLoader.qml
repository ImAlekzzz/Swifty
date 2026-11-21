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
import QtQuick.Templates as T
import Swifty

// a menu built the first time it is opened, rather than with the view it
// belongs to: most menus are never opened, and each one built is a few
// hundred objects kept for as long as the view is
Loader {
    id: root

    active: false

    /// opens it under or beside `anchor`, at `x`, `y` in its coordinates
    function popupAt(anchor: Item, x: real, y: real) {
        active = true
        // through a name: a line opening with a bracket runs on from the
        // one before, and `true(item)` is no call
        const menu = item as T.Menu
        menu.popup(anchor, x, y)
    }

    /// opens it at the pointer
    function popupHere() {
        active = true
        const menu = item as T.Menu
        menu.popup()
    }
}
