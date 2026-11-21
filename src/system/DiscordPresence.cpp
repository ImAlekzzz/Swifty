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

#include "system/DiscordPresence.h"

#include "app/Settings.h"
#include "playback/PlaybackController.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QLoggingCategory>
#include <QUuid>
#include <QtEndian>

#include <cstdlib>
#include <cstring>
#include <utility>

namespace swifty {

Q_LOGGING_CATEGORY(lcDiscord, "swifty.discord")

namespace {

constexpr int kRetryMs = 15'000;
constexpr int kDebounceMs = 400;
/// a position this far from where the clock says it should be was a seek
constexpr qint64 kSeekDriftMs = 2'500;
constexpr int kMaxSockets = 10;

/// discord asks for 2 to 128 characters
QString fitted(QString text)
{
    text = text.trimmed();
    if (text.size() > 128) {
        text = text.left(125) + QStringLiteral("...");
    }
    while (text.size() < 2) {
        text += QChar(0x2800);
    }
    return text;
}

} // namespace

// --- the worker, on its own thread --------------------------------------------------------

DiscordWorker::DiscordWorker(QObject *parent)
    : QObject(parent)
{
}

QStringList DiscordWorker::socketPaths()
{
    QStringList paths;
#ifdef Q_OS_WIN
    for (int i = 0; i < kMaxSockets; ++i) {
        paths.append(QStringLiteral("discord-ipc-%1").arg(i));
    }
#else
    QStringList bases;
    for (const char *name : {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"}) {
        if (const QString value = qEnvironmentVariable(name); !value.isEmpty() && !bases.contains(value)) {
            bases.append(value);
        }
    }
    if (!bases.contains(QStringLiteral("/tmp"))) {
        bases.append(QStringLiteral("/tmp"));
    }
    // discord installed as a flatpak or a snap keeps its socket in a folder of its own
    const QStringList inside {QString(), QStringLiteral("/app/com.discordapp.Discord"),
                              QStringLiteral("/snap.discord"), QStringLiteral("/snap.discord-canary")};
    for (const QString &base : bases) {
        for (const QString &folder : inside) {
            for (int i = 0; i < kMaxSockets; ++i) {
                paths.append(QStringLiteral("%1%2/discord-ipc-%3").arg(base, folder).arg(i));
            }
        }
    }
#endif
    return paths;
}

bool DiscordWorker::wanted() const
{
    return m_enabled && !m_clientId.isEmpty() && !m_activity.isEmpty();
}

void DiscordWorker::ensureSocket()
{
    if (m_socket) {
        return;
    }
    // made here, on the worker's thread, so that everything of it lives there
    m_socket = new QLocalSocket(this);
    connect(m_socket, &QLocalSocket::connected, this, &DiscordWorker::onConnected);
    connect(m_socket, &QLocalSocket::readyRead, this, &DiscordWorker::onReadyRead);
    connect(m_socket, &QLocalSocket::disconnected, this, [this] {
        m_ready = false;
        m_buffer.clear();
    });
    connect(m_socket, &QLocalSocket::errorOccurred, this, [this] {
        if (m_socket->state() == QLocalSocket::UnconnectedState) {
            // that socket is not there: the next one is tried, and when none is, Discord is waited for
            m_ready = false;
            if (++m_candidate < socketPaths().size() && wanted()) {
                QMetaObject::invokeMethod(this, &DiscordWorker::tryConnect, Qt::QueuedConnection);
            } else {
                m_candidate = 0;
            }
        }
    });
    m_retry = new QTimer(this);
    m_retry->setInterval(kRetryMs);
    connect(m_retry, &QTimer::timeout, this, &DiscordWorker::tryConnect);
}

void DiscordWorker::configure(bool enabled, const QString &clientId)
{
    const bool applicationChanged = clientId != m_clientId;
    m_enabled = enabled;
    m_clientId = clientId;
    if (!wanted()) {
        if (m_ready && m_socket) {
            // switched off: what was shown goes
            const QByteArray before = std::exchange(m_activity, {});
            sendActivity();
            m_activity = before;
        }
        disconnectFromDiscord();
        return;
    }
    if (applicationChanged) {
        disconnectFromDiscord();
    }
    tryConnect();
}

void DiscordWorker::setActivity(const QByteArray &activityJson)
{
    m_activity = activityJson;
    if (m_ready) {
        sendActivity();
    } else if (wanted()) {
        tryConnect();
    }
}

void DiscordWorker::shutdown()
{
    if (m_ready && m_socket) {
        m_activity.clear();
        sendActivity();
        m_socket->flush();
    }
    disconnectFromDiscord();
}

void DiscordWorker::tryConnect()
{
    if (!wanted()) {
        if (m_retry) {
            m_retry->stop();
        }
        return;
    }
    ensureSocket();
    m_retry->start();
    if (m_ready || m_socket->state() != QLocalSocket::UnconnectedState) {
        return;
    }
    const QStringList paths = socketPaths();
    if (m_candidate >= paths.size()) {
        m_candidate = 0;
    }
    m_socket->connectToServer(paths.at(m_candidate));
}

void DiscordWorker::disconnectFromDiscord()
{
    if (m_retry) {
        m_retry->stop();
    }
    if (m_socket) {
        m_socket->abort();
    }
    m_ready = false;
    m_candidate = 0;
    m_buffer.clear();
}

void DiscordWorker::onConnected()
{
    m_buffer.clear();
    m_ready = false;
    const QJsonObject hello {{QStringLiteral("v"), 1}, {QStringLiteral("client_id"), m_clientId}};
    send(Handshake, QJsonDocument(hello).toJson(QJsonDocument::Compact));
}

void DiscordWorker::send(Opcode opcode, const QByteArray &payload)
{
    // an opcode and a length, each four bytes and little endian, then the json
    QByteArray packet(8, '\0');
    const qint32 header[2] = {qToLittleEndian<qint32>(opcode), qToLittleEndian<qint32>(qint32(payload.size()))};
    std::memcpy(packet.data(), header, sizeof(header));
    packet.append(payload);
    m_socket->write(packet);
}

void DiscordWorker::onReadyRead()
{
    m_buffer.append(m_socket->readAll());
    while (m_buffer.size() >= 8) {
        qint32 header[2];
        std::memcpy(header, m_buffer.constData(), sizeof(header));
        const qint32 opcode = qFromLittleEndian<qint32>(header[0]);
        const qint32 length = qFromLittleEndian<qint32>(header[1]);
        if (length < 0 || length > (1 << 20)) {
            m_socket->abort();
            return;
        }
        if (m_buffer.size() < 8 + length) {
            return;
        }
        const QByteArray payload = m_buffer.mid(8, length);
        m_buffer.remove(0, 8 + length);
        switch (opcode) {
        case Frame: handleFrame(payload); break;
        case Ping: send(Pong, payload); break;
        case Close:
            qCDebug(lcDiscord).noquote() << "Discord closed the connection:" << QString::fromUtf8(payload);
            m_socket->abort();
            return;
        default: break;
        }
    }
}

void DiscordWorker::handleFrame(const QByteArray &payload)
{
    const QJsonObject message = QJsonDocument::fromJson(payload).object();
    const QString event = message.value(QStringLiteral("evt")).toString();
    if (event == QLatin1StringView("READY")) {
        qCDebug(lcDiscord) << "connected to Discord";
        m_ready = true;
        sendActivity();
    } else if (event == QLatin1StringView("ERROR")) {
        qCDebug(lcDiscord).noquote() << "Discord refused:" << QString::fromUtf8(payload);
    }
}

void DiscordWorker::sendActivity()
{
    if (!m_ready || !m_socket) {
        return;
    }
    QJsonObject args {{QStringLiteral("pid"), qint64(QCoreApplication::applicationPid())}};
    if (!m_activity.isEmpty()) {
        args.insert(QStringLiteral("activity"), QJsonDocument::fromJson(m_activity).object());
    }
    const QJsonObject command {
        {QStringLiteral("cmd"), QStringLiteral("SET_ACTIVITY")},
        {QStringLiteral("args"), args},
        {QStringLiteral("nonce"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
    };
    send(Frame, QJsonDocument(command).toJson(QJsonDocument::Compact));
}

// --- the part in the app -------------------------------------------------------------------

DiscordPresence::DiscordPresence(PlaybackController *player, Settings *settings, QObject *parent)
    : QObject(parent)
    , m_player(player)
    , m_settings(settings)
    , m_worker(new DiscordWorker)
{
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_thread.start();

    m_debounce.setSingleShot(true);
    m_debounce.setInterval(kDebounceMs);
    connect(&m_debounce, &QTimer::timeout, this, &DiscordPresence::publish);

    connect(player, &PlaybackController::currentSongChanged, this, &DiscordPresence::schedule);
    connect(player, &PlaybackController::playingChanged, this, &DiscordPresence::schedule);
    connect(player, &PlaybackController::durationChanged, this, &DiscordPresence::schedule);
    connect(player, &PlaybackController::positionChanged, this, [this] {
        const qint64 start = QDateTime::currentMSecsSinceEpoch() - qint64(m_player->livePositionMs());
        if (m_player->isPlaying() && std::llabs(start - m_sentStartMs) > kSeekDriftMs) {
            schedule();
        }
    });
    // the settings page: changes take effect at once
    connect(settings, &Settings::discordPresenceChanged, this, &DiscordPresence::pushConfiguration);
    connect(settings, &Settings::discordClientIdChanged, this, &DiscordPresence::pushConfiguration);
    if (QCoreApplication::instance()) {
        connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, [this] {
            QMetaObject::invokeMethod(m_worker, &DiscordWorker::shutdown, Qt::QueuedConnection);
        });
    }

    pushConfiguration();
    schedule();
}

DiscordPresence::~DiscordPresence()
{
    QMetaObject::invokeMethod(m_worker, &DiscordWorker::shutdown, Qt::QueuedConnection);
    m_thread.quit();
    // never wait for long on something that is not ours: a stuck connect is given up on
    if (!m_thread.wait(1500)) {
        m_thread.terminate();
        m_thread.wait(500);
    }
}

void DiscordPresence::pushConfiguration()
{
    const bool enabled = m_settings->discordPresence();
    const QString clientId = m_settings->effectiveDiscordClientId();
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, enabled, clientId] { worker->configure(enabled, clientId); },
                              Qt::QueuedConnection);
    // the song playing now is shown as soon as it is switched on
    schedule();
}

void DiscordPresence::schedule()
{
    m_debounce.start();
}

void DiscordPresence::publish()
{
    QByteArray activityJson;
    // a paused or stopped player shows nothing: Discord would keep counting
    if (m_settings->discordPresence() && m_player->hasSong() && m_player->isPlaying()) {
        const Song song = m_player->currentSong();
        const qint64 start = QDateTime::currentMSecsSinceEpoch() - qint64(m_player->livePositionMs());
        QJsonObject timestamps {{QStringLiteral("start"), start}};
        if (m_player->durationMs() > 0) {
            timestamps.insert(QStringLiteral("end"), start + m_player->durationMs());
        }
        QJsonObject activity {
            {QStringLiteral("type"), 2}, // listening
            {QStringLiteral("details"), fitted(song.name)},
            {QStringLiteral("timestamps"), timestamps},
        };
        if (!song.artists.isEmpty()) {
            activity.insert(QStringLiteral("state"), fitted(song.artists));
        }
        if (!song.albumName.isEmpty()) {
            activity.insert(QStringLiteral("assets"), QJsonObject {{QStringLiteral("large_text"), fitted(song.albumName)}});
        }
        m_sentStartMs = start;
        activityJson = QJsonDocument(activity).toJson(QJsonDocument::Compact);
    }
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, activityJson] { worker->setActivity(activityJson); },
                              Qt::QueuedConnection);
}

} // namespace swifty
