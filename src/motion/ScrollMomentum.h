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

#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QQuickItem>
#include <QTimer>
#include <QWindow>

class QWheelEvent;

namespace swifty {

/// a trackpad swipe glides on once the fingers lift, as it does on macos.
/// macos sends the glide itself, as wheel events in the momentum phase;
/// wayland compositors (libinput) stop at the lift and leave the glide to the
/// app, so a list stopped dead under the fingers. installed on the application,
/// this watches a window's trackpad scrolls and, after one ends, keeps the
/// view the swipe was moving going at the swipe's speed, slowing, until it
/// rests, hits its end or a new touch stops it.
/// the view is moved directly: qt's flickable, handed wheel events after the
/// lift, measures them from where the swipe began and jumps back there
class ScrollMomentum : public QObject {
    Q_OBJECT

public:
    explicit ScrollMomentum(QObject *parent = nullptr);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct Sample {
        quint64 timestamp;
        QPointF delta;
    };

    void track(QWindow *window, const QWheelEvent *event);
    void pickViews(QWindow *window, const QPointF &position);
    void start(const QWheelEvent *end);
    void glide();
    void stop();

    QList<Sample> m_samples;
    QTimer m_timer;
    QElapsedTimer m_clock;
    qint64 m_lastTick = 0;
    QPointer<QWindow> m_window;
    // the views a swipe was over that can scroll each way, and where each
    // was when it began, to tell which way the swipe moved it
    QPointer<QQuickItem> m_viewX;
    QPointer<QQuickItem> m_viewY;
    QPointF m_startPosition;
    QPointF m_direction {1, 1};
    QPointF m_velocity;
    QPointF m_remainder;
    qreal m_gain = 1.0;
    bool m_gliding = false;
    // a swipe has begun and its views are not yet picked
    bool m_pick = false;
};

} // namespace swifty
