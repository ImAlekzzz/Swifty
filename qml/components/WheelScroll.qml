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

import QtQuick

// scrolls the list or page it sits in by a set distance for each notch of a
// mouse wheel, the way a browser does: the view is drawn toward where the
// wheel has sent it like a critically damped spring, so it eases in, never
// overshoots, and the speed carries on smoothly from one notch to the next
// instead of starting over. qt's own step is only a few lines, which crosses a
// long library very slowly. a touchpad keeps its own smooth scrolling.
WheelHandler {
    id: root

    /// pixels for one notch of the wheel. browsers use about 100
    property real step: 160
    /// about how long a single notch takes to come to rest. smaller is snappier
    property real settleSeconds: 0.24
    // the spring's natural frequency: critically damped, it has covered 95%
    // of the way after 4.74 / omega
    readonly property real omega: 4.74 / settleSeconds

    // a pointer handler sits on the item it is declared in, which for a
    // flickable may be its content item
    readonly property Flickable flickable: (parent as Flickable) ?? (parent?.parent as Flickable)

    acceptedDevices: PointerDevice.Mouse
    // the flickable's own wheel handling would add its small step to ours
    blocking: true

    // where the wheel has sent the view, how fast it is moving there, and
    // where this handler last put it
    property real target: 0
    property real velocity: 0
    property real placed: 0

    function stopGliding() {
        frames.stop()
    }

    property FrameAnimation frames: FrameAnimation {
        onTriggered: {
            const view = root.flickable
            // somebody else moved the view (a drag, the scroll bar, a jump):
            // let go instead of dragging it back
            if (!view || view.dragging || Math.abs(view.contentY - root.placed) > 2) {
                root.velocity = 0
                stop()
                return
            }
            // the spring's exact motion over this frame, whatever its length
            const dt = Math.min(frameTime, 0.05)
            const w = root.omega
            const offset = view.contentY - root.target
            const b = root.velocity + w * offset
            const decay = Math.exp(-w * dt)
            const nextOffset = (offset + b * dt) * decay
            const nextVelocity = (root.velocity - w * b * dt) * decay
            if (Math.abs(nextOffset) < 0.3 && Math.abs(nextVelocity) < 8) {
                view.contentY = root.target
                root.placed = view.contentY
                root.velocity = 0
                stop()
                return
            }
            const top = view.originY - view.topMargin
            const bottom = view.originY + view.contentHeight - view.height + view.bottomMargin
            const next = root.target + nextOffset
            view.contentY = Math.max(top, Math.min(bottom, next))
            // pushed against an end: nothing is left to carry on with
            root.velocity = view.contentY === next ? nextVelocity : 0
            root.placed = view.contentY
        }
    }

    onWheel: (event) => {
        const view = root.flickable
        if (!view || event.angleDelta.y === 0) {
            return
        }
        const top = view.originY - view.topMargin
        const bottom = view.originY + view.contentHeight - view.height + view.bottomMargin
        if (bottom <= top) {
            return
        }
        view.cancelFlick()
        // a glide still under way carries on from where it was headed, with the
        // speed it had, so fast spinning adds up; a view that stopped starts
        // from rest where it is
        if (!frames.running || Math.abs(view.contentY - placed) > 2) {
            target = view.contentY
            velocity = 0
        }
        target = Math.max(top, Math.min(bottom, target - event.angleDelta.y / 120 * root.step))
        placed = view.contentY
        frames.start()
    }
}
