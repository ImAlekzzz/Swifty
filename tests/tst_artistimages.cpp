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

#include "app/Settings.h"
#include "library/ArtistImages.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

using swifty::ArtistImages;
using swifty::Settings;

namespace {

void touch(const QString &path)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("x");
}

QByteArray json(const QJsonObject &object)
{
    return QJsonDocument(object).toJson();
}

} // namespace

class ArtistImagesTest : public QObject {
    Q_OBJECT

private slots:
    void aPictureInTheArtistsFolderIsFoundFromTheirSongs()
    {
        QTemporaryDir dir;
        const QString music = dir.path() + QStringLiteral("/Music");
        touch(music + QStringLiteral("/Daft Punk/Discovery/01 One More Time.mp3"));
        touch(music + QStringLiteral("/Daft Punk/artist.jpg"));
        const QString song = music + QStringLiteral("/Daft Punk/Discovery/01 One More Time.mp3");

        QCOMPARE(ArtistImages::findLocalPicture(QStringLiteral("Daft Punk"), {song}, {}), music + QStringLiteral("/Daft Punk/artist.jpg"));
        // and from the library's folders alone
        QCOMPARE(ArtistImages::findLocalPicture(QStringLiteral("Daft Punk"), {}, {music}), music + QStringLiteral("/Daft Punk/artist.jpg"));
    }

    void capitalsPunctuationAndExtensionsDoNotMatter()
    {
        QTemporaryDir dir;
        const QString music = dir.path();
        touch(music + QStringLiteral("/ac-dc/ARTIST.PNG"));
        touch(music + QStringLiteral("/Björk/Artist.webp"));
        touch(music + QStringLiteral("/Nirvana/artist.pdf"));  // not a picture
        touch(music + QStringLiteral("/Nirvana/artist.txt"));
        touch(music + QStringLiteral("/Queen/cover.jpg"));      // not called artist

        QCOMPARE(QFileInfo(ArtistImages::findLocalPicture(QStringLiteral("AC/DC"), {}, {music})).fileName(), QStringLiteral("ARTIST.PNG"));
        QVERIFY(!ArtistImages::findLocalPicture(QStringLiteral("Björk"), {}, {music}).isEmpty());
        QVERIFY(ArtistImages::findLocalPicture(QStringLiteral("Nirvana"), {}, {music}).isEmpty());
        QVERIFY(ArtistImages::findLocalPicture(QStringLiteral("Queen"), {}, {music}).isEmpty());
        for (const QString &extension : {QStringLiteral("jpeg"), QStringLiteral("jpe"), QStringLiteral("jfif"), QStringLiteral("gif"),
                                         QStringLiteral("bmp"), QStringLiteral("svg"), QStringLiteral("ico")}) {
            const QString folder = music + QStringLiteral("/Band ") + extension;
            touch(folder + QStringLiteral("/artist.") + extension);
            QVERIFY2(!ArtistImages::findLocalPicture(QStringLiteral("Band ") + extension, {}, {music}).isEmpty(), qPrintable(extension));
        }
    }

    void aFolderOneLevelDownIsFoundToo()
    {
        QTemporaryDir dir;
        touch(dir.path() + QStringLiteral("/Rock/Foo Fighters/artist.jpg"));
        QVERIFY(!ArtistImages::findLocalPicture(QStringLiteral("Foo Fighters"), {}, {dir.path()}).isEmpty());
        // an artist with no folder has none
        QVERIFY(ArtistImages::findLocalPicture(QStringLiteral("Nobody"), {}, {dir.path()}).isEmpty());
        QVERIFY(ArtistImages::findLocalPicture(QString(), {}, {dir.path()}).isEmpty());
    }

    void discogsGivesThePictureOfTheArtistAsked()
    {
        const auto result = [](const QString &title, const QString &image) {
            return QJsonObject {{QStringLiteral("title"), title}, {QStringLiteral("cover_image"), image}};
        };
        const QByteArray search = json({{QStringLiteral("results"),
                                         QJsonArray {result(QStringLiteral("Another Artist"), QStringLiteral("https://i.discogs.com/other.jpg")),
                                                     result(QStringLiteral("Nirvana (2)"), QStringLiteral("https://st.discogs.com/spacer.gif")),
                                                     result(QStringLiteral("Nirvana (2)"), QStringLiteral("https://i.discogs.com/nirvana.jpg"))}}});
        // the placeholder is skipped, the number that tells two artists apart is not part of the name
        QCOMPARE(ArtistImages::discogsImage(search, QStringLiteral("Nirvana")), QUrl(QStringLiteral("https://i.discogs.com/nirvana.jpg")));
        QVERIFY(!ArtistImages::discogsImage(search, QStringLiteral("Unknown")).isValid());
    }

    void aPictureKeptFromBeforeComesFirstThenTheArtistsFolderThenNothing()
    {
        QTemporaryDir dir;
        Settings settings(dir.filePath(QStringLiteral("swifty.conf")));
        settings.setOnlineArtistImages(false);  // nothing here may go to the internet
        const QString cache = dir.filePath(QStringLiteral("cache"));
        const QString music = dir.filePath(QStringLiteral("Music"));
        touch(music + QStringLiteral("/Local Band/artist.png"));

        // nothing known
        {
            ArtistImages images(&settings, nullptr, cache);
            images.setLibraryFolders({music});
            QCOMPARE(images.imageFor(QStringLiteral("Nobody")), QUrl());
            // the artist's folder
            QCOMPARE(images.imageFor(QStringLiteral("Local Band")), QUrl::fromLocalFile(music + QStringLiteral("/Local Band/artist.png")));
        }
        // a picture found online earlier beats the folder's
        const QString found = cache + QStringLiteral("/artist-images/found.jpg");
        touch(found);
        QFile index(cache + QStringLiteral("/artist-images/index.json"));
        QVERIFY(index.open(QIODevice::WriteOnly));
        index.write(json({{QStringLiteral("source"), QStringLiteral("discogs")}, {QStringLiteral("found"), QJsonObject {{QStringLiteral("localband"), found}}}}));
        index.close();
        ArtistImages images(&settings, nullptr, cache);
        images.setLibraryFolders({music});
        QCOMPARE(images.imageFor(QStringLiteral("Local Band")), QUrl::fromLocalFile(found));
        QCOMPARE(images.imageFor(QStringLiteral("LOCAL-BAND")), QUrl::fromLocalFile(found));
    }

    void picturesFromOtherServicesAreForgotten()
    {
        QTemporaryDir dir;
        Settings settings(dir.filePath(QStringLiteral("swifty.conf")));
        settings.setOnlineArtistImages(false);
        const QString cache = dir.filePath(QStringLiteral("cache"));
        const QString music = dir.filePath(QStringLiteral("Music"));
        // an index kept by a version that asked MusicBrainz and Wikimedia: no source in it
        const QString stale = cache + QStringLiteral("/artist-images/stale.jpg");
        touch(stale);
        QFile index(cache + QStringLiteral("/artist-images/index.json"));
        QVERIFY(index.open(QIODevice::WriteOnly));
        index.write(json({{QStringLiteral("found"), QJsonObject {{QStringLiteral("juicewrld"), stale}}},
                          {QStringLiteral("missing"), QJsonObject {{QStringLiteral("someone"), 1}}}}));
        index.close();

        ArtistImages images(&settings, nullptr, cache);
        images.setLibraryFolders({music});
        QCOMPARE(images.imageFor(QStringLiteral("Juice WRLD")), QUrl());
        QVERIFY(!QFileInfo::exists(stale));
    }
};

QTEST_GUILESS_MAIN(ArtistImagesTest)
#include "tst_artistimages.moc"
