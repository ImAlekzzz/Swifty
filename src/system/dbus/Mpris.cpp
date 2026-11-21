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

#include "system/dbus/Mpris.h"

#include "core/Diagnostics.h"
#include "playback/PlaybackController.h"

#include <QCoreApplication>
#include <QDBusError>
#include <QDBusConnectionInterface>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QLoggingCategory>
#include <QRegularExpression>

#include <cstdio>

namespace swifty {

Q_LOGGING_CATEGORY(lcMpris, "swifty.mpris")

namespace {

/// a failure, straight to the terminal, past Qt's logging rules, which some
/// desktops turn off: this is what tells why a shell cannot see the player
void say(const QString &text)
{
    std::fprintf(stderr, "swifty mpris: %s\n", qPrintable(text));
    std::fflush(stderr);
}

/// what is only status, told when SWIFTY_DEBUG is set
void note(const QString &text)
{
    diagnostic("mpris", text);
}

const QString kObjectPath = QStringLiteral("/org/mpris/MediaPlayer2");
const QString kPlayerInterface = QStringLiteral("org.mpris.MediaPlayer2.Player");
constexpr qint64 kConnectionPlayGraceMs = 3000;

QDBusObjectPath trackPath(const Song &song)
{
    // a path holds only letters, digits and underscores: a file goes by a
    // hash of where it is
    const QString id = QStringLiteral("local_") + QString::number(qHash(song.uri), 16);
    return QDBusObjectPath(QStringLiteral("/dev/imalekzzz/swifty/track/") + id);
}

/// "A, B" is two artists, which is how the spec wants them
QStringList artistList(const Song &song)
{
    QStringList artists;
    for (const QString &part : song.artists.split(QStringLiteral(", "), Qt::SkipEmptyParts)) {
        if (!part.trimmed().isEmpty()) {
            artists.append(part.trimmed());
        }
    }
    return artists.isEmpty() ? QStringList {song.artists} : artists;
}

QStringList genreList(const Song &song)
{
    QStringList genres;
    for (const QString &part : song.genre.split(QRegularExpression(QStringLiteral("\\s*[;/]\\s*")), Qt::SkipEmptyParts)) {
        genres.append(part.trimmed());
    }
    return genres.isEmpty() ? QStringList {song.genre} : genres;
}

} // namespace

Mpris::Mpris(PlaybackController *player, QObject *parent)
    : QObject(parent)
    , m_player(player)
{
    new MprisRootAdaptor(this);
    new MprisPlayerAdaptor(this);

    QDBusConnection bus = QDBusConnection::sessionBus();
    note(QStringLiteral("starting; bus %1, connected %2, logging rules \"%3\"")
            .arg(QString::fromLocal8Bit(qgetenv("DBUS_SESSION_BUS_ADDRESS")), bus.isConnected() ? QStringLiteral("yes") : QStringLiteral("no"),
                 QString::fromLocal8Bit(qgetenv("QT_LOGGING_RULES"))));
    if (!bus.isConnected()) {
        say(QStringLiteral("no session bus: %1").arg(bus.lastError().message()));
        qCInfo(lcMpris) << "no session bus; media keys will not reach swifty";
        return;
    }
    if (!bus.registerObject(kObjectPath, this)) {
        say(QStringLiteral("could not register the media player object: %1").arg(bus.lastError().message()));
        qCWarning(lcMpris) << "could not register the media player object";
        return;
    }
    // a second copy of swifty would clash on the plain name. what the bus says
    // when it refuses is kept, since this is where a shell finds the player
    const auto claim = [&bus](const QString &name) {
        if (bus.registerService(name)) {
            return true;
        }
        const QDBusError error = bus.lastError();
        say(QStringLiteral("the bus did not give the name %1: %2").arg(name, error.isValid() ? error.name() + QStringLiteral(": ") + error.message() : QStringLiteral("(no error: another program has it)")));
        qCWarning(lcMpris) << "the session bus did not give the name" << name
                           << (error.isValid() ? error.name() + QStringLiteral(": ") + error.message()
                                               : QStringLiteral("(no error: another program already has it)"));
        return false;
    };
    const QString instanceName = QStringLiteral("org.mpris.MediaPlayer2.swifty.instance%1").arg(QCoreApplication::applicationPid());
    m_registered = claim(QStringLiteral("org.mpris.MediaPlayer2.swifty")) || claim(instanceName);

    if (m_registered) {
        const bool owned = bus.interface() && (bus.interface()->isServiceRegistered(QStringLiteral("org.mpris.MediaPlayer2.swifty")).value()
                                               || bus.interface()->isServiceRegistered(instanceName).value());
        note(QStringLiteral("media controls offered on the session bus, connection %1, the bus %2 the name")
                .arg(bus.baseService(), owned ? QStringLiteral("lists") : QStringLiteral("does NOT list")));
        qCInfo(lcMpris) << "media controls offered on the session bus, connection" << bus.baseService()
                        << (owned ? "(the bus lists the name)" : "(the bus does NOT list the name)");
    } else {
        say(QStringLiteral("no org.mpris.MediaPlayer2 name could be taken, so no shell can see swifty"));
        qCWarning(lcMpris) << "no org.mpris.MediaPlayer2 name could be taken on the session bus, so no shell can see swifty";
    }

    connect(m_player, &PlaybackController::mediaStateChanged, this, &Mpris::update);
    connect(m_player, &PlaybackController::volumeChanged, this, &Mpris::update);
    connect(m_player, &PlaybackController::shuffleChanged, this, &Mpris::update);
    connect(m_player, &PlaybackController::repeatModeChanged, this, &Mpris::update);
    update();
}

Mpris::~Mpris()
{
    if (m_registered) {
        QDBusConnection::sessionBus().unregisterObject(kObjectPath);
    }
}

void Mpris::noteOutputArrived()
{
    m_outputArrivedAt = QDateTime::currentDateTimeUtc();
}

bool Mpris::startIsRefused() const
{
    return m_outputArrivedAt.isValid() && m_outputArrivedAt.msecsTo(QDateTime::currentDateTimeUtc()) < kConnectionPlayGraceMs;
}

bool Mpris::hasSong() const
{
    return m_player->hasSong();
}

bool Mpris::canSeek() const
{
    return m_player->hasSong() && m_player->durationMs() > 0;
}

bool Mpris::canGoNext() const
{
    return m_player->canSkipForward();
}

bool Mpris::canGoPrevious() const
{
    return m_player->canSkipBackward();
}

QString Mpris::playbackStatus() const
{
    if (!m_player->hasSong()) {
        return QStringLiteral("Stopped");
    }
    return m_player->isPlaying() ? QStringLiteral("Playing") : QStringLiteral("Paused");
}

qlonglong Mpris::positionMicroseconds() const
{
    return qlonglong(m_player->livePositionMs()) * 1000;
}

QVariantMap Mpris::metadata() const
{
    if (!m_player->hasSong()) {
        return {{QStringLiteral("mpris:trackid"), QVariant::fromValue(QDBusObjectPath(QStringLiteral("/org/mpris/MediaPlayer2/TrackList/NoTrack")))}};
    }
    const Song song = m_player->currentSong();
    const qlonglong durationMs = std::max<qlonglong>(m_player->durationMs(), song.durationMs);
    QVariantMap map {
        {QStringLiteral("mpris:trackid"), QVariant::fromValue(trackPath(song))},
        {QStringLiteral("xesam:title"), song.name},
        {QStringLiteral("xesam:artist"), artistList(song)},
        {QStringLiteral("xesam:url"), song.uri},
    };
    if (!song.albumName.isEmpty()) {
        map.insert(QStringLiteral("xesam:album"), song.albumName);
    }
    if (!song.genre.isEmpty()) {
        map.insert(QStringLiteral("xesam:genre"), genreList(song));
    }
    if (durationMs > 0) {
        map.insert(QStringLiteral("mpris:length"), durationMs * 1000);
    }
    if (song.artworkUrl.isValid()) {
        map.insert(QStringLiteral("mpris:artUrl"), song.artworkUrl.toString());
    }
    return map;
}

void Mpris::update()
{
    if (!m_registered) {
        return;
    }
    QVariantMap changed;
    const auto publish = [&changed](const QString &name, auto &published, const auto &value) {
        if (published != value) {
            published = value;
            changed.insert(name, QVariant::fromValue(value));
        }
    };
    publish(QStringLiteral("PlaybackStatus"), m_publishedStatus, playbackStatus());
    publish(QStringLiteral("Metadata"), m_publishedMetadata, metadata());
    publish(QStringLiteral("CanGoNext"), m_publishedCanGoNext, canGoNext());
    publish(QStringLiteral("CanGoPrevious"), m_publishedCanGoPrevious, canGoPrevious());
    publish(QStringLiteral("CanSeek"), m_publishedCanSeek, canSeek());
    const bool songPresent = hasSong();
    if (m_publishedHasSong != songPresent) {
        m_publishedHasSong = songPresent;
        changed.insert(QStringLiteral("CanPlay"), songPresent);
        changed.insert(QStringLiteral("CanPause"), songPresent);
    }
    publish(QStringLiteral("Volume"), m_publishedVolume, m_player->volume());
    publish(QStringLiteral("Shuffle"), m_publishedShuffle, m_player->isShuffleEnabled());
    const QString loop = m_player->repeatMode() == PlaybackController::RepeatMode::One ? QStringLiteral("Track")
        : m_player->repeatMode() == PlaybackController::RepeatMode::All                ? QStringLiteral("Playlist")
                                                                                       : QStringLiteral("None");
    publish(QStringLiteral("LoopStatus"), m_publishedLoop, loop);
    if (!changed.isEmpty()) {
        emitPropertiesChanged(kPlayerInterface, changed);
    }

    // clients run the clock themselves from the playback state, so only a
    // position that drifted from where that clock says it is, a seek or a
    // stall, is news.
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const qlonglong position = positionMicroseconds();
    const qlonglong elapsed = m_lastPositionAt.isValid() && m_player->isPlaying() ? m_lastPositionAt.msecsTo(now) * 1000 : 0;
    if (std::llabs(m_lastPositionUs + elapsed - position) >= 1'000'000 || changed.contains(QStringLiteral("Metadata"))) {
        if (auto *adaptor = findChild<MprisPlayerAdaptor *>()) {
            emit adaptor->Seeked(position);
        }
    }
    m_lastPositionUs = position;
    m_lastPositionAt = now;
}

void Mpris::emitPropertiesChanged(const QString &interface, const QVariantMap &changed)
{
    QDBusMessage signal = QDBusMessage::createSignal(kObjectPath, QStringLiteral("org.freedesktop.DBus.Properties"),
                                                     QStringLiteral("PropertiesChanged"));
    signal << interface << changed << QStringList {};
    QDBusConnection::sessionBus().send(signal);
}

MprisRootAdaptor::MprisRootAdaptor(Mpris *mpris)
    : QDBusAbstractAdaptor(mpris)
    , m_mpris(mpris)
{
}

void MprisRootAdaptor::Raise()
{
    emit m_mpris->raiseRequested();
}

void MprisRootAdaptor::Quit()
{
    emit m_mpris->quitRequested();
}

MprisPlayerAdaptor::MprisPlayerAdaptor(Mpris *mpris)
    : QDBusAbstractAdaptor(mpris)
    , m_mpris(mpris)
{
}

QString MprisPlayerAdaptor::loopStatus() const
{
    switch (m_mpris->player()->repeatMode()) {
    case PlaybackController::RepeatMode::One: return QStringLiteral("Track");
    case PlaybackController::RepeatMode::All: return QStringLiteral("Playlist");
    default: return QStringLiteral("None");
    }
}

void MprisPlayerAdaptor::setLoopStatus(const QString &status)
{
    const auto target = status == QLatin1StringView("Track") ? PlaybackController::RepeatMode::One
        : status == QLatin1StringView("Playlist")             ? PlaybackController::RepeatMode::All
                                                              : PlaybackController::RepeatMode::Off;
    for (int step = 0; step < 3 && m_mpris->player()->repeatMode() != target; ++step) {
        m_mpris->player()->cycleRepeatMode();
    }
}

bool MprisPlayerAdaptor::shuffle() const
{
    return m_mpris->player()->isShuffleEnabled();
}

void MprisPlayerAdaptor::setShuffle(bool shuffle)
{
    if (m_mpris->player()->isShuffleEnabled() != shuffle) {
        m_mpris->player()->toggleShuffle();
    }
}

double MprisPlayerAdaptor::volume() const
{
    return m_mpris->player()->volume();
}

void MprisPlayerAdaptor::setVolume(double volume)
{
    m_mpris->player()->setVolume(volume);
}

void MprisPlayerAdaptor::Next()
{
    m_mpris->player()->skipForward();
}

void MprisPlayerAdaptor::Previous()
{
    m_mpris->player()->skipBackward();
}

void MprisPlayerAdaptor::Pause()
{
    note(QStringLiteral("another program asked to pause (Pause)"));
    // pause is always honoured
    m_mpris->player()->pause();
}

void MprisPlayerAdaptor::PlayPause()
{
    note(QStringLiteral("another program sent PlayPause"));
    if (m_mpris->player()->isPlaying()) {
        m_mpris->player()->pause();
    } else if (!m_mpris->startIsRefused()) {
        m_mpris->player()->togglePlayback();
    }
}

void MprisPlayerAdaptor::Stop()
{
    note(QStringLiteral("another program asked to stop (Stop)"));
    m_mpris->player()->pause();
}

void MprisPlayerAdaptor::Play()
{
    if (!m_mpris->startIsRefused() && !m_mpris->player()->isPlaying()) {
        m_mpris->player()->resume();
    }
}

void MprisPlayerAdaptor::Seek(qlonglong offset)
{
    m_mpris->player()->seekBy(int(offset / 1000));
}

void MprisPlayerAdaptor::SetPosition(const QDBusObjectPath &trackId, qlonglong position)
{
    // a position for a track that has since changed is dropped, as the spec asks.
    if (!m_mpris->player()->hasSong() || trackId != trackPath(m_mpris->player()->currentSong())) {
        return;
    }
    m_mpris->player()->seek(int(position / 1000));
}

} // namespace swifty
