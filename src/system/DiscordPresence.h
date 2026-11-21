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

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QTimer>

class QLocalSocket;

namespace swifty {

class PlaybackController;
class Settings;

/// talks to the Discord app's local socket, on a thread of its own: looking
/// for the socket, and connecting to it, can take a while (a busy pipe on
/// windows makes the connect wait), and the window must never wait for it
class DiscordWorker : public QObject {
    Q_OBJECT

public:
    explicit DiscordWorker(QObject *parent = nullptr);

public slots:
    /// whether to show anything, and under which Discord application
    void configure(bool enabled, const QString &clientId);
    /// what to show: the json of an activity, or nothing for none
    void setActivity(const QByteArray &activityJson);
    /// takes the presence down and lets go of the socket
    void shutdown();

private:
    enum Opcode : qint32 { Handshake = 0, Frame = 1, Close = 2, Ping = 3, Pong = 4 };

    /// whether there is a reason to be connected: it is on, it has an
    /// application, and there is something to show
    bool wanted() const;
    void ensureSocket();
    void tryConnect();
    void disconnectFromDiscord();
    void onConnected();
    void onReadyRead();
    void send(Opcode opcode, const QByteArray &payload);
    void handleFrame(const QByteArray &payload);
    void sendActivity();
    static QStringList socketPaths();

    QLocalSocket *m_socket = nullptr;
    QTimer *m_retry = nullptr;
    QByteArray m_buffer;
    QByteArray m_activity;
    QString m_clientId;
    bool m_enabled = false;
    bool m_ready = false;
    int m_candidate = 0;
};

/// shows what is playing on the person's Discord profile ("Listening to
/// ..."), through the Discord desktop app. switched on and off, and given its
/// Discord application, in the settings (Settings.discordPresence and
/// Settings.discordClientId, which falls back to Swifty's own application). nothing happens if Discord is not running: it is
/// looked for again every so often while a song plays.
class DiscordPresence : public QObject {
    Q_OBJECT

public:
    DiscordPresence(PlaybackController *player, Settings *settings, QObject *parent = nullptr);
    ~DiscordPresence() override;

private:
    void pushConfiguration();
    /// asks for the presence to be sent soon: several changes at once make one update
    void schedule();
    void publish();

    PlaybackController *m_player;
    Settings *m_settings;
    QThread m_thread;
    DiscordWorker *m_worker;
    QTimer m_debounce;
    /// the start of the song as last sent, to tell a seek from the clock running
    qint64 m_sentStartMs = 0;
};

} // namespace swifty
