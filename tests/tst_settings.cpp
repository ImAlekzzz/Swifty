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

#include "app/Paths.h"
#include "app/Settings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

using swifty::Settings;

class SettingsTest : public QObject {
    Q_OBJECT

private slots:
    void defaultsMatchTheMacApp()
    {
        QTemporaryDir directory;
        Settings settings(directory.filePath(QStringLiteral("swifty.conf")));
        QCOMPARE(settings.visualizersEnabled(), true);
        QCOMPARE(settings.circularVisualizer(), false);
        QCOMPARE(settings.visualizerShape(), QStringLiteral("bars"));
        QCOMPARE(settings.visualizerSensitivity(), 1.0);
        QCOMPARE(settings.livingBackdrop(), true);
        QCOMPARE(settings.backdropBlur(), 16.0);
        QCOMPARE(settings.backdropReactivity(), 0.3);
        QCOMPARE(settings.immersivePlayerPanel(), QStringLiteral("lyrics"));
        QCOMPARE(settings.sleepTimerFade(), QStringLiteral("finalSeconds"));
        QCOMPARE(settings.themeSource(), QStringLiteral("builtin"));
        QCOMPARE(settings.themeMode(), QStringLiteral("system"));
#ifdef Q_OS_WIN
        QVERIFY(!settings.nativeTitleBar());
#else
        QVERIFY(settings.nativeTitleBar());
#endif
    }

    void savedThemeOverridesTheDefault_data()
    {
        QTest::addColumn<QString>("source");
        for (const char *source : {"system", "builtin", "spotify", "catppuccin-mocha"}) {
            QTest::newRow(source) << QString::fromLatin1(source);
        }
    }

    void savedThemeOverridesTheDefault()
    {
        QFETCH(QString, source);
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("swifty.conf"));
        {
            QSettings store(path, QSettings::IniFormat);
            store.setValue(QStringLiteral("themeSource"), source);
        }
        Settings settings(path);
        QCOMPARE(settings.themeSource(), source);
    }

    void titleBarPreferencePersists_data()
    {
        QTest::addColumn<bool>("nativeTitleBar");
        QTest::newRow("native") << true;
        QTest::newRow("in-app") << false;
    }

    void titleBarPreferencePersists()
    {
        QFETCH(bool, nativeTitleBar);
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("swifty.conf"));
        {
            Settings settings(path);
            settings.setNativeTitleBar(!nativeTitleBar);
            QSignalSpy changed(&settings, &Settings::nativeTitleBarChanged);
            settings.setNativeTitleBar(nativeTitleBar);
            settings.setNativeTitleBar(nativeTitleBar);
            QCOMPARE(changed.count(), 1);
        }
        Settings reopened(path);
        QCOMPARE(reopened.nativeTitleBar(), nativeTitleBar);
    }

    void valuesAreSanitized()
    {
        QTemporaryDir directory;
        Settings settings(directory.filePath(QStringLiteral("swifty.conf")));
        settings.setVisualizerSensitivity(4.0);
        QCOMPARE(settings.visualizerSensitivity(), 1.8);
        settings.setVisualizerShape(QStringLiteral("spiral"));
        QCOMPARE(settings.visualizerShape(), QStringLiteral("bars"));
        settings.setBackdropBlur(-3);
        QCOMPARE(settings.backdropBlur(), 0.0);
    }

    void changesNotifyOnceAndPersistUnderTheMacKeys()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("swifty.conf"));
        {
            Settings settings(path);
            QSignalSpy spy(&settings, &Settings::visualizerShapeChanged);
            settings.setVisualizerShape(QStringLiteral("wave"));
            settings.setVisualizerShape(QStringLiteral("wave"));
            QCOMPARE(spy.count(), 1);
            settings.setPerformanceMode(true);
            settings.setNoShapes(true);
        }
        QSettings raw(path, QSettings::IniFormat);
        QCOMPARE(raw.value(QStringLiteral("circularVisualizerShape")).toString(), QStringLiteral("wave"));

        Settings reopened(path);
        QCOMPARE(reopened.visualizerShape(), QStringLiteral("wave"));
        QVERIFY(reopened.performanceMode());
        QVERIFY(reopened.noShapes());
        QVERIFY(!reopened.reduceMotion());
    }
};

QTEST_GUILESS_MAIN(SettingsTest)
#include "tst_settings.moc"
