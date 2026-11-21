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

#include "app/AppController.h"
#include "app/ErrorCenter.h"
#include "app/Fonts.h"
#include "app/Paths.h"
#include "app/Settings.h"
#include "artwork/ArtworkCache.h"
#include "audio/AudioShaping.h"
#include "core/RustCore.h"
#include "library/LibraryStore.h"
#include "library/LocalLibrary.h"
#include "motion/ScrollMomentum.h"
#include "playback/SessionStore.h"
#include "library/ArtistImages.h"
#include "playback/LyricsFetcher.h"
#include "playback/PlaybackController.h"
#include "render/SpectrumSource.h"
#include "system/DiscordPresence.h"
#include "system/SingleInstance.h"
#include "theme/PlayerStyle.h"
#include "theme/Theme.h"

#ifdef SWIFTY_HAS_DBUS
#include "system/dbus/Mpris.h"
#include "system/dbus/SleepMonitor.h"
#endif
#ifdef SWIFTY_DESKTOP_INTEGRATION
#include "system/linux/AudioDeviceMonitor.h"
#endif
#ifdef Q_OS_MACOS
#include "system/macos/MacWindow.h"
#include "system/macos/NowPlaying.h"
#endif
#ifdef Q_OS_WIN
#include "system/windows/WindowsIntegration.h"
#endif

#if defined(Q_OS_LINUX) || defined(Q_OS_WIN)
#include <QApplication>
#else
#include <QGuiApplication>
#endif
#include <QEvent>
#include <QIcon>
#include <QPlatformSurfaceEvent>
#include <QSet>
#include <QWindow>
#include <QNetworkAccessManager>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QSocketNotifier>
#include <QtQml/qqmlextensionplugin.h>
#include <QFileOpenEvent>
#include <functional>
#include <utility>

#ifdef Q_OS_UNIX
#include <csignal>
#include <sys/socket.h>
#include <unistd.h>
#endif

#ifdef Q_OS_MACOS
#include <mach-o/dyld.h>
#include <unistd.h>

#include <climits>
#include <cstdint>
#include <cstdlib>
#endif

Q_IMPORT_QML_PLUGIN(SwiftyPlugin)

using namespace swifty;

namespace {

#ifdef Q_OS_UNIX
int s_quitSignals[2] = {-1, -1};

void forwardQuitSignal(int)
{
    const char byte = 1;
    [[maybe_unused]] const ssize_t written = ::write(s_quitSignals[1], &byte, 1);
}

/// a terminate (a kill, the session ending) quits as the menu does, rather
/// than stopping dead: what is still being kept gets written first. a second
/// one stops it at once
void quitGracefullyOnSignals(QCoreApplication &application)
{
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, s_quitSignals) != 0) {
        return;
    }
    auto *notifier = new QSocketNotifier(s_quitSignals[0], QSocketNotifier::Read, &application);
    QObject::connect(notifier, &QSocketNotifier::activated, &application, [notifier] {
        notifier->setEnabled(false);
        char byte = 0;
        [[maybe_unused]] const ssize_t read = ::read(s_quitSignals[0], &byte, 1);
        for (const int signal : {SIGTERM, SIGHUP, SIGINT}) {
            std::signal(signal, SIG_DFL);
        }
        QCoreApplication::quit();
    });
    struct sigaction action {};
    action.sa_handler = forwardQuitSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    for (const int signal : {SIGTERM, SIGHUP, SIGINT}) {
        sigaction(signal, &action, nullptr);
    }
}
#endif

/// how the scene graph keeps what it draws, set before anything is drawn.
/// a variable already in the environment wins, for measuring
void tuneSceneGraph()
{
    const auto fallback = [](const char *name, const char *value) {
        if (!qEnvironmentVariableIsSet(name)) {
            qputenv(name, value);
        }
    };
    // the shared texture small images are packed into is sized from the
    // window, rounded up to a power of two: 4096 by 2048, 32 MB of video
    // memory, for a retina window the main one's size. the only images the
    // app shows in any number are covers, drawn through a shader that cannot
    // read from it, so qt copied each small one back out into a texture of
    // its own and kept the shared one (4 MB at 1024 square) besides: nothing
    // goes in it, and it is kept small should anything ever do
    fallback("QSG_ATLAS_WIDTH", "64");
    fallback("QSG_ATLAS_HEIGHT", "64");
    fallback("QSG_ATLAS_SIZE_LIMIT", "1");
    // a decoded image is let go of once it is on the gpu, rather than kept
    // in memory beside its texture for as long as it shows: every cover on
    // screen was held twice. a window that draws an image another window
    // already drew loads its own (ArtworkImage.cache)
    fallback("QSG_TRANSIENT_IMAGES", "1");
    // icons are drawn into their atlas by a painter (MaterialSymbol), which
    // draws a glyph over 64 pixels by filling its outline rather than by the
    // platform's rasteriser, as qt's own text does at any size: a big
    // outlined icon came out a tenth lighter than it did as text
    fallback("QT_MAX_CACHED_GLYPH_SIZE", "512");
}

#ifdef Q_OS_MACOS
/// macos's allocator keeps what a process frees for its next allocations,
/// spread over fronts for each thread and cluster of cores: a third of the
/// heap's pages held nothing live, and counted against the app all the same.
/// asked as the process starts to favour space (MallocSpaceEfficient), it
/// holds a fraction of that, and the window draws no slower. it reads the
/// variable only then: the bundle sets it for a launch from the finder or
/// the dock (LSEnvironment), and a launch without it starts over with it
void startCompact(char *argv[])
{
    if (std::getenv("MallocSpaceEfficient") != nullptr) {
        return;
    }
    setenv("MallocSpaceEfficient", "1", 1);
    char path[PATH_MAX];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) == 0) {
        execv(path, argv);
    }
    // it could not start over; it carries on as it is
}

#ifdef Q_OS_MACOS
/// macos hands a swifty: link to the app as an event, not as an argument. one that comes
/// before the app is set up waits for it
class LinkOpener : public QObject {
public:
    using QObject::QObject;

    void ready(std::function<void(const QString &)> handle)
    {
        m_handle = std::move(handle);
        if (!m_pending.isEmpty()) {
            m_handle(std::exchange(m_pending, QString()));
        }
    }

protected:
    bool eventFilter(QObject *, QEvent *event) override
    {
        if (event->type() != QEvent::FileOpen) {
            return false;
        }
        const QString command = SingleInstance::commandFromLink(static_cast<QFileOpenEvent *>(event)->url());
        if (command.isEmpty()) {
            return false;
        }
        if (m_handle) {
            m_handle(command);
        } else {
            m_pending = command;
        }
        return true;
    }

private:
    std::function<void(const QString &)> m_handle;
    QString m_pending;
};
#endif

/// every window's metal layer keeps two drawables, and its view reports a
/// colour space that keeps its profile at hand (MacWindow.h). set as its
/// surface is made, and again when it is first shown, as the layer and the
/// frame may come only then
class WindowTuner : public QObject {
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *object, QEvent *event) override
    {
        const bool created = event->type() == QEvent::PlatformSurface
            && static_cast<QPlatformSurfaceEvent *>(event)->surfaceEventType() == QPlatformSurfaceEvent::SurfaceCreated;
        if (created || event->type() == QEvent::Expose) {
            if (auto *window = qobject_cast<QWindow *>(object)) {
                tune(window, m_drawables, macos::limitDrawables);
                tune(window, m_colorSpaces, macos::holdColorSpace);
            }
        }
        return false;
    }

private:
    void tune(QWindow *window, QSet<QWindow *> &done, bool (*step)(QWindow *))
    {
        if (done.contains(window) || !step(window)) {
            return;
        }
        done.insert(window);
        connect(window, &QObject::destroyed, this, [&done, window] { done.remove(window); });
    }

    QSet<QWindow *> m_drawables;
    QSet<QWindow *> m_colorSpaces;
};
#endif

} // namespace

int main(int argc, char *argv[])
{
#ifdef Q_OS_WIN
    WindowsIntegration::initializeProcess();
#endif
#ifdef Q_OS_MACOS
    startCompact(argv);
#endif
    tuneSceneGraph();
    QGuiApplication::setApplicationName(QStringLiteral("Swifty"));
    QGuiApplication::setApplicationVersion(QStringLiteral(SWIFTY_VERSION));
    // the wayland app id, which hyprland window rules and the .desktop file match.
    QGuiApplication::setDesktopFileName(QStringLiteral("swifty"));
    // the tray menu can fall back to widgets on linux
#if defined(Q_OS_LINUX) || defined(Q_OS_WIN)
    QApplication application(argc, argv);
#else
    QGuiApplication application(argc, argv);
#endif
#ifdef Q_OS_WIN
    application.setQuitOnLastWindowClosed(false);
#endif
#ifdef Q_OS_UNIX
    quitGracefullyOnSignals(application);
#endif
#ifndef Q_OS_MACOS
    // x11 shows this; wayland goes by the desktop entry, macos by the bundle
    QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/qt/qml/Swifty/resources/swifty-icon.png")));
#endif

    SingleInstance instance;
    if (instance.forwardToRunningInstance(QCoreApplication::arguments())) {
        return EXIT_SUCCESS;
    }
#ifdef Q_OS_MACOS
    WindowTuner windowTuner;
    application.installEventFilter(&windowTuner);
    LinkOpener linkOpener;
    application.installEventFilter(&linkOpener);
#endif
#ifdef Q_OS_LINUX
    // a trackpad swipe glides on after the fingers lift, as macos does itself
    // SWIFTY_NO_SCROLL_MOMENTUM=1 turns it off, to tell it from other scroll problems
    ScrollMomentum scrollMomentum;
    if (!qEnvironmentVariableIsSet("SWIFTY_NO_SCROLL_MOMENTUM")) {
        application.installEventFilter(&scrollMomentum);
    }
#endif

    // the plain style: every control swifty shows is drawn by its own components.
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    fonts::loadIconFont();
    fonts::loadInterfaceFont();

    Settings settings(nullptr);
    Theme theme(&settings);
    ErrorCenter errors(nullptr);
    QNetworkAccessManager network;
    RustCore core;
    // outlives the player, whose lookups may still be waiting on it at exit
    LyricsFetcher lyricsFetcher;
    PlaybackController player(&core, &settings, &errors);
    player.setLyricsFetcher(&lyricsFetcher);
    // outlives the library, which asks it for pictures
    ArtistImages artistImages(&settings);
    LibraryStore library(&settings, &errors);
    library.setArtistImages(&artistImages);
    library.attachPlayer(&player);
    AudioShaping sound(&core, &settings);
    LocalLibrary localFiles(&settings);
    library.attachLocalFiles(&localFiles);
    // a song whose tags were edited shows as it is now wherever it is queued
    QObject::connect(&localFiles, &LocalLibrary::scanned, &player,
                     [&] { player.refreshSongDetails(localFiles.allSongs()); });
    // what was playing comes back where it was left, paused
    SessionStore session(&settings, &player, [&localFiles] { return localFiles.allSongs(); });
    session.restore();
    QObject::connect(&localFiles, &LocalLibrary::scanned, &session, &SessionStore::restoreIfPending);
    AppController app(nullptr);
    ArtworkCache artwork(&network);
    PlayerStyle playerStyle(&player, &theme, &artwork);
    SpectrumSource spectrum(&core, &player);
    DiscordPresence discord(&player, &settings);

    QObject::connect(&instance, &SingleInstance::commandReceived, &app, &AppController::handleCommand);
    instance.listen();
#ifdef Q_OS_MACOS
    linkOpener.ready([&app](const QString &command) { app.handleCommand(command); });
#endif

#ifdef SWIFTY_HAS_DBUS
    Mpris mpris(&player);
    QObject::connect(&mpris, &Mpris::raiseRequested, &app, &AppController::raiseRequested);
    QObject::connect(&mpris, &Mpris::quitRequested, &app, &AppController::quit);
    SleepMonitor sleepMonitor(&player);
#endif
#ifdef Q_OS_MACOS
    // the media keys, headphone buttons and control centre's now playing
    NowPlaying nowPlaying(&player, &artwork);
#endif
#ifdef SWIFTY_DESKTOP_INTEGRATION
    AudioDeviceMonitor devices;
    QObject::connect(&devices, &AudioDeviceMonitor::outputLost, &player, &PlaybackController::invalidateAudioOutput);
    QObject::connect(&devices, &AudioDeviceMonitor::outputArrived, &mpris, &Mpris::noteOutputArrived);
    QObject::connect(&devices, &AudioDeviceMonitor::defaultOutputChanged, &player, &PlaybackController::refreshAudioOutput);
#endif

    QQmlApplicationEngine engine;
#ifdef Q_OS_WIN
    WindowsIntegration windowsIntegration(&player, &theme);
    engine.setInitialProperties({{QStringLiteral("visible"), false}});
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreated, &windowsIntegration,
                     [&windowsIntegration](QObject *object, const QUrl &) {
        if (auto *window = qobject_cast<QWindow *>(object)) {
            windowsIntegration.setWindow(window);
            window->show();
        }
    });
#endif
    engine.addImageProvider(QStringLiteral("art"), new ArtworkImageProvider);
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &application,
                     [] { QCoreApplication::exit(EXIT_FAILURE); }, Qt::QueuedConnection);
    engine.loadFromModule("Swifty", "Main");
    // the first launch carries out its own command too, once the window exists
    if (const QString command = SingleInstance::commandFrom(QCoreApplication::arguments()); command != QLatin1StringView("raise")) {
        app.handleCommand(command);
    }

    return application.exec();
}
