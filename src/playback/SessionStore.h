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

#include <QList>
#include <QObject>
#include <QTimer>

#include <functional>

#include "core/MusicTypes.h"

namespace swifty {

class PlaybackController;
class Settings;

/// remembers what was playing, and where: the song, the queue around it and the
/// place in it, so that opening the app again offers it where it was left
/// (paused: nothing makes sound until play is pressed)
class SessionStore : public QObject {
    Q_OBJECT

public:
    /// `catalog` is every song there is, which the saved ones are found among
    using Catalog = std::function<QList<Song>()>;

    SessionStore(Settings *settings, PlaybackController *player, Catalog catalog, QObject *parent = nullptr);

    /// puts back what was saved. false when nothing is, something is already
    /// playing, or the songs are not known yet (then it can be asked again)
    bool restore();

public slots:
    void save();
    /// for when the library has been read: asks again, once
    void restoreIfPending() { restore(); }

private:
    Settings *m_settings;
    PlaybackController *m_player;
    Catalog m_catalog;
    QTimer m_whilePlaying;
    QTimer m_afterChange;
    bool m_restored = false;
};

} // namespace swifty
