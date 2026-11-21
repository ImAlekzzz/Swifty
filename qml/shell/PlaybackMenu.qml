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

// the macos playback menu, which on linux lives behind the top bar's overflow
// button. labels follow the player's state, as the menu bar's did.
StyledMenu {
    id: root

    StyledMenuItem {
        text: Player.playing ? "Pause" : "Play"
        symbol: Player.playing ? "pause" : "play_arrow"
        enabled: Player.hasSong
        onTriggered: Player.togglePlayback()
    }
    StyledMenuItem {
        text: "Next Song"
        symbol: "skip_next"
        enabled: Player.canSkipForward
        onTriggered: Player.skipForward()
    }
    StyledMenuItem {
        text: "Previous Song"
        symbol: "skip_previous"
        enabled: Player.canSkipBackward
        onTriggered: Player.skipBackward()
    }
    StyledMenuSeparator {}
    StyledMenuItem {
        text: "Skip Forward 10 Seconds"
        symbol: "forward_10"
        enabled: Player.hasSong
        onTriggered: Player.seekBy(10000)
    }
    StyledMenuItem {
        text: "Skip Back 10 Seconds"
        symbol: "replay_10"
        enabled: Player.hasSong
        onTriggered: Player.seekBy(-10000)
    }
    StyledMenuSeparator {}
    StyledMenuItem {
        text: "Increase Volume"
        symbol: "volume_up"
        onTriggered: Player.adjustVolume(0.05)
    }
    StyledMenuItem {
        text: "Decrease Volume"
        symbol: "volume_down"
        onTriggered: Player.adjustVolume(-0.05)
    }
    StyledMenuItem {
        text: Player.volume <= 0.01 ? "Unmute" : "Mute"
        symbol: Player.volume <= 0.01 ? "volume_up" : "volume_off"
        onTriggered: Player.toggleMute()
    }
    StyledMenuSeparator {}
    StyledMenuItem {
        text: Player.shuffle ? "Shuffle Off" : "Shuffle On"
        symbol: "shuffle"
        enabled: Player.hasSong
        onTriggered: Player.toggleShuffle()
    }
    StyledMenuItem {
        text: Player.repeatMode === Player.RepeatMode.One ? "Repeat song" : (Player.repeatMode === Player.RepeatMode.All ? "Repeat queue" : "Repeat off")
        symbol: Player.repeatMode === Player.RepeatMode.One ? "repeat_one" : "repeat"
        enabled: Player.hasSong
        onTriggered: Player.cycleRepeatMode()
    }
    StyledMenuSeparator {}
    StyledMenuItem {
        text: "Settings…"
        symbol: "settings"
        onTriggered: UiState.settingsRequested()
    }
}
