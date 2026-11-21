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

#include "motion/ScrollMomentum.h"

#include <QCoreApplication>
#include <QQuickItem>
#include <QQuickWindow>
#include <QLoggingCategory>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <utility>

namespace swifty {

namespace {

Q_LOGGING_CATEGORY(lcScroll, "swifty.scroll", QtInfoMsg)

// the swipe's speed is read over its last moments: the fingers slow as they
// lift, and an average over the whole swipe would carry its start
constexpr quint64 kSampleWindowMs = 80;
// fingers that came to rest before lifting meant to stop there
constexpr quint64 kRestMs = 50;
// how quickly a glide slows: its speed falls by e every this long, near
// what macos and ios use. a swipe glides on about three times this far
constexpr qreal kDecayMs = 400.0;
// in pixels a millisecond: slower than this a swipe does not glide, and a
// glide this slow has come to rest
constexpr qreal kMinSpeed = 0.1;
constexpr qreal kRestSpeed = 0.02;
constexpr qreal kMaxSpeed = 12.0;
// a swipe's speed as libinput reports it is slow beside what macos sends, so
// the glide goes on at this many times it. SWIFTY_SCROLL_GAIN overrides it
constexpr qreal kGain = 5.0;
constexpr int kTickMs = 8;

qreal length(const QPointF &point)
{
    return std::hypot(point.x(), point.y());
}

bool isFlickable(const QQuickItem *item)
{
    return item->inherits("QQuickFlickable");
}

/// the flickables under a point, the deepest first, in the topmost branch of
/// the item tree that has any
QList<QQuickItem *> flickablesAt(QQuickItem *item, const QPointF &scenePosition)
{
    // topmost first: by z, and later siblings over earlier ones
    QList<QQuickItem *> children = item->childItems();
    std::reverse(children.begin(), children.end());
    std::stable_sort(children.begin(), children.end(), [](const QQuickItem *a, const QQuickItem *b) { return a->z() > b->z(); });
    for (QQuickItem *child : std::as_const(children)) {
        if (!child->isVisible() || child->opacity() <= 0.0 || !child->contains(child->mapFromScene(scenePosition))) {
            continue;
        }
        QList<QQuickItem *> found = flickablesAt(child, scenePosition);
        if (isFlickable(child)) {
            found.append(child);
        }
        if (!found.isEmpty()) {
            return found;
        }
    }
    return {};
}

struct Axis {
    const char *position;
    const char *origin;
    const char *content;
    const char *startMargin;
    const char *endMargin;
};

constexpr Axis kVertical {"contentY", "originY", "contentHeight", "topMargin", "bottomMargin"};
constexpr Axis kHorizontal {"contentX", "originX", "contentWidth", "leftMargin", "rightMargin"};

qreal size(const QQuickItem *view, bool vertical)
{
    return vertical ? view->height() : view->width();
}

/// the range a flickable's content position can be in
bool range(const QQuickItem *view, bool vertical, qreal *low, qreal *high)
{
    const Axis &axis = vertical ? kVertical : kHorizontal;
    if (!view->property("interactive").toBool()) {
        return false;
    }
    const qreal origin = view->property(axis.origin).toReal();
    const qreal content = view->property(axis.content).toReal();
    const qreal start = view->property(axis.startMargin).toReal();
    const qreal end = view->property(axis.endMargin).toReal();
    if (content + start + end <= size(view, vertical) + 1.0) {
        return false;
    }
    *low = origin - start;
    *high = qMax(*low, origin + content - size(view, vertical) + end);
    return true;
}

qreal contentPosition(const QQuickItem *view, bool vertical)
{
    return view->property((vertical ? kVertical : kHorizontal).position).toReal();
}

/// moves a flickable's content position by `delta`, to its limits at most,
/// and says whether it moved
bool moveBy(QQuickItem *view, bool vertical, qreal delta)
{
    qreal low = 0;
    qreal high = 0;
    if (!range(view, vertical, &low, &high)) {
        return false;
    }
    const qreal now = contentPosition(view, vertical);
    const qreal next = qBound(low, now + delta, high);
    if (qFuzzyCompare(1.0 + next, 1.0 + now)) {
        return false;
    }
    view->setProperty((vertical ? kVertical : kHorizontal).position, next);
    return true;
}

} // namespace

ScrollMomentum::ScrollMomentum(QObject *parent)
    : QObject(parent)
{
    bool ok = false;
    const qreal gain = qEnvironmentVariable("SWIFTY_SCROLL_GAIN").toDouble(&ok);
    m_gain = ok && gain > 0 ? gain : kGain;
    m_timer.setInterval(kTickMs);
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, &ScrollMomentum::glide);
}

bool ScrollMomentum::eventFilter(QObject *watched, QEvent *event)
{
    if (!watched->isWindowType()) {
        return false;
    }
    switch (event->type()) {
    case QEvent::Wheel: {
        auto *window = static_cast<QWindow *>(watched);
        const auto *wheel = static_cast<const QWheelEvent *>(event);
        qCDebug(lcScroll) << wheel->timestamp() << wheel->phase() << wheel->source() << wheel->pixelDelta()
                          << wheel->angleDelta() << "inverted" << wheel->inverted() << wheel->device()->name();
        // a new touch, or a mouse's wheel, takes over from a glide. an end
        // does not: the window can see one end twice (on hyprland), and the
        // second stopped the glide the first had just started
        if (wheel->phase() != Qt::ScrollEnd) {
            stop();
        }
        // a swipe begins with an event of no movement that is not marked as
        // from the system, and what it scrolls is told from its first update,
        // before the view has moved
        if (wheel->phase() == Qt::ScrollBegin) {
            m_samples.clear();
            m_pick = true;
        }
        // only a trackpad says when the fingers lift; a mouse's wheel has
        // no phases and qt already eases it
        if (wheel->source() != Qt::MouseEventSynthesizedBySystem) {
            if (wheel->phase() != Qt::ScrollBegin) {
                m_samples.clear();
            }
            break;
        }
        switch (wheel->phase()) {
        case Qt::ScrollUpdate:
            track(window, wheel);
            if (m_pick) {
                m_pick = false;
                pickViews(window, wheel->position());
            }
            break;
        case Qt::ScrollEnd:
            if (window == m_window) {
                start(wheel);
            }
            break;
        default:
            break;
        }
        break;
    }
    case QEvent::MouseButtonPress:
    case QEvent::TouchBegin:
    case QEvent::KeyPress:
        stop();
        break;
    case QEvent::Hide:
    case QEvent::Close:
        if (watched == m_window) {
            stop();
        }
        break;
    default:
        break;
    }
    return false;
}

void ScrollMomentum::track(QWindow *window, const QWheelEvent *event)
{
    if (window != m_window) {
        m_samples.clear();
        m_window = window;
    }
    if (!event->pixelDelta().isNull()) {
        m_samples.append({event->timestamp(), QPointF(event->pixelDelta())});
    }
    while (!m_samples.isEmpty() && m_samples.constLast().timestamp - m_samples.constFirst().timestamp > kSampleWindowMs) {
        m_samples.removeFirst();
    }
}

void ScrollMomentum::pickViews(QWindow *window, const QPointF &position)
{
    m_viewX = nullptr;
    m_viewY = nullptr;
    auto *quick = qobject_cast<QQuickWindow *>(window);
    if (!quick) {
        return;
    }
    qreal low = 0;
    qreal high = 0;
    for (QQuickItem *view : flickablesAt(quick->contentItem(), position)) {
        if (!m_viewY && range(view, true, &low, &high)) {
            m_viewY = view;
        }
        if (!m_viewX && range(view, false, &low, &high)) {
            m_viewX = view;
        }
    }
    m_startPosition = {m_viewX ? contentPosition(m_viewX, false) : 0.0, m_viewY ? contentPosition(m_viewY, true) : 0.0};
    qCDebug(lcScroll) << "swipe over" << m_viewX << m_viewY;
}

void ScrollMomentum::start(const QWheelEvent *end)
{
    const QList<Sample> samples = std::exchange(m_samples, {});
    qCDebug(lcScroll) << "lift after" << samples.size() << "samples, last"
                      << (samples.isEmpty() ? 0 : end->timestamp() - samples.constLast().timestamp) << "ms before";
    if (samples.size() < 2 || end->timestamp() - samples.constLast().timestamp > kRestMs || (!m_viewX && !m_viewY)) {
        return;
    }
    // the first sample's movement happened before the window began
    QPointF distance;
    for (qsizetype i = 1; i < samples.size(); ++i) {
        distance += samples.at(i).delta;
    }
    const qreal span = qMax<qreal>(samples.constLast().timestamp - samples.constFirst().timestamp, kTickMs);
    QPointF velocity = distance / span;
    const qreal speed = length(velocity);
    qCDebug(lcScroll) << "lift speed" << speed << "px/ms, gain" << m_gain;
    if (speed < kMinSpeed) {
        return;
    }
    velocity *= m_gain;
    if (length(velocity) > kMaxSpeed) {
        velocity *= kMaxSpeed / length(velocity);
    }
    // which way a swipe moved its view is told by where the view went, not
    // assumed: a swipe up moves the content up, unless the scrolling is natural
    // or inverted, and the events do not all say so alike
    m_direction = {1, 1};
    if (m_viewY) {
        const qreal moved = contentPosition(m_viewY, true) - m_startPosition.y();
        if (std::abs(moved) > 1.0 && std::abs(distance.y()) > 1.0) {
            m_direction.setY(moved * distance.y() < 0 ? 1 : -1);
        }
    }
    if (m_viewX) {
        const qreal moved = contentPosition(m_viewX, false) - m_startPosition.x();
        if (std::abs(moved) > 1.0 && std::abs(distance.x()) > 1.0) {
            m_direction.setX(moved * distance.x() < 0 ? 1 : -1);
        }
    }
    m_velocity = velocity;
    m_remainder = {};
    m_gliding = true;
    m_clock.start();
    m_lastTick = 0;
    m_timer.start();
}

void ScrollMomentum::glide()
{
    if (!m_window || (!m_viewX && !m_viewY)) {
        stop();
        return;
    }
    const qint64 now = m_clock.elapsed();
    const qreal elapsed = now - m_lastTick;
    if (elapsed <= 0) {
        return;
    }
    m_lastTick = now;
    // the distance a speed slowing by e every kDecayMs covers in `elapsed`
    const qreal decay = std::exp(-elapsed / kDecayMs);
    m_remainder += m_velocity * kDecayMs * (1.0 - decay);
    m_velocity *= decay;
    const QPoint step(qRound(m_remainder.x()), qRound(m_remainder.y()));
    m_remainder -= QPointF(step);
    // the content moves against the swipe's own sign, as the flickable's does
    if (step.y() != 0) {
        if (!m_viewY || !moveBy(m_viewY, true, -step.y() * m_direction.y())) {
            // at its end: a glide goes no further
            m_velocity.setY(0);
            m_remainder.setY(0);
        }
    }
    if (step.x() != 0) {
        if (!m_viewX || !moveBy(m_viewX, false, -step.x() * m_direction.x())) {
            m_velocity.setX(0);
            m_remainder.setX(0);
        }
    }
    if (length(m_velocity) < kRestSpeed) {
        stop();
    }
}

void ScrollMomentum::stop()
{
    if (!m_gliding) {
        return;
    }
    m_gliding = false;
    m_timer.stop();
}

} // namespace swifty
