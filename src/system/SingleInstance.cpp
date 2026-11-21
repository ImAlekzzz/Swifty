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

#include "system/SingleInstance.h"

#include <QDir>
#include <QLocalSocket>

namespace swifty {

QString SingleInstance::commandFromLink(const QUrl &url)
{
    static const QStringList known {
        QStringLiteral("raise"),     QStringLiteral("play-pause"),       QStringLiteral("next"),
        QStringLiteral("previous"),  QStringLiteral("immersive"),        QStringLiteral("close-immersive"),
        QStringLiteral("home"),      QStringLiteral("playlists"),        QStringLiteral("songs"),
        QStringLiteral("favourites"), QStringLiteral("albums"),          QStringLiteral("artists"),
        QStringLiteral("back"),      QStringLiteral("play-recent"),
    };
    if (url.scheme().compare(QLatin1StringView("swifty"), Qt::CaseInsensitive) != 0) {
        return {};
    }
    // swifty://play-pause names it as the host, swifty:play-pause and swifty:///play-pause as the path
    QString name = url.host();
    if (name.isEmpty()) {
        name = url.path();
    }
    name.remove(u'/');
    name = name.trimmed().toLower();
    return known.contains(name) ? name : QStringLiteral("raise");
}

QString SingleInstance::commandFrom(const QStringList &arguments)
{
    for (const QString &argument : arguments.mid(1)) {
        if (argument.startsWith(QLatin1StringView("swifty:"), Qt::CaseInsensitive)) {
            return commandFromLink(QUrl(argument));
        }
        if (argument == QLatin1StringView("--immersive")) return QStringLiteral("immersive");
        if (argument == QLatin1StringView("--play-pause")) return QStringLiteral("play-pause");
        if (argument == QLatin1StringView("--next")) return QStringLiteral("next");
        if (argument == QLatin1StringView("--previous")) return QStringLiteral("previous");
    }
    return QStringLiteral("raise");
}

SingleInstance::SingleInstance(QObject *parent)
    : QObject(parent)
{
    connect(&m_server, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket *socket = m_server.nextPendingConnection()) {
            connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QLocalSocket::readyRead, this, [this, socket] {
                while (socket->canReadLine()) {
                    emit commandReceived(QString::fromUtf8(socket->readLine()).trimmed());
                }
            });
        }
    });
}

QString SingleInstance::serverName()
{
    return QStringLiteral("swifty-") + QDir::home().dirName();
}

bool SingleInstance::forwardToRunningInstance(const QStringList &arguments)
{
    QLocalSocket socket;
    socket.connectToServer(serverName());
    if (!socket.waitForConnected(300)) {
        return false;
    }
    socket.write(commandFrom(arguments).toUtf8() + '\n');
    socket.waitForBytesWritten(300);
    socket.disconnectFromServer();
    return true;
}

void SingleInstance::listen()
{
    // a crashed copy leaves its socket file behind; nobody is answering on it.
    QLocalServer::removeServer(serverName());
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    m_server.listen(serverName());
}

} // namespace swifty
