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

#include <QByteArray>
#include <QString>

#include <exception>
#include <utility>

namespace swifty {

/// a failure the user may be shown. the message is already user-facing.
class PlaybackError : public std::exception {
public:
    enum class Kind {
        InvalidData,
        /// the audio core reported the failure
        Core,
        /// the core gave up on a request because a newer one replaced it
        Superseded,
    };

    PlaybackError(Kind kind, QString message)
        : m_kind(kind)
        , m_message(std::move(message))
        , m_utf8(m_message.toUtf8())
    {
    }

    Kind kind() const { return m_kind; }
    const QString &message() const { return m_message; }
    const char *what() const noexcept override { return m_utf8.constData(); }

    static PlaybackError invalidData(const QString &detail)
    {
        return {Kind::InvalidData, QStringLiteral("Could not read the data: %1").arg(detail)};
    }

private:
    Kind m_kind;
    QString m_message;
    QByteArray m_utf8;
};

/// a request that was withdrawn: by a newer one, or navigation. never shown
/// to anyone.
struct Cancelled : std::exception {
    const char *what() const noexcept override { return "cancelled"; }
};

} // namespace swifty
