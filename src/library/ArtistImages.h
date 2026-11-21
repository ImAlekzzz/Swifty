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

#pragma once

#include <QCoroTask>
#include <QHash>
#include <QNetworkAccessManager>
#include <QObject>
#include <QStringList>
#include <QUrl>

#include <optional>

namespace swifty {

class Settings;

/// the picture of an artist, from the first of these that has one:
///   1. Discogs, which needs a personal token; with none, it is not asked
///   2. a file called artist, in any of the usual picture formats (jpg, png, webp, gif,
///      bmp, svg, ico, and more where Qt reads them), kept in the
///      folder with the artist's name
///   3. nothing, and the caller shows what it already had: a cover
///
/// pictures found online are kept on disk, and a miss is not asked about again
/// for two weeks. lookups run one at a time, a second and a bit apart, which
/// keeps within what Discogs allows.
class ArtistImages : public QObject {
    Q_OBJECT

public:
    explicit ArtistImages(Settings *settings, QObject *parent = nullptr, const QString &cacheDirectory = {});

    /// the folders the music is kept in, where an artist's own folder is looked for
    void setLibraryFolders(const QStringList &folders);

    /// the picture to show for `artist` now: the one found online if it has
    /// been, otherwise a picture file in the artist's folder, otherwise empty.
    /// `songFiles` are files of the artist's, whose folders are looked in too.
    /// starts a lookup online when there has not been one, and imageChanged
    /// says when it brings something.
    QUrl imageFor(const QString &artist, const QStringList &songFiles = {});

    /// a file named artist.<extension> in a folder named for the artist: among
    /// the folders of `songFiles` and their parents, and the folders directly
    /// inside `folders` or inside those
    static QString findLocalPicture(const QString &artist, const QStringList &songFiles, const QStringList &folders);

    // what Discogs answers, read, public so that tests can drive them
    /// the picture Discogs has of the artist asked for, or empty
    static QUrl discogsImage(const QByteArray &searchJson, const QString &name);
signals:
    /// a picture arrived for this artist
    void imageChanged(const QString &artist);

private:
    struct Lookup {
        bool networkTrouble = false;
        QUrl image;
        bool discogs = false;
    };

    bool onlineAllowed() const;
    void enqueue(const QString &artist);
    QCoro::Task<> work();
    static QCoro::Task<> pause(int milliseconds);
    QCoro::Task<Lookup> findOnline(QString artist);
    QCoro::Task<Lookup> findOnDiscogs(QString artist);
    QByteArray discogsAuthorization() const;
    /// the body of an answer; none when the server could not be reached or is overloaded.
    /// a `postBody` makes it a form post
    QCoro::Task<std::optional<QByteArray>> get(QUrl url, QByteArray authorization = {}, QByteArray postBody = {});
    QString localPicture(const QString &artist, const QStringList &songFiles);
    QString fileFor(const QString &key) const;
    void load();
    void save() const;

    Settings *m_settings;
    QString m_directory;
    QNetworkAccessManager m_network;
    QStringList m_folders;
    /// by flattened name: the file a picture found online is in
    QHash<QString, QString> m_found;
    /// by flattened name: when it was last found that nobody has one
    QHash<QString, qint64> m_missing;
    QHash<QString, QString> m_local;
    QStringList m_queue;
    QStringList m_asked;
    bool m_working = false;
    qint64 m_pausedUntil = 0;
};

} // namespace swifty
