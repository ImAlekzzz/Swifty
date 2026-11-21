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

#include <QObject>
#include <QSettings>
#include <QString>
#include <QVariant>
#include <QtQml/qqmlregistration.h>

class QQmlEngine;
class QJSEngine;

namespace swifty {

/// the app's preferences, under the macos app's key names and defaults
/// (AppDesign.swift) so the two read the same way. stores that keep their own
/// data (volume, pins, search history, the equalizer) use value() and setValue().
class Settings : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool visualizersEnabled READ visualizersEnabled WRITE setVisualizersEnabled NOTIFY visualizersEnabledChanged FINAL)
    Q_PROPERTY(bool circularVisualizer READ circularVisualizer WRITE setCircularVisualizer NOTIFY circularVisualizerChanged FINAL)
    /// the players' covers round, with their squiggle edge, while visualizers
    /// are off; with them on, the circular style rounds them
    Q_PROPERTY(bool roundArtwork READ roundArtwork WRITE setRoundArtwork NOTIFY roundArtworkChanged FINAL)
    Q_PROPERTY(QString visualizerShape READ visualizerShape WRITE setVisualizerShape NOTIFY visualizerShapeChanged FINAL)
    Q_PROPERTY(double visualizerSensitivity READ visualizerSensitivity WRITE setVisualizerSensitivity NOTIFY visualizerSensitivityChanged FINAL)
    Q_PROPERTY(bool livingBackdrop READ livingBackdrop WRITE setLivingBackdrop NOTIFY livingBackdropChanged FINAL)
    Q_PROPERTY(double backdropBlur READ backdropBlur WRITE setBackdropBlur NOTIFY backdropBlurChanged FINAL)
    Q_PROPERTY(double backdropReactivity READ backdropReactivity WRITE setBackdropReactivity NOTIFY backdropReactivityChanged FINAL)
    Q_PROPERTY(bool debuggingEnabled READ debuggingEnabled WRITE setDebuggingEnabled NOTIFY debuggingEnabledChanged FINAL)
    Q_PROPERTY(bool pinnedPlaylistsExpanded READ pinnedPlaylistsExpanded WRITE setPinnedPlaylistsExpanded NOTIFY pinnedPlaylistsExpandedChanged FINAL)
    Q_PROPERTY(bool allPlaylistsExpanded READ allPlaylistsExpanded WRITE setAllPlaylistsExpanded NOTIFY allPlaylistsExpandedChanged FINAL)
    Q_PROPERTY(QString immersivePlayerPanel READ immersivePlayerPanel WRITE setImmersivePlayerPanel NOTIFY immersivePlayerPanelChanged FINAL)
    Q_PROPERTY(QString sleepTimerFade READ sleepTimerFade WRITE setSleepTimerFade NOTIFY sleepTimerFadeChanged FINAL)
    Q_PROPERTY(bool reduceMotion READ reduceMotion WRITE setReduceMotion NOTIFY reduceMotionChanged FINAL)
    Q_PROPERTY(bool performanceMode READ performanceMode WRITE setPerformanceMode NOTIFY performanceModeChanged FINAL)
    /// no shapes behind the pages or the immersive player, and every other
    /// shape a plain circle, but for the favourite songs' heart
    Q_PROPERTY(bool noShapes READ noShapes WRITE setNoShapes NOTIFY noShapesChanged FINAL)
    /// "system" (a Matugen scheme), "builtin", or a theme id
    Q_PROPERTY(QString themeSource READ themeSource WRITE setThemeSource NOTIFY themeSourceChanged FINAL)
    /// "system", "light" or "dark"; selects a variant when Matugen provides both
    Q_PROPERTY(QString themeMode READ themeMode WRITE setThemeMode NOTIFY themeModeChanged FINAL)
    Q_PROPERTY(QString matugenColorsPath READ matugenColorsPath WRITE setMatugenColorsPath NOTIFY matugenColorsPathChanged FINAL)
    Q_PROPERTY(bool showWindowControls READ showWindowControls WRITE setShowWindowControls NOTIFY showWindowControlsChanged FINAL)
    Q_PROPERTY(bool nativeTitleBar READ nativeTitleBar WRITE setNativeTitleBar NOTIFY nativeTitleBarChanged FINAL)
    Q_PROPERTY(bool trayIcon READ trayIcon WRITE setTrayIcon NOTIFY trayIconChanged FINAL)
    /// look a song's lyrics up on LRCLIB and MusicBrainz, ahead of the
    /// lyrics kept beside or inside the file. sends the title, artist, album and
    /// length of what plays to those services.
    Q_PROPERTY(bool onlineLyrics READ onlineLyrics WRITE setOnlineLyrics NOTIFY onlineLyricsChanged FINAL)
    /// look an artist's picture up on Discogs (needs a token),
    /// ahead of a picture file kept in the artist's folder. sends the artist's name
    /// to those services.
    Q_PROPERTY(bool onlineArtistImages READ onlineArtistImages WRITE setOnlineArtistImages NOTIFY onlineArtistImagesChanged FINAL)
    /// a personal access token from discogs.com/settings/developers, which
    /// Discogs asks for with every search. empty: Discogs is not asked
    Q_PROPERTY(QString discogsToken READ discogsToken WRITE setDiscogsToken NOTIFY discogsTokenChanged FINAL)
    /// show what plays on the Discord profile, through the Discord app running
    /// on this computer (DiscordPresence)
    Q_PROPERTY(bool discordPresence READ discordPresence WRITE setDiscordPresence NOTIFY discordPresenceChanged FINAL)
    /// the Application ID of the Discord application (discord.com/developers)
    /// whose name Discord shows after "Listening to". empty: Swifty's own, below
    Q_PROPERTY(QString discordClientId READ discordClientId WRITE setDiscordClientId NOTIFY discordClientIdChanged FINAL)
    /// Swifty's own Discord application, used while discordClientId is empty. an
    /// Application ID is public (Discord shows it to anyone), not a secret
    Q_PROPERTY(QString discordDefaultClientId READ discordDefaultClientId CONSTANT FINAL)

public:
    /// the default file, ~/.config/Swifty/Swifty.ini. no default argument:
    /// qml builds a singleton with a default constructor in preference to
    /// create(), and would end up with an instance of its own
    explicit Settings(QObject *parent);
    /// a specific file, for tests
    explicit Settings(const QString &path, QObject *parent = nullptr);
    ~Settings() override;

    static Settings *instance();
    static Settings *create(QQmlEngine *, QJSEngine *);

    QVariant value(const QString &key, const QVariant &fallback = {}) const;
    void setValue(const QString &key, const QVariant &value);
    void remove(const QString &key);
    bool contains(const QString &key) const;
    void sync();

    /// every audio visualizer in the app
    bool visualizersEnabled() const { return m_visualizersEnabled; }
    void setVisualizersEnabled(bool value);
    /// ring around round artwork instead of a band of bars
    bool circularVisualizer() const { return m_circularVisualizer; }
    void setCircularVisualizer(bool value);
    bool roundArtwork() const { return m_roundArtwork; }
    void setRoundArtwork(bool value);
    /// bars or wave. stored under its first name, from when it applied to the ring alone
    QString visualizerShape() const { return m_visualizerShape; }
    void setVisualizerShape(const QString &value);
    double visualizerSensitivity() const { return m_visualizerSensitivity; }
    void setVisualizerSensitivity(double value);
    /// kbps: 96, 160 or 320
    /// animate the immersive background with the music
    bool livingBackdrop() const { return m_livingBackdrop; }
    void setLivingBackdrop(bool value);
    double backdropBlur() const { return m_backdropBlur; }
    void setBackdropBlur(double value);
    double backdropReactivity() const { return m_backdropReactivity; }
    void setBackdropReactivity(double value);
    /// adds a seconds field to the sleep timer
    bool debuggingEnabled() const { return m_debuggingEnabled; }
    void setDebuggingEnabled(bool value);
    bool pinnedPlaylistsExpanded() const { return m_pinnedPlaylistsExpanded; }
    void setPinnedPlaylistsExpanded(bool value);
    bool allPlaylistsExpanded() const { return m_allPlaylistsExpanded; }
    void setAllPlaylistsExpanded(bool value);
    QString immersivePlayerPanel() const { return m_immersivePlayerPanel; }
    void setImmersivePlayerPanel(const QString &value);
    QString sleepTimerFade() const { return m_sleepTimerFade; }
    void setSleepTimerFade(const QString &value);
    /// linux only. seeded from the desktop's animation setting on first launch
    bool reduceMotion() const { return m_reduceMotion; }
    void setReduceMotion(bool value);
    /// as little drawn as can be: nothing moves, fades or turns that does not
    /// have to, beyond reduced motion's fades
    bool performanceMode() const { return m_performanceMode; }
    void setPerformanceMode(bool value);
    bool noShapes() const { return m_noShapes; }
    void setNoShapes(bool value);
    /// system follows a discovered or explicitly selected Matugen scheme
    QString themeSource() const { return m_themeSource; }
    void setThemeSource(const QString &value);
    QString themeMode() const { return m_themeMode; }
    void setThemeMode(const QString &value);
    /// empty discovers Matugen JSON in common config/cache locations
    QString matugenColorsPath() const { return m_matugenColorsPath; }
    void setMatugenColorsPath(const QString &value);
    /// linux only. minimise, maximise and close in the top bar
    bool showWindowControls() const { return m_showWindowControls; }
    void setShowWindowControls(bool value);
    bool nativeTitleBar() const { return m_nativeTitleBar; }
    void setNativeTitleBar(bool value);
    /// linux only
    bool trayIcon() const { return m_trayIcon; }
    void setTrayIcon(bool value);
    bool onlineLyrics() const { return m_onlineLyrics; }
    void setOnlineLyrics(bool value);
    bool onlineArtistImages() const { return m_onlineArtistImages; }
    void setOnlineArtistImages(bool value);
    QString discogsToken() const { return m_discogsToken; }
    void setDiscogsToken(const QString &value);
    bool discordPresence() const { return m_discordPresence; }
    void setDiscordPresence(bool value);
    QString discordClientId() const { return m_discordClientId; }
    static QString discordDefaultClientId() { return QStringLiteral("1571430563569737879"); }
    /// the one that is used: the person's own if they gave one, else Swifty's
    QString effectiveDiscordClientId() const
    {
        return m_discordClientId.isEmpty() ? discordDefaultClientId() : m_discordClientId;
    }
    void setDiscordClientId(const QString &value);

signals:
    void visualizersEnabledChanged();
    void circularVisualizerChanged();
    void roundArtworkChanged();
    void visualizerShapeChanged();
    void visualizerSensitivityChanged();
    void livingBackdropChanged();
    void backdropBlurChanged();
    void backdropReactivityChanged();
    void debuggingEnabledChanged();
    void pinnedPlaylistsExpandedChanged();
    void allPlaylistsExpandedChanged();
    void immersivePlayerPanelChanged();
    void sleepTimerFadeChanged();
    void reduceMotionChanged();
    void performanceModeChanged();
    void noShapesChanged();
    void themeSourceChanged();
    void themeModeChanged();
    void matugenColorsPathChanged();
    void showWindowControlsChanged();
    void nativeTitleBarChanged();
    void trayIconChanged();
    void onlineLyricsChanged();
    void onlineArtistImagesChanged();
    void discogsTokenChanged();
    void discordPresenceChanged();
    void discordClientIdChanged();

private:
    void load();

    QSettings m_store;

    bool m_visualizersEnabled = true;
    bool m_circularVisualizer = false;
    bool m_roundArtwork = false;
    QString m_visualizerShape = QStringLiteral("bars");
    double m_visualizerSensitivity = 1.0;
    bool m_livingBackdrop = true;
    double m_backdropBlur = 16.0;
    double m_backdropReactivity = 0.3;
    bool m_debuggingEnabled = false;
    bool m_pinnedPlaylistsExpanded = false;
    bool m_allPlaylistsExpanded = false;
    QString m_immersivePlayerPanel = QStringLiteral("lyrics");
    QString m_sleepTimerFade = QStringLiteral("finalSeconds");
    bool m_reduceMotion = false;
    bool m_performanceMode = false;
    bool m_noShapes = false;
    QString m_themeSource = QStringLiteral("builtin");
    QString m_themeMode = QStringLiteral("system");
    QString m_matugenColorsPath = QString();
    bool m_showWindowControls = true;
#ifdef Q_OS_WIN
    bool m_nativeTitleBar = false;
#else
    bool m_nativeTitleBar = true;
#endif
    bool m_trayIcon = true;
    bool m_onlineLyrics = true;
    bool m_onlineArtistImages = true;
    QString m_discogsToken;
    bool m_discordPresence = true;
    QString m_discordClientId;
};

} // namespace swifty
