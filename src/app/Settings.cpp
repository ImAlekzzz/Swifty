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

#include "app/Settings.h"

#include <QQmlEngine>

#include <algorithm>
#include <initializer_list>

namespace swifty {

namespace {

Settings *s_instance = nullptr;

QString oneOf(const QString &value, std::initializer_list<QString> allowed, const QString &fallback)
{
    return std::find(allowed.begin(), allowed.end(), value) != allowed.end() ? value : fallback;
}

template <typename T>
T stored(const QSettings &store, const char *key, const T &fallback)
{
    const QVariant value = store.value(QLatin1StringView(key));
    return value.isValid() && value.canConvert<T>() ? value.value<T>() : fallback;
}

} // namespace

Settings::Settings(QObject *parent)
    : QObject(parent)
    , m_store(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("Swifty"),
              QStringLiteral("Swifty"))
{
    load();
    if (!s_instance) {
        s_instance = this;
    }
}

Settings::Settings(const QString &path, QObject *parent)
    : QObject(parent)
    , m_store(path, QSettings::IniFormat)
{
    load();
    // a test's settings are the ones its views read, never the real file
    if (!s_instance) {
        s_instance = this;
    }
}

Settings::~Settings()
{
    m_store.sync();
    if (s_instance == this) {
        s_instance = nullptr;
    }
}

Settings *Settings::instance()
{
    return s_instance;
}

Settings *Settings::create(QQmlEngine *, QJSEngine *)
{
    QJSEngine::setObjectOwnership(s_instance, QJSEngine::CppOwnership);
    return s_instance;
}

QVariant Settings::value(const QString &key, const QVariant &fallback) const
{
    return m_store.value(key, fallback);
}

void Settings::setValue(const QString &key, const QVariant &value)
{
    m_store.setValue(key, value);
}

void Settings::remove(const QString &key)
{
    m_store.remove(key);
}

bool Settings::contains(const QString &key) const
{
    return m_store.contains(key);
}

void Settings::sync()
{
    m_store.sync();
}

void Settings::load()
{
    m_visualizersEnabled = stored<bool>(m_store, "visualizersEnabled", m_visualizersEnabled);
    m_circularVisualizer = stored<bool>(m_store, "circularVisualizer", m_circularVisualizer);
    m_roundArtwork = stored<bool>(m_store, "roundArtwork", m_roundArtwork);
    {
        const QString raw = stored<QString>(m_store, "circularVisualizerShape", m_visualizerShape);
        m_visualizerShape = oneOf(raw, {QStringLiteral("bars"), QStringLiteral("wave")}, QStringLiteral("bars"));
    }
    {
        const double raw = stored<double>(m_store, "visualizerSensitivity", m_visualizerSensitivity);
        m_visualizerSensitivity = std::clamp(raw, 0.6, 1.8);
    }
    m_livingBackdrop = stored<bool>(m_store, "livingBackdrop", m_livingBackdrop);
    {
        const double raw = stored<double>(m_store, "backdropBlur", m_backdropBlur);
        m_backdropBlur = std::clamp(raw, 0.0, 60.0);
    }
    {
        const double raw = stored<double>(m_store, "backdropReactivity", m_backdropReactivity);
        m_backdropReactivity = std::clamp(raw, 0.0, 3.0);
    }
    m_debuggingEnabled = stored<bool>(m_store, "debuggingEnabled", m_debuggingEnabled);
    m_pinnedPlaylistsExpanded = stored<bool>(m_store, "pinnedPlaylistsExpanded", m_pinnedPlaylistsExpanded);
    m_allPlaylistsExpanded = stored<bool>(m_store, "allPlaylistsExpanded", m_allPlaylistsExpanded);
    {
        const QString raw = stored<QString>(m_store, "immersivePlayerPanel", m_immersivePlayerPanel);
        m_immersivePlayerPanel = oneOf(raw, {QStringLiteral("artwork"), QStringLiteral("lyrics"), QStringLiteral("queue"), QStringLiteral("sleep"), QStringLiteral("audio")}, QStringLiteral("lyrics"));
    }
    {
        const QString raw = stored<QString>(m_store, "sleepTimerFade", m_sleepTimerFade);
        m_sleepTimerFade = oneOf(raw, {QStringLiteral("none"), QStringLiteral("finalSeconds"), QStringLiteral("entire")}, QStringLiteral("finalSeconds"));
    }
    m_reduceMotion = stored<bool>(m_store, "reduceMotion", m_reduceMotion);
    m_performanceMode = stored<bool>(m_store, "performanceMode", m_performanceMode);
    m_noShapes = stored<bool>(m_store, "noShapes", m_noShapes);
    {
        // an unknown theme falls back to the app's own colours in Theme
        const QString raw = stored<QString>(m_store, "themeSource", m_themeSource).trimmed();
        if (!raw.isEmpty()) {
            m_themeSource = raw;
        }
    }
    {
        const QString raw = stored<QString>(m_store, "themeMode", m_themeMode);
        m_themeMode = oneOf(raw, {QStringLiteral("system"), QStringLiteral("light"), QStringLiteral("dark")}, QStringLiteral("system"));
    }
    {
        const QString raw = stored<QString>(m_store, "matugenColorsPath", m_matugenColorsPath);
        m_matugenColorsPath = raw.trimmed();
    }
    m_showWindowControls = stored<bool>(m_store, "showWindowControls", m_showWindowControls);
    m_nativeTitleBar = stored<bool>(m_store, "nativeTitleBar", m_nativeTitleBar);
    m_trayIcon = stored<bool>(m_store, "trayIcon", m_trayIcon);
    m_onlineLyrics = stored<bool>(m_store, "onlineLyrics", m_onlineLyrics);
    m_onlineArtistImages = stored<bool>(m_store, "onlineArtistImages", m_onlineArtistImages);
    m_discogsToken = stored<QString>(m_store, "discogsToken", m_discogsToken).trimmed();
    m_discordPresence = stored<bool>(m_store, "discordPresence", m_discordPresence);
    m_discordClientId = stored<QString>(m_store, "discordClientId", m_discordClientId).trimmed();
}

void Settings::setVisualizersEnabled(bool value)
{
    if (m_visualizersEnabled == value) {
        return;
    }
    m_visualizersEnabled = value;
    m_store.setValue(QStringLiteral("visualizersEnabled"), value);
    emit visualizersEnabledChanged();
}

void Settings::setCircularVisualizer(bool value)
{
    if (m_circularVisualizer == value) {
        return;
    }
    m_circularVisualizer = value;
    m_store.setValue(QStringLiteral("circularVisualizer"), value);
    emit circularVisualizerChanged();
}

void Settings::setRoundArtwork(bool value)
{
    if (m_roundArtwork == value) {
        return;
    }
    m_roundArtwork = value;
    m_store.setValue(QStringLiteral("roundArtwork"), value);
    emit roundArtworkChanged();
}

void Settings::setVisualizerShape(const QString &raw)
{
    const QString value = oneOf(raw, {QStringLiteral("bars"), QStringLiteral("wave")}, QStringLiteral("bars"));
    if (m_visualizerShape == value) {
        return;
    }
    m_visualizerShape = value;
    m_store.setValue(QStringLiteral("circularVisualizerShape"), value);
    emit visualizerShapeChanged();
}

void Settings::setVisualizerSensitivity(double raw)
{
    const double value = std::clamp(raw, 0.6, 1.8);
    if (m_visualizerSensitivity == value) {
        return;
    }
    m_visualizerSensitivity = value;
    m_store.setValue(QStringLiteral("visualizerSensitivity"), value);
    emit visualizerSensitivityChanged();
}

void Settings::setLivingBackdrop(bool value)
{
    if (m_livingBackdrop == value) {
        return;
    }
    m_livingBackdrop = value;
    m_store.setValue(QStringLiteral("livingBackdrop"), value);
    emit livingBackdropChanged();
}

void Settings::setBackdropBlur(double raw)
{
    const double value = std::clamp(raw, 0.0, 60.0);
    if (m_backdropBlur == value) {
        return;
    }
    m_backdropBlur = value;
    m_store.setValue(QStringLiteral("backdropBlur"), value);
    emit backdropBlurChanged();
}

void Settings::setBackdropReactivity(double raw)
{
    const double value = std::clamp(raw, 0.0, 3.0);
    if (m_backdropReactivity == value) {
        return;
    }
    m_backdropReactivity = value;
    m_store.setValue(QStringLiteral("backdropReactivity"), value);
    emit backdropReactivityChanged();
}

void Settings::setDebuggingEnabled(bool value)
{
    if (m_debuggingEnabled == value) {
        return;
    }
    m_debuggingEnabled = value;
    m_store.setValue(QStringLiteral("debuggingEnabled"), value);
    emit debuggingEnabledChanged();
}

void Settings::setPinnedPlaylistsExpanded(bool value)
{
    if (m_pinnedPlaylistsExpanded == value) {
        return;
    }
    m_pinnedPlaylistsExpanded = value;
    m_store.setValue(QStringLiteral("pinnedPlaylistsExpanded"), value);
    emit pinnedPlaylistsExpandedChanged();
}

void Settings::setAllPlaylistsExpanded(bool value)
{
    if (m_allPlaylistsExpanded == value) {
        return;
    }
    m_allPlaylistsExpanded = value;
    m_store.setValue(QStringLiteral("allPlaylistsExpanded"), value);
    emit allPlaylistsExpandedChanged();
}

void Settings::setImmersivePlayerPanel(const QString &raw)
{
    const QString value = oneOf(raw, {QStringLiteral("artwork"), QStringLiteral("lyrics"), QStringLiteral("queue"), QStringLiteral("sleep"), QStringLiteral("audio")}, QStringLiteral("lyrics"));
    if (m_immersivePlayerPanel == value) {
        return;
    }
    m_immersivePlayerPanel = value;
    m_store.setValue(QStringLiteral("immersivePlayerPanel"), value);
    emit immersivePlayerPanelChanged();
}

void Settings::setSleepTimerFade(const QString &raw)
{
    const QString value = oneOf(raw, {QStringLiteral("none"), QStringLiteral("finalSeconds"), QStringLiteral("entire")}, QStringLiteral("finalSeconds"));
    if (m_sleepTimerFade == value) {
        return;
    }
    m_sleepTimerFade = value;
    m_store.setValue(QStringLiteral("sleepTimerFade"), value);
    emit sleepTimerFadeChanged();
}

void Settings::setReduceMotion(bool value)
{
    if (m_reduceMotion == value) {
        return;
    }
    m_reduceMotion = value;
    m_store.setValue(QStringLiteral("reduceMotion"), value);
    emit reduceMotionChanged();
}

void Settings::setPerformanceMode(bool value)
{
    if (m_performanceMode == value) {
        return;
    }
    m_performanceMode = value;
    m_store.setValue(QStringLiteral("performanceMode"), value);
    emit performanceModeChanged();
}

void Settings::setNoShapes(bool value)
{
    if (m_noShapes == value) {
        return;
    }
    m_noShapes = value;
    m_store.setValue(QStringLiteral("noShapes"), value);
    emit noShapesChanged();
}

void Settings::setThemeSource(const QString &raw)
{
    const QString value = raw.trimmed();
    if (value.isEmpty() || m_themeSource == value) {
        return;
    }
    m_themeSource = value;
    m_store.setValue(QStringLiteral("themeSource"), value);
    emit themeSourceChanged();
}

void Settings::setThemeMode(const QString &raw)
{
    const QString value = oneOf(raw, {QStringLiteral("system"), QStringLiteral("light"), QStringLiteral("dark")}, QStringLiteral("system"));
    if (m_themeMode == value) {
        return;
    }
    m_themeMode = value;
    m_store.setValue(QStringLiteral("themeMode"), value);
    emit themeModeChanged();
}

void Settings::setMatugenColorsPath(const QString &raw)
{
    const QString value = raw.trimmed();
    if (m_matugenColorsPath == value) {
        return;
    }
    m_matugenColorsPath = value;
    m_store.setValue(QStringLiteral("matugenColorsPath"), value);
    emit matugenColorsPathChanged();
}

void Settings::setShowWindowControls(bool value)
{
    if (m_showWindowControls == value) {
        return;
    }
    m_showWindowControls = value;
    m_store.setValue(QStringLiteral("showWindowControls"), value);
    emit showWindowControlsChanged();
}

void Settings::setNativeTitleBar(bool value)
{
    if (m_nativeTitleBar == value) {
        return;
    }
    m_nativeTitleBar = value;
    m_store.setValue(QStringLiteral("nativeTitleBar"), value);
    emit nativeTitleBarChanged();
}

void Settings::setOnlineArtistImages(bool value)
{
    if (m_onlineArtistImages == value) {
        return;
    }
    m_onlineArtistImages = value;
    m_store.setValue(QStringLiteral("onlineArtistImages"), value);
    emit onlineArtistImagesChanged();
}

void Settings::setDiscogsToken(const QString &raw)
{
    const QString value = raw.trimmed();
    if (m_discogsToken == value) {
        return;
    }
    m_discogsToken = value;
    m_store.setValue(QStringLiteral("discogsToken"), value);
    emit discogsTokenChanged();
}

void Settings::setDiscordPresence(bool value)
{
    if (m_discordPresence == value) {
        return;
    }
    m_discordPresence = value;
    m_store.setValue(QStringLiteral("discordPresence"), value);
    emit discordPresenceChanged();
}

void Settings::setDiscordClientId(const QString &raw)
{
    const QString value = raw.trimmed();
    if (m_discordClientId == value) {
        return;
    }
    m_discordClientId = value;
    m_store.setValue(QStringLiteral("discordClientId"), value);
    emit discordClientIdChanged();
}

void Settings::setOnlineLyrics(bool value)
{
    if (m_onlineLyrics == value) {
        return;
    }
    m_onlineLyrics = value;
    m_store.setValue(QStringLiteral("onlineLyrics"), value);
    emit onlineLyricsChanged();
}

void Settings::setTrayIcon(bool value)
{
    if (m_trayIcon == value) {
        return;
    }
    m_trayIcon = value;
    m_store.setValue(QStringLiteral("trayIcon"), value);
    emit trayIconChanged();
}

} // namespace swifty
