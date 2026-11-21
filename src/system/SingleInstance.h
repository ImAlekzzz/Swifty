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

#include <QLocalServer>
#include <QObject>
#include <QStringList>
#include <QUrl>

namespace swifty {

/// one swifty per user. a second launch hands its command to the first and
/// exits: no command raises the window, and the others are for compositor
/// key binds (`swifty --immersive`, `swifty --play-pause`).
class SingleInstance : public QObject {
    Q_OBJECT

public:
    explicit SingleInstance(QObject *parent = nullptr);

    /// true when another copy took the command, and this one should exit
    bool forwardToRunningInstance(const QStringList &arguments);
    /// the command a launch's arguments ask for; "raise" without one. a swifty: link
    /// (swifty://play-pause) is one of the arguments
    static QString commandFrom(const QStringList &arguments);
    /// the command of a swifty: link, or nothing when it is not one. a link the app
    /// does not know raises the window. a web page can open a swifty: link by itself,
    /// so only harmless commands are known: none that changes a setting or names a file
    static QString commandFromLink(const QUrl &url);
    /// starts answering later launches
    void listen();

signals:
    void commandReceived(const QString &command);

private:
    static QString serverName();
    QLocalServer m_server;
};

} // namespace swifty
