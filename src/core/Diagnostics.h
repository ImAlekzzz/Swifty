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

#include <cstdio>

namespace swifty {

/// whether status lines about the sound, the devices and the media controls
/// are wanted: start swifty with SWIFTY_DEBUG=1 (any value but 0 or empty).
/// they go to the terminal itself, past Qt's logging rules, which some
/// desktops turn off. what has actually failed is always told, without this
inline bool diagnosticsWanted()
{
    static const bool wanted = [] {
        const QByteArray value = qgetenv("SWIFTY_DEBUG");
        return !value.isEmpty() && value != "0";
    }();
    return wanted;
}

/// a status line, when wanted
inline void diagnostic(const char *area, const QString &text)
{
    if (!diagnosticsWanted()) {
        return;
    }
    std::fprintf(stderr, "swifty %s: %s\n", area, qPrintable(text));
    std::fflush(stderr);
}

} // namespace swifty
