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

#pragma once

#include <windows.h>

#include <functional>
#include <memory>
#include <string>

namespace swifty {

/// the system's media controls on Windows (System Media Transport Controls):
/// what the volume flyout, the lock screen and the media keys of keyboards and
/// headsets show and drive. it needs Windows 10 or later, and does nothing,
/// quietly, where it cannot start.
///
/// it knows nothing of Qt: button presses reach the handler on a thread of the
/// system's, which the caller moves to its own.
class MediaTransport {
public:
    enum class Button { Play, Pause, Stop, Next, Previous };
    enum class State { Closed, Stopped, Playing, Paused };
    struct Track {
        std::wstring title;
        std::wstring artist;
        std::wstring album;
        /// a url the system can load: a file:/// one for a cover on this computer
        std::wstring artwork;
    };
    using ButtonHandler = std::function<void(Button)>;

    MediaTransport(HWND window, ButtonHandler handler);
    ~MediaTransport();
    MediaTransport(const MediaTransport &) = delete;
    MediaTransport &operator=(const MediaTransport &) = delete;

    /// whether the system accepted the window
    bool valid() const;
    /// what is playing; an empty title clears it
    void setTrack(const Track &track);
    void setState(State state);
    /// which buttons can be pressed
    void setAvailable(bool play, bool pause, bool next, bool previous);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace swifty
