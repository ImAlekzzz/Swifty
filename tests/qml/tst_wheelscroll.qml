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

// run with:
//   qmltestrunner -input tests/qml
// (a headless run needs QT_QPA_PLATFORM=offscreen)
import QtQuick
import QtTest
import "../../qml/components"

TestCase {
    id: test
    name: "WheelScroll"
    width: 400
    height: 400
    visible: true
    when: windowShown

    Flickable { id: flick; width: 400; height: 400; contentWidth: 400; contentHeight: 40000
        WheelScroll { id: scroller } }

    function init() {
        flick.contentY = 0
        wait(80)
    }

    function test_oneNotchMovesAStepAndSettles() {
        mouseWheel(flick, 200, 200, 0, -120)
        tryCompare(flick, "contentY", scroller.step, 2000)
    }

    // 20 notches, one every 50 ms: how even is the speed in the middle of it?
    function spin(view) {
        const t0 = Date.now()
        const samples = []
        let nextNotch = 0
        while (Date.now() - t0 < 1300) {
            const t = Date.now() - t0
            if (t >= nextNotch && nextNotch < 1000) {
                mouseWheel(view, 200, 200, 0, -120)
                nextNotch += 50
            }
            samples.push([t, view.contentY])
            wait(4)
        }
        const speeds = []
        let last = samples[0]
        for (let i = 1; i < samples.length; ++i) {
            const dt = samples[i][0] - last[0]
            if (dt >= 16) {
                // a frame-sized window, as the eye sees it
                if (samples[i][0] > 300 && samples[i][0] < 950) {
                    speeds.push((samples[i][1] - last[1]) / dt)
                }
                last = samples[i]
            }
        }
        const mean = speeds.reduce((a, b) => a + b, 0) / speeds.length
        const variance = speeds.reduce((a, b) => a + (b - mean) * (b - mean), 0) / speeds.length
        let backwards = 0
        for (let i = 1; i < samples.length; ++i) {
            if (samples[i][1] < samples[i - 1][1] - 0.001) backwards++
        }
        return {mean: mean, cv: Math.sqrt(variance) / mean, end: view.contentY, backwards: backwards}
    }

    function test_steadySpinningHasEvenSpeed() {
        const fresh = spin(flick)
        compare(fresh.backwards, 0)
        verify(Math.abs(fresh.end - 20 * scroller.step) < 5, "ended at " + fresh.end)
        verify(fresh.cv < 0.5, "speed varies too much: " + fresh.cv)
    }

    // one notch: how fast does it move at the very start, against its fastest?
    function easeInOf(view) {
        const t0 = Date.now()
        const samples = []
        mouseWheel(view, 200, 200, 0, -120)
        while (Date.now() - t0 < 600) {
            samples.push([Date.now() - t0, view.contentY])
            wait(2)
        }
        // speeds over windows of about a frame and a half
        let peak = 0
        let first = -1
        let last = samples[0]
        for (let i = 1; i < samples.length; ++i) {
            const dt = samples[i][0] - last[0]
            if (dt >= 24) {
                const speed = (samples[i][1] - last[1]) / dt
                if (first < 0) {
                    first = speed
                }
                peak = Math.max(peak, speed)
                last = samples[i]
            }
        }
        return {ratio: first / peak, end: view.contentY}
    }

    function test_aNotchEasesInInsteadOfJumpingToSpeed() {
        const result = easeInOf(flick)
        verify(Math.abs(result.end - scroller.step) < 1)
        // a fixed-length glide restarted on each notch is at about 95% of its
        // top speed straight away; the spring is at about 40%
        verify(result.ratio < 0.7, "starts too abruptly: " + result.ratio)
    }

    function test_clampsAtTheTopAndBottom() {
        flick.contentY = 300
        wait(50)
        for (let i = 0; i < 10; ++i) { mouseWheel(flick, 200, 200, 0, 120); wait(10) }
        tryCompare(flick, "contentY", 0, 2000)
        flick.contentY = flick.contentHeight - flick.height - 100
        wait(50)
        for (let i = 0; i < 10; ++i) { mouseWheel(flick, 200, 200, 0, -120); wait(10) }
        tryCompare(flick, "contentY", flick.contentHeight - flick.height, 2000)
    }

    function test_aDragTakesOverFromTheGlide() {
        mouseWheel(flick, 200, 200, 0, -120)
        wait(20)
        flick.contentY = 5000
        wait(300)
        compare(flick.contentY, 5000)
    }
}
