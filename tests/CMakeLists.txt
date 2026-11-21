#[[
    Swiftify, a spotify player for desktop
    Copyright (C) 2026  shred0s
    Copyright (C) 2026  ImAlekz (modified for Swifty)

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
]]

add_test(NAME download_asset
    COMMAND ${CMAKE_COMMAND} "-DTEST_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/download_asset"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/check_download_asset.cmake)

function(swifty_test name)
    qt_add_executable(${name} ${name}.cpp ${ARGN})
    target_link_libraries(${name} PRIVATE swifty_app Qt6::Test)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endfunction()

swifty_test(tst_settings)
swifty_test(tst_lyrics)
swifty_test(tst_artistimages)
swifty_test(tst_theme)
swifty_test(tst_playback FakeCore.h)
swifty_test(tst_palette)
swifty_test(tst_spectrum FakeCore.h)
swifty_test(tst_covers)
swifty_test(tst_views FakeCore.h)
swifty_test(tst_local FakeCore.h)
# frames are read back through qrhi, which is only offered as a private api
# only this test binary, which is rebuilt with qt anyway
set(QT_NO_PRIVATE_MODULE_WARNING ON)
find_package(Qt6 REQUIRED COMPONENTS GuiPrivate)
target_link_libraries(tst_views PRIVATE swifty_appplugin Qt6::GuiPrivate)
if(WIN32)
    find_program(SWIFTY_POWERSHELL NAMES powershell pwsh)
    if(SWIFTY_POWERSHELL)
        add_test(NAME windows_build_script
            COMMAND ${SWIFTY_POWERSHELL} -NoProfile -ExecutionPolicy Bypass
                -File ${CMAKE_CURRENT_SOURCE_DIR}/check_windows_build.ps1)
    endif()
    swifty_test(tst_windows FakeCore.h)
    target_link_libraries(tst_windows PRIVATE swifty_appplugin Qt6::GuiPrivate)
endif()

# what a frame of the main window costs: run by hand, not by ctest
find_package(Qt6 REQUIRED COMPONENTS QuickPrivate)
qt_add_executable(bench_frames bench_frames.cpp FakeCore.h ViewRig.h)
target_link_libraries(bench_frames PRIVATE swifty_app swifty_appplugin Qt6::Test Qt6::GuiPrivate Qt6::QuickPrivate)

# every material symbol the qml names exists in the pinned font
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND AND SWIFTY_ICON_CODEPOINTS)
    add_test(NAME icon_names
        COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/check_icon_names.py
            ${SWIFTY_ICON_CODEPOINTS} ${PROJECT_SOURCE_DIR}/qml)
endif()
# a script line opening with a bracket runs on from the one before
if(Python3_FOUND)
    add_test(NAME qml_statements
        COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/check_qml_statements.py ${PROJECT_SOURCE_DIR}/qml)
endif()
