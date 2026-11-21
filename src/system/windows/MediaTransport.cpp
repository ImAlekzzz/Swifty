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

#include "system/windows/MediaTransport.h"

// c++/winrt's own headers, from the windows sdk. the interop one is the way to
// get the controls of a window that is not a uwp app's
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Storage.Streams.h>

#include <systemmediatransportcontrolsinterop.h>

#include <utility>

namespace swifty {

namespace wm = winrt::Windows::Media;
namespace ws = winrt::Windows::Storage::Streams;

struct MediaTransport::Impl {
    wm::SystemMediaTransportControls controls {nullptr};
    winrt::event_token pressed {};
    bool listening = false;
    ButtonHandler handler;
};

MediaTransport::MediaTransport(HWND window, ButtonHandler handler)
    : m_impl(std::make_unique<Impl>())
{
    m_impl->handler = std::move(handler);
    try {
        const auto interop = winrt::get_activation_factory<wm::SystemMediaTransportControls, ISystemMediaTransportControlsInterop>();
        winrt::check_hresult(interop->GetForWindow(window, winrt::guid_of<wm::SystemMediaTransportControls>(),
                                                   winrt::put_abi(m_impl->controls)));
        m_impl->controls.IsEnabled(true);
        m_impl->controls.DisplayUpdater().Type(wm::MediaPlaybackType::Music);

        Impl *impl = m_impl.get();
        m_impl->pressed = m_impl->controls.ButtonPressed(
            [impl](wm::SystemMediaTransportControls const &, wm::SystemMediaTransportControlsButtonPressedEventArgs const &args) {
                if (!impl->handler) {
                    return;
                }
                switch (args.Button()) {
                case wm::SystemMediaTransportControlsButton::Play: impl->handler(Button::Play); break;
                case wm::SystemMediaTransportControlsButton::Pause: impl->handler(Button::Pause); break;
                case wm::SystemMediaTransportControlsButton::Stop: impl->handler(Button::Stop); break;
                case wm::SystemMediaTransportControlsButton::Next: impl->handler(Button::Next); break;
                case wm::SystemMediaTransportControlsButton::Previous: impl->handler(Button::Previous); break;
                default: break;
                }
            });
        m_impl->listening = true;
    } catch (winrt::hresult_error const &) {
        // an older windows, or one that will not give the window's controls
        m_impl->controls = nullptr;
    }
}

MediaTransport::~MediaTransport()
{
    if (!m_impl || !m_impl->controls) {
        return;
    }
    try {
        // nothing is called into what is going away
        m_impl->handler = nullptr;
        if (m_impl->listening) {
            m_impl->controls.ButtonPressed(m_impl->pressed);
        }
        m_impl->controls.DisplayUpdater().ClearAll();
        m_impl->controls.PlaybackStatus(wm::MediaPlaybackStatus::Closed);
        m_impl->controls.IsEnabled(false);
    } catch (winrt::hresult_error const &) {
    }
}

bool MediaTransport::valid() const
{
    return m_impl && m_impl->controls;
}

void MediaTransport::setState(State state)
{
    if (!valid()) {
        return;
    }
    try {
        wm::MediaPlaybackStatus status = wm::MediaPlaybackStatus::Closed;
        switch (state) {
        case State::Closed: status = wm::MediaPlaybackStatus::Closed; break;
        case State::Stopped: status = wm::MediaPlaybackStatus::Stopped; break;
        case State::Playing: status = wm::MediaPlaybackStatus::Playing; break;
        case State::Paused: status = wm::MediaPlaybackStatus::Paused; break;
        }
        m_impl->controls.PlaybackStatus(status);
    } catch (winrt::hresult_error const &) {
    }
}

void MediaTransport::setAvailable(bool play, bool pause, bool next, bool previous)
{
    if (!valid()) {
        return;
    }
    try {
        m_impl->controls.IsPlayEnabled(play);
        m_impl->controls.IsPauseEnabled(pause);
        m_impl->controls.IsNextEnabled(next);
        m_impl->controls.IsPreviousEnabled(previous);
    } catch (winrt::hresult_error const &) {
    }
}

void MediaTransport::setTrack(const Track &track)
{
    if (!valid()) {
        return;
    }
    try {
        const wm::SystemMediaTransportControlsDisplayUpdater updater = m_impl->controls.DisplayUpdater();
        if (track.title.empty()) {
            updater.ClearAll();
            updater.Type(wm::MediaPlaybackType::Music);
            updater.Update();
            return;
        }
        updater.Type(wm::MediaPlaybackType::Music);
        const wm::MusicDisplayProperties music = updater.MusicProperties();
        music.Title(winrt::hstring {track.title});
        music.Artist(winrt::hstring {track.artist});
        music.AlbumTitle(winrt::hstring {track.album});

        // the cover: a reference to the file, which the system reads itself
        if (!track.artwork.empty()) {
            updater.Thumbnail(ws::RandomAccessStreamReference::CreateFromUri(winrt::Windows::Foundation::Uri {winrt::hstring {track.artwork}}));
        } else {
            updater.Thumbnail(ws::RandomAccessStreamReference {nullptr});
        }
        updater.Update();
    } catch (winrt::hresult_error const &) {
        // a cover the system cannot read must not cost the title: try without it
        try {
            const wm::SystemMediaTransportControlsDisplayUpdater updater = m_impl->controls.DisplayUpdater();
            updater.Thumbnail(ws::RandomAccessStreamReference {nullptr});
            updater.Update();
        } catch (winrt::hresult_error const &) {
        }
    }
}

} // namespace swifty
