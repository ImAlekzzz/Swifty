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

#include "library/ArtistImages.h"

#include "app/Paths.h"
#include "app/Settings.h"
#include "core/Async.h"

#include <QCoroNetworkReply>
#include <QCoroTimer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>
#include <QUrlQuery>

#include <QImageReader>
#include <algorithm>

namespace swifty {

Q_LOGGING_CATEGORY(lcArtistImages, "swifty.artistimages")

namespace {

constexpr qint64 kRetryMissAfterSeconds = 14 * 24 * 60 * 60;
constexpr qint64 kPauseAfterTroubleSeconds = 10 * 60;
constexpr int kTimeoutMs = 10000;
constexpr int kSpacingMs = 1100;
constexpr int kPictureSize = 800;
/// the endings of a picture file. the common ones always count; the rest only
/// where this Qt has a plugin that reads them, or the picture would not show
const QStringList &pictureExtensions()
{
    static const QStringList extensions = [] {
        QStringList list {QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("jpe"), QStringLiteral("jfif"),
                          QStringLiteral("png"), QStringLiteral("gif"), QStringLiteral("bmp"), QStringLiteral("webp"),
                          QStringLiteral("svg"), QStringLiteral("ico")};
        const QList<QByteArray> readable = QImageReader::supportedImageFormats();
        for (const QString &extension : {QStringLiteral("tif"), QStringLiteral("tiff"), QStringLiteral("avif"), QStringLiteral("jxl"),
                                         QStringLiteral("heic"), QStringLiteral("heif"), QStringLiteral("jp2")}) {
            const QByteArray format = extension.toLatin1();
            // tif is what a tiff plugin calls itself, and heif what heic is read as
            if (readable.contains(format) || (extension == QLatin1StringView("tif") && readable.contains("tiff"))
                || (extension == QLatin1StringView("heic") && readable.contains("heif"))) {
                list.append(extension);
            }
        }
        return list;
    }();
    return extensions;
}

/// letters and digits only, lowercase: "AC/DC" and "ac-dc" are the same artist
QString flattened(const QString &text)
{
    QString result;
    for (const QChar c : text) {
        if (c.isLetterOrNumber()) {
            result.append(c.toLower());
        }
    }
    return result;
}

QString userAgent()
{
    return QStringLiteral("Swifty/%1 ( https://github.com/ImAlekzzz/swifty )").arg(QStringLiteral(SWIFTY_VERSION));
}

/// a picture file called artist.<ext> in `directory`, whatever the capitals
QString pictureIn(const QString &directory)
{
    const QDir dir(directory);
    for (const QFileInfo &info : dir.entryInfoList(QDir::Files | QDir::Readable)) {
        if (info.completeBaseName().compare(QStringLiteral("artist"), Qt::CaseInsensitive) == 0
            && pictureExtensions().contains(info.suffix().toLower())) {
            return info.absoluteFilePath();
        }
    }
    return {};
}

} // namespace

ArtistImages::ArtistImages(Settings *settings, QObject *parent, const QString &cacheDirectory)
    : QObject(parent)
    , m_settings(settings)
    , m_directory((cacheDirectory.isEmpty() ? paths::cacheDirectory() : cacheDirectory) + QStringLiteral("/artist-images"))
{
    load();
    if (m_settings) {
        // a new token may be one that works, where the last one did not
        const auto askAgain = [this] {
            m_missing.clear();
            m_asked.clear();
            save();
        };
        connect(m_settings, &Settings::discogsTokenChanged, this, askAgain);
    }
}

bool ArtistImages::onlineAllowed() const
{
    // Discogs refuses every search that has no token
    return m_settings && m_settings->onlineArtistImages() && !m_settings->discogsToken().isEmpty();
}

void ArtistImages::setLibraryFolders(const QStringList &folders)
{
    if (m_folders == folders) {
        return;
    }
    m_folders = folders;
    // what was not found may be there now
    m_local.clear();
}

// --- local pictures --------------------------------------------------------------------------

QString ArtistImages::findLocalPicture(const QString &artist, const QStringList &songFiles, const QStringList &folders)
{
    const QString wanted = flattened(artist);
    if (wanted.isEmpty()) {
        return {};
    }
    const auto named = [&wanted](const QString &directory) { return flattened(QFileInfo(directory).fileName()) == wanted; };

    // the artist's folder is usually one of the folders a song is in
    QStringList seen;
    for (const QString &file : songFiles) {
        QDir dir = QFileInfo(file).dir();
        for (int level = 0; level < 4 && !dir.isRoot(); ++level) {
            const QString path = dir.absolutePath();
            if (named(path) && !seen.contains(path)) {
                seen.append(path);
                if (const QString found = pictureIn(path); !found.isEmpty()) {
                    return found;
                }
            }
            if (!dir.cdUp()) {
                break;
            }
        }
    }
    // or sits in the library: Music/Artist, or Music/Genre/Artist
    for (const QString &folder : folders) {
        const QDir root(folder);
        for (const QFileInfo &first : root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Readable)) {
            const QString firstPath = first.absoluteFilePath();
            if (named(firstPath) && !seen.contains(firstPath)) {
                seen.append(firstPath);
                if (const QString found = pictureIn(firstPath); !found.isEmpty()) {
                    return found;
                }
            }
            for (const QFileInfo &second : QDir(firstPath).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Readable)) {
                const QString secondPath = second.absoluteFilePath();
                if (named(secondPath) && !seen.contains(secondPath)) {
                    seen.append(secondPath);
                    if (const QString found = pictureIn(secondPath); !found.isEmpty()) {
                        return found;
                    }
                }
            }
        }
    }
    return {};
}

QString ArtistImages::localPicture(const QString &artist, const QStringList &songFiles)
{
    const QString key = flattened(artist);
    const auto known = m_local.constFind(key);
    if (known != m_local.cend()) {
        return *known;
    }
    const QString found = findLocalPicture(artist, songFiles, m_folders);
    m_local.insert(key, found);
    return found;
}

// --- what the services say -----------------------------------------------------------------------

QUrl ArtistImages::discogsImage(const QByteArray &searchJson, const QString &name)
{
    const QString wanted = flattened(name);
    for (const QJsonValue &value : QJsonDocument::fromJson(searchJson).object().value(QStringLiteral("results")).toArray()) {
        const QJsonObject result = value.toObject();
        // Discogs tells two artists of one name apart as "Name (2)"
        QString title = result.value(QStringLiteral("title")).toString();
        title.remove(QRegularExpression(QStringLiteral("\\s*\\(\\d+\\)$")));
        if (flattened(title) != wanted) {
            continue;
        }
        const QString image = result.value(QStringLiteral("cover_image")).toString();
        // an artist with no picture gets a blank placeholder
        if (!image.isEmpty() && !image.contains(QLatin1StringView("spacer.gif"))) {
            return QUrl(image);
        }
    }
    return {};
}

QCoro::Task<> ArtistImages::pause(int milliseconds)
{
    QTimer timer;
    timer.setSingleShot(true);
    timer.start(milliseconds);
    co_await qCoro(timer).waitForTimeout();
}

// --- the network ------------------------------------------------------------------------------------

QCoro::Task<std::optional<QByteArray>> ArtistImages::get(QUrl url, QByteArray authorization, QByteArray postBody)
{
    QNetworkRequest request(url);
    request.setTransferTimeout(kTimeoutMs);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setHeader(QNetworkRequest::UserAgentHeader, userAgent());
    if (!authorization.isEmpty()) {
        request.setRawHeader("Authorization", authorization);
    }
    QNetworkReply *reply = nullptr;
    if (postBody.isEmpty()) {
        reply = m_network.get(request);
    } else {
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
        reply = m_network.post(request, postBody);
    }
    co_await reply;
    const QVariant status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    std::optional<QByteArray> body;
    if (!status.isValid()) {
        qCDebug(lcArtistImages).noquote() << url.host() << ':' << reply->errorString();
    } else if (status.toInt() >= 200 && status.toInt() < 300) {
        body = reply->readAll();
    } else if (status.toInt() == 429 || status.toInt() >= 500) {
        qCDebug(lcArtistImages).noquote() << url.host() << "answered" << status.toInt();
    } else {
        // answered, with nothing for us
        body = QByteArray();
    }
    reply->deleteLater();
    co_return body;
}

QByteArray ArtistImages::discogsAuthorization() const
{
    return QByteArrayLiteral("Discogs token=") + m_settings->discogsToken().toUtf8();
}

QCoro::Task<ArtistImages::Lookup> ArtistImages::findOnDiscogs(QString artist)
{
    QUrl url(QStringLiteral("https://api.discogs.com/database/search"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("q"), artist);
    query.addQueryItem(QStringLiteral("type"), QStringLiteral("artist"));
    query.addQueryItem(QStringLiteral("per_page"), QStringLiteral("5"));
    url.setQuery(query);
    const std::optional<QByteArray> body = co_await get(url, discogsAuthorization());
    if (!body) {
        co_return Lookup {true, {}, true};
    }
    co_return Lookup {false, discogsImage(*body, artist), true};
}

QCoro::Task<ArtistImages::Lookup> ArtistImages::findOnline(QString artist)
{
    co_return co_await findOnDiscogs(artist);
}

// --- the queue -----------------------------------------------------------------------------------------

void ArtistImages::enqueue(const QString &artist)
{
    const QString key = flattened(artist);
    if (key.isEmpty() || m_found.contains(key) || m_asked.contains(key)) {
        return;
    }
    const auto missed = m_missing.constFind(key);
    if (missed != m_missing.cend() && QDateTime::currentSecsSinceEpoch() - *missed < kRetryMissAfterSeconds) {
        return;
    }
    m_asked.append(key);
    m_queue.append(artist);
    detach(work());
}

QCoro::Task<> ArtistImages::work()
{
    if (m_working) {
        co_return;
    }
    m_working = true;
    QDir().mkpath(m_directory);
    while (!m_queue.isEmpty() && onlineAllowed()) {
        if (QDateTime::currentSecsSinceEpoch() < m_pausedUntil) {
            break;
        }
        const QString artist = m_queue.takeFirst();
        const QString key = flattened(artist);
        const Lookup lookup = co_await findOnline(artist);
        if (lookup.networkTrouble) {
            // cannot reach it, or it is overloaded: leave off for a while
            // instead of asking for every artist in turn, and ask again later
            m_pausedUntil = QDateTime::currentSecsSinceEpoch() + kPauseAfterTroubleSeconds;
            m_asked.removeAll(key);
            break;
        }
        bool stored = false;
        if (lookup.image.isValid()) {
            const std::optional<QByteArray> bytes = co_await get(lookup.image, lookup.discogs ? discogsAuthorization() : QByteArray());
            if (bytes && !bytes->isEmpty()) {
                QImage image = QImage::fromData(*bytes);
                if (!image.isNull()) {
                    if (image.width() > kPictureSize || image.height() > kPictureSize) {
                        image = image.scaled(kPictureSize, kPictureSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                    }
                    const QString path = fileFor(key);
                    stored = image.save(path, "JPEG", 88) || image.save(path, "PNG");
                }
            }
        }
        if (stored) {
            m_found.insert(key, fileFor(key));
            m_missing.remove(key);
            save();
            emit imageChanged(artist);
        } else {
            m_missing.insert(key, QDateTime::currentSecsSinceEpoch());
            save();
        }
        co_await pause(kSpacingMs);
    }
    m_working = false;
}

// --- what is kept ------------------------------------------------------------------------------------------

QString ArtistImages::fileFor(const QString &key) const
{
    const QByteArray digest = QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex();
    return m_directory + QLatin1Char('/') + QString::fromLatin1(digest.left(20)) + QStringLiteral(".jpg");
}

void ArtistImages::load()
{
    QFile file(m_directory + QStringLiteral("/index.json"));
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    const QJsonObject found = root.value(QStringLiteral("found")).toObject();
    file.close();
    if (root.value(QStringLiteral("source")).toString() != QLatin1StringView("discogs")) {
        // kept by a version that asked other services as well: those pictures, and
        // the artists it found nothing for, are not to be trusted now that only
        // Discogs is asked. they are forgotten, and asked for again
        for (auto it = found.begin(); it != found.end(); ++it) {
            const QString path = it.value().toString();
            if (path.startsWith(m_directory + QLatin1Char('/'))) {
                QFile::remove(path);
            }
        }
        save();
        return;
    }
    for (auto it = found.begin(); it != found.end(); ++it) {
        const QString path = it.value().toString();
        if (QFileInfo::exists(path)) {
            m_found.insert(it.key(), path);
        }
    }
    const QJsonObject missing = root.value(QStringLiteral("missing")).toObject();
    for (auto it = missing.begin(); it != missing.end(); ++it) {
        m_missing.insert(it.key(), it.value().toInteger());
    }
}

void ArtistImages::save() const
{
    QJsonObject found;
    for (auto it = m_found.begin(); it != m_found.end(); ++it) {
        found.insert(it.key(), it.value());
    }
    QJsonObject missing;
    for (auto it = m_missing.begin(); it != m_missing.end(); ++it) {
        missing.insert(it.key(), it.value());
    }
    QDir().mkpath(m_directory);
    QFile file(m_directory + QStringLiteral("/index.json"));
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(QJsonDocument(QJsonObject {{QStringLiteral("source"), QStringLiteral("discogs")}, {QStringLiteral("found"), found}, {QStringLiteral("missing"), missing}}).toJson(QJsonDocument::Compact));
    }
}

// --- the answer -----------------------------------------------------------------------------------------------

QUrl ArtistImages::imageFor(const QString &artist, const QStringList &songFiles)
{
    const QString key = flattened(artist);
    if (key.isEmpty()) {
        return {};
    }
    if (const auto found = m_found.constFind(key); found != m_found.cend() && QFileInfo::exists(*found)) {
        return QUrl::fromLocalFile(*found);
    }
    // until the internet has something, or when it has not: the artist's folder
    const QString local = localPicture(artist, songFiles);
    if (onlineAllowed()) {
        enqueue(artist);
    }
    return local.isEmpty() ? QUrl() : QUrl::fromLocalFile(local);
}

} // namespace swifty
