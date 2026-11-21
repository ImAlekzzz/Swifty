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

cmake_minimum_required(VERSION 3.24)

project(Swifty
    VERSION 0.3.3
    DESCRIPTION "A native local music player"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# the window's title bar on macos is styled through appkit
if(APPLE)
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 20)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
endif()

# on windows <cmath> gives M_PI only when asked, and windows.h's min and
# max macros are kept off std::min and std::max; msvc also reads the
# sources as utf-8
if(WIN32)
    add_compile_definitions(_USE_MATH_DEFINES NOMINMAX)
endif()
if(MSVC)
    add_compile_options(/utf-8)
endif()

include(GNUInstallDirs)
include(CTest)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(SWIFTY_IS_LINUX ON)
else()
    set(SWIFTY_IS_LINUX OFF)
endif()

# the wayland, d-bus and pipewire pieces. off elsewhere so the portable parts
# can be built and tested on a development machine without them.
option(SWIFTY_DESKTOP_INTEGRATION "Build the Linux desktop integration" ${SWIFTY_IS_LINUX})

find_package(Qt6 6.10 REQUIRED COMPONENTS
    Core Gui Network Concurrent Sql Qml Quick QuickControls2 ShaderTools)
if(BUILD_TESTING)
    find_package(Qt6 6.10 REQUIRED COMPONENTS Test QuickTest)
endif()
if(SWIFTY_IS_LINUX)
    find_package(Qt6 6.10 REQUIRED COMPONENTS DBus Widgets)
else()
    find_package(Qt6 6.10 OPTIONAL_COMPONENTS DBus)
endif()
if(SWIFTY_DESKTOP_INTEGRATION)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PULSE REQUIRED IMPORTED_TARGET libpulse)
endif()
if(WIN32)
    find_package(Qt6 6.10 REQUIRED COMPONENTS Widgets)
endif()
find_package(QCoro6 0.10 REQUIRED COMPONENTS Core Network)
qcoro_enable_coroutines()

qt_standard_project_setup(REQUIRES 6.10)
# modules get their own folder so tooling finds them as <dir>/Swifty/qmldir,
# clear of the executable (the same name on a case-insensitive disk).
set(QT_QML_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/qml_modules)

# --- audio-core (Rust) -----------------------------------------------------

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/audio-core/Cargo.toml)
    set(SWIFTY_CORE_DEFAULT ${CMAKE_CURRENT_SOURCE_DIR}/audio-core)
else()
    set(SWIFTY_CORE_DEFAULT ${CMAKE_CURRENT_SOURCE_DIR}/../audio-core)
endif()
set(SWIFTY_CORE_DIR ${SWIFTY_CORE_DEFAULT} CACHE PATH "The Rust core, audio-core")

include(FetchContent)
FetchContent_Declare(Corrosion
    GIT_REPOSITORY https://github.com/corrosion-rs/corrosion.git
    GIT_TAG v0.6.1)
FetchContent_MakeAvailable(Corrosion)

corrosion_import_crate(
    MANIFEST_PATH ${SWIFTY_CORE_DIR}/Cargo.toml
    CRATE_TYPES staticlib
    NO_DEFAULT_FEATURES
    FEATURES capi)
target_include_directories(audio_core INTERFACE
    ${SWIFTY_CORE_DIR}/include)
# what rustc reports as the staticlib's native dependencies
# (cargo rustc -- --print native-static-libs).
if(APPLE)
    target_link_libraries(audio_core INTERFACE
        "-framework AudioToolbox" "-framework CoreAudio" "-framework IOKit"
        "-framework CoreFoundation" "-framework Foundation" "-framework SystemConfiguration"
        "-framework Security" objc iconv)
elseif(WIN32)
    # windows: wasapi for audio, and what rust's std needs; the linker leaves out any of them nothing uses
    target_link_libraries(audio_core INTERFACE
        ws2_32 userenv bcrypt ntdll advapi32 kernel32 user32 shell32 ole32 oleaut32
        propsys crypt32 secur32 ncrypt runtimeobject synchronization)
else()
    find_package(ALSA REQUIRED)
    find_package(Threads REQUIRED)
    target_link_libraries(audio_core INTERFACE
        ALSA::ALSA Threads::Threads ${CMAKE_DL_LIBS} m)
endif()

# --- material-color-utilities ------------------------------------------------

file(GLOB_RECURSE MCU_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/material-color-utilities/cpp/*.cc)
add_library(material_color_utilities STATIC ${MCU_SOURCES})
target_include_directories(material_color_utilities PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/material-color-utilities)
set_target_properties(material_color_utilities PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- fonts ---------------------------------------------------------------------

option(SWIFTY_DOWNLOAD_ICON_FONT
    "Download Material Symbols Rounded to install beside the app" ON)
include(cmake/DownloadAsset.cmake)
set(SWIFTY_FONT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/fonts)
if(SWIFTY_DOWNLOAD_ICON_FONT)
    set(SWIFTY_ICON_FONT ${SWIFTY_FONT_DIRECTORY}/MaterialSymbolsRounded.ttf)
    swifty_download_asset(
        "https://raw.githubusercontent.com/google/material-design-icons/737e3324305806514d7909874fa1818ae1808232/variablefont/MaterialSymbolsRounded%5BFILL,GRAD,opsz,wght%5D.ttf"
        "${SWIFTY_ICON_FONT}"
        95b24392bb49efd1bc3e92cff4e2452ad094461bab7c97e7d8723fab97e330ca)
    if(NOT APPLE)
        install(FILES ${SWIFTY_ICON_FONT} DESTINATION ${CMAKE_INSTALL_DATADIR}/swifty/fonts)
    endif()
    # the font's list of names, from the same commit: the tests check every
    # symbol the qml asks for is in it, since a misspelt one shows as text
    set(SWIFTY_ICON_CODEPOINTS ${SWIFTY_FONT_DIRECTORY}/MaterialSymbolsRounded.codepoints)
    if(BUILD_TESTING)
        swifty_download_asset(
            "https://raw.githubusercontent.com/google/material-design-icons/737e3324305806514d7909874fa1818ae1808232/variablefont/MaterialSymbolsRounded%5BFILL,GRAD,opsz,wght%5D.codepoints"
            "${SWIFTY_ICON_CODEPOINTS}"
            225bd09137103cb7746bc93dc08d08764c9f0c3bd04f4b958d4a3c3c19432dd6)
    endif()
endif()

# google sans flex, end4's face: the interface text, and the titles in it
# drawn round, wide and heavy through its variable axes. its licence travels
# with it.
option(SWIFTY_DOWNLOAD_INTERFACE_FONT
    "Download Google Sans Flex to install beside the app" ON)
if(SWIFTY_DOWNLOAD_INTERFACE_FONT)
    set(SWIFTY_INTERFACE_FONT ${SWIFTY_FONT_DIRECTORY}/GoogleSansFlex.ttf)
    set(SWIFTY_INTERFACE_FONT_LICENSE ${SWIFTY_FONT_DIRECTORY}/GoogleSansFlex-OFL.txt)
    set(SWIFTY_GOOGLE_FONTS https://raw.githubusercontent.com/google/fonts/a0e3dbcdc3a3ecfafff3f071159ae0221628922d/ofl/googlesansflex)
    swifty_download_asset(
        "${SWIFTY_GOOGLE_FONTS}/GoogleSansFlex%5BGRAD,ROND,opsz,slnt,wdth,wght%5D.ttf"
        "${SWIFTY_INTERFACE_FONT}"
        c31a482fbecbf2e07e6890134d20078723aadf732c9b9c6c9a44f86f8265b6fe)
    swifty_download_asset("${SWIFTY_GOOGLE_FONTS}/OFL.txt"
        "${SWIFTY_INTERFACE_FONT_LICENSE}"
        fc13d69f63e36d284b6e383d4d1463d8ad404f0aa065e57cf961252d51063137)
    if(NOT APPLE)
        install(FILES ${SWIFTY_INTERFACE_FONT} ${SWIFTY_INTERFACE_FONT_LICENSE}
            DESTINATION ${CMAKE_INSTALL_DATADIR}/swifty/fonts)
    endif()
endif()

# --- licences ------------------------------------------------------------------

# the code built into swifty or bundled with it, whose licences go out with
# every copy beside its own. they're copied under their installed names first,
# as the app bundle can't rename what it carries
set(SWIFTY_LICENSE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/licenses)
configure_file(third_party/material-color-utilities/LICENSE
    ${SWIFTY_LICENSE_DIRECTORY}/material-color-utilities.LICENSE COPYONLY)
configure_file(third_party/rounded-polygon-qmljs/LICENSE
    ${SWIFTY_LICENSE_DIRECTORY}/rounded-polygon-qmljs.LICENSE COPYONLY)
set(SWIFTY_THIRD_PARTY_LICENSES
    ${SWIFTY_LICENSE_DIRECTORY}/material-color-utilities.LICENSE
    ${SWIFTY_LICENSE_DIRECTORY}/rounded-polygon-qmljs.LICENSE
    # the rust crates of audio-core, symphonia's MPL-2.0 among them
    ${CMAKE_CURRENT_SOURCE_DIR}/packaging/licenses/rust-crates.LICENSE)
# material symbols, the icon font downloaded above
if(SWIFTY_DOWNLOAD_ICON_FONT)
    list(APPEND SWIFTY_THIRD_PARTY_LICENSES ${CMAKE_CURRENT_SOURCE_DIR}/packaging/licenses/material-symbols.LICENSE)
endif()
# the prebuilt downloads bundle qt and qcoro, and their licences go with them.
# a distro's package uses the system's and leaves them off
option(SWIFTY_BUNDLED_QT "Install the licences of Qt and QCoro, for a build that bundles them" OFF)
if(SWIFTY_BUNDLED_QT)
    configure_file(packaging/licenses/qt.NOTICE.in ${SWIFTY_LICENSE_DIRECTORY}/qt.NOTICE @ONLY)
    list(APPEND SWIFTY_THIRD_PARTY_LICENSES
        ${CMAKE_CURRENT_SOURCE_DIR}/packaging/licenses/qt.LICENSE
        ${SWIFTY_LICENSE_DIRECTORY}/qt.NOTICE
        ${CMAKE_CURRENT_SOURCE_DIR}/packaging/licenses/qcoro.LICENSE)
endif()

# --- the app ---------------------------------------------------------------------

file(GLOB_RECURSE SWIFTY_SOURCES CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    src/*.cpp src/*.h src/*.mm)
list(REMOVE_ITEM SWIFTY_SOURCES src/main.cpp)
if(NOT APPLE)
    list(FILTER SWIFTY_SOURCES EXCLUDE REGEX "^src/system/macos/")
endif()
if(NOT WIN32)
    list(FILTER SWIFTY_SOURCES EXCLUDE REGEX "^src/system/windows/")
endif()
# the media controls of windows use c++/winrt, from the windows sdk that comes
# with visual studio. mingw's headers do not have it, and build without. they
# can also be turned off, with -DSWIFTY_WINDOWS_MEDIA_CONTROLS=OFF
option(SWIFTY_WINDOWS_MEDIA_CONTROLS "Windows volume flyout, lock screen and media keys (MSVC only)" ON)
if(NOT (MSVC AND SWIFTY_WINDOWS_MEDIA_CONTROLS))
    list(FILTER SWIFTY_SOURCES EXCLUDE REGEX "^src/system/windows/MediaTransport")
    if(WIN32)
        message(STATUS "building without the Windows media controls")
    endif()
endif()
if(NOT SWIFTY_DESKTOP_INTEGRATION)
    list(FILTER SWIFTY_SOURCES EXCLUDE REGEX "^src/system/linux/")
endif()
if(NOT TARGET Qt6::DBus)
    list(FILTER SWIFTY_SOURCES EXCLUDE REGEX "^src/system/dbus/")
endif()

file(GLOB_RECURSE SWIFTY_QML CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    qml/*.qml)
file(GLOB SWIFTY_SINGLETONS CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    qml/design/*.qml)
set_source_files_properties(${SWIFTY_SINGLETONS} PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

file(GLOB SWIFTY_SHADERS CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    shaders/*.frag shaders/*.vert)

qt_add_library(swifty_app STATIC)
qt_add_qml_module(swifty_app
    URI Swifty
    VERSION 1.0
    SOURCES ${SWIFTY_SOURCES}
    QML_FILES ${SWIFTY_QML}
    RESOURCES resources/swifty-icon.png resources/cameo.png
    DEPENDENCIES QtQuick)
# BATCHABLE: the spectrum's material can be merged into the scene graph's
# batches, which needs the extra vertex input qsb adds for it.
qt_add_shaders(swifty_app "swifty_shaders"
    BATCHABLE
    PREFIX "/shaders"
    BASE shaders
    FILES ${SWIFTY_SHADERS})

# material 3's shapes as distance fields, for MaterialShape: drawn at build
# time from the vendored rounded-polygon-qmljs (tools/shape_atlas)
# the tool runs during the build, so a cross build (windows from a mac, say)
# takes one built for the machine doing the building
set(SWIFTY_SHAPE_ATLAS_TOOL "" CACHE FILEPATH
    "a shape_atlas built for the build machine, for cross builds")
if(SWIFTY_SHAPE_ATLAS_TOOL)
    set(SWIFTY_SHAPE_ATLAS_COMMAND ${SWIFTY_SHAPE_ATLAS_TOOL})
elseif(CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "a cross build needs -DSWIFTY_SHAPE_ATLAS_TOOL=<shape_atlas built for this machine>")
else()
    add_executable(shape_atlas tools/shape_atlas/main.cpp tools/shape_atlas/ShapeAtlasLayout.h)
    target_include_directories(shape_atlas PRIVATE tools/shape_atlas)
    target_link_libraries(shape_atlas PRIVATE Qt6::Core Qt6::Gui Qt6::Qml)
    # it runs on every fresh build, so it is optimised even in a debug one
    target_compile_options(shape_atlas PRIVATE $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-O2>)
    set(SWIFTY_SHAPE_ATLAS_COMMAND shape_atlas)
endif()
set(SWIFTY_SHAPE_LIBRARY ${CMAKE_CURRENT_SOURCE_DIR}/third_party/rounded-polygon-qmljs)
file(GLOB_RECURSE SWIFTY_SHAPE_SOURCES CONFIGURE_DEPENDS ${SWIFTY_SHAPE_LIBRARY}/*.js)
set(SWIFTY_SHAPE_ATLAS ${CMAKE_CURRENT_BINARY_DIR}/shapes/shape_atlas.png)
add_custom_command(OUTPUT ${SWIFTY_SHAPE_ATLAS}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/shapes
    COMMAND ${SWIFTY_SHAPE_ATLAS_COMMAND} ${SWIFTY_SHAPE_LIBRARY} ${SWIFTY_SHAPE_ATLAS}
    DEPENDS ${SWIFTY_SHAPE_ATLAS_COMMAND} ${SWIFTY_SHAPE_SOURCES}
    COMMENT "Drawing the shape atlas"
    VERBATIM)
qt_add_resources(swifty_app "swifty_shape_atlas"
    PREFIX "/shapes"
    BASE ${CMAKE_CURRENT_BINARY_DIR}/shapes
    FILES ${SWIFTY_SHAPE_ATLAS})

target_include_directories(swifty_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
# qmltyperegistrar includes registered headers by file name alone, so every
# folder holding one is searched too. header names stay unique across src/.
set(SWIFTY_SOURCE_DIRECTORIES)
foreach(source IN LISTS SWIFTY_SOURCES)
    get_filename_component(directory ${CMAKE_CURRENT_SOURCE_DIR}/${source} DIRECTORY)
    list(APPEND SWIFTY_SOURCE_DIRECTORIES ${directory})
endforeach()
list(REMOVE_DUPLICATES SWIFTY_SOURCE_DIRECTORIES)
target_include_directories(swifty_app PRIVATE ${SWIFTY_SOURCE_DIRECTORIES})
# MaterialShapeItem finds a shape's cell the way the atlas tool lays them out
target_include_directories(swifty_app PRIVATE tools/shape_atlas)
target_compile_definitions(swifty_app PUBLIC SWIFTY_VERSION="${PROJECT_VERSION}")
target_compile_definitions(swifty_app PRIVATE
    SWIFTY_BUILD_FONT_DIRECTORY="${SWIFTY_FONT_DIRECTORY}"
    $<$<BOOL:${SWIFTY_DESKTOP_INTEGRATION}>:SWIFTY_DESKTOP_INTEGRATION>
    $<$<TARGET_EXISTS:Qt6::DBus>:SWIFTY_HAS_DBUS>)
if(TARGET Qt6::DBus)
    target_link_libraries(swifty_app PUBLIC Qt6::DBus)
endif()
if(SWIFTY_DESKTOP_INTEGRATION)
    target_link_libraries(swifty_app PUBLIC PkgConfig::PULSE)
endif()
if(APPLE)
    target_link_libraries(swifty_app PUBLIC "-framework AppKit" "-framework MediaPlayer" "-framework QuartzCore")
    # now playing holds its objective-c objects with arc
    set_source_files_properties(src/system/macos/NowPlaying.mm src/system/macos/MacWindow.mm
        PROPERTIES COMPILE_OPTIONS -fobjc-arc)
endif()
target_link_libraries(swifty_app PUBLIC
    Qt6::Core Qt6::Gui Qt6::Network Qt6::Concurrent Qt6::Sql Qt6::Qml Qt6::Quick Qt6::QuickControls2
    QCoro6::Core QCoro6::Network
    material_color_utilities
    audio_core)

if(WIN32)
    target_link_libraries(swifty_app PUBLIC Qt6::Widgets comctl32 dwmapi ole32 shell32 uuid)
endif()
if(MSVC AND SWIFTY_WINDOWS_MEDIA_CONTROLS)
    # the volume flyout, lock screen and media keys (MediaTransport)
    target_compile_definitions(swifty_app PRIVATE SWIFTY_HAS_SMTC)
    target_link_libraries(swifty_app PRIVATE WindowsApp)
endif()

qt_add_executable(swifty src/main.cpp)
# main.cpp builds the media controls (MPRIS), the sleep monitor and the audio
# device monitor under these, and is not part of swifty_app, whose private
# definitions it would otherwise never see: without them none of the three
# was ever made on Linux, and no shell could see what was playing
target_compile_definitions(swifty PRIVATE
    $<$<BOOL:${SWIFTY_DESKTOP_INTEGRATION}>:SWIFTY_DESKTOP_INTEGRATION>
    $<$<TARGET_EXISTS:Qt6::DBus>:SWIFTY_HAS_DBUS>)
target_link_libraries(swifty PRIVATE swifty_app swifty_appplugin)
if(SWIFTY_IS_LINUX)
    target_link_libraries(swifty PRIVATE Qt6::Widgets)
endif()
# a windowed app on windows, with no console window of its own
set_target_properties(swifty PROPERTIES WIN32_EXECUTABLE ON)
if(MINGW)
    # qt's WinMain shim reads msvcrt's __argc, which a mingw on ucrt (as
    # homebrew's is) lacks; mingw's own start-up calls main() just as well
    set_target_properties(swifty PROPERTIES qt_no_entrypoint ON)
endif()

# on macos, an app bundle: its own name in the menu bar and the dock, its own
# icon, and an identifier apart from the swift app's (com.addenator.Swifty)
if(APPLE)
    set(SWIFTY_ICON_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/packaging/icons)
    # the swift app's icon composer icon, compiled as xcode compiles it: an
    # asset catalog that macos 26 draws with its glass and its dark and tinted
    # looks, and an .icns for older systems. without xcode's actool, an .icns
    # made from the rendered pngs instead.
    execute_process(COMMAND xcrun --find actool
        OUTPUT_VARIABLE SWIFTY_ACTOOL OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(SWIFTY_ACTOOL)
        set(SWIFTY_ICON_OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/icon)
        file(GLOB SWIFTY_ICON_PARTS LIST_DIRECTORIES false CONFIGURE_DEPENDS ${SWIFTY_ICON_SOURCE}/swifty.icon/*
            ${SWIFTY_ICON_SOURCE}/swifty.icon/Assets/*)
        add_custom_command(OUTPUT ${SWIFTY_ICON_OUTPUT}/Assets.car ${SWIFTY_ICON_OUTPUT}/swifty.icns
            COMMAND ${CMAKE_COMMAND} -E make_directory ${SWIFTY_ICON_OUTPUT}
            COMMAND ${SWIFTY_ACTOOL} --compile ${SWIFTY_ICON_OUTPUT} --platform macosx
                --minimum-deployment-target 13.0 --app-icon swifty
                --output-partial-info-plist ${SWIFTY_ICON_OUTPUT}/icon.plist
                ${SWIFTY_ICON_SOURCE}/swifty.icon > /dev/null
            DEPENDS ${SWIFTY_ICON_PARTS}
            COMMENT "Compiling the app icon")
        set(SWIFTY_ICON_FILES ${SWIFTY_ICON_OUTPUT}/Assets.car ${SWIFTY_ICON_OUTPUT}/swifty.icns)
    else()
        set(SWIFTY_ICONSET ${CMAKE_CURRENT_BINARY_DIR}/swifty.iconset)
        set(SWIFTY_ICNS ${CMAKE_CURRENT_BINARY_DIR}/swifty.icns)
        add_custom_command(OUTPUT ${SWIFTY_ICNS}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${SWIFTY_ICONSET}
            COMMAND sips -z 16 16 ${SWIFTY_ICON_SOURCE}/swifty-32.png --out ${SWIFTY_ICONSET}/icon_16x16.png > /dev/null
            COMMAND ${CMAKE_COMMAND} -E copy ${SWIFTY_ICON_SOURCE}/swifty-32.png ${SWIFTY_ICONSET}/icon_16x16@2x.png
            COMMAND ${CMAKE_COMMAND} -E copy ${SWIFTY_ICON_SOURCE}/swifty-32.png ${SWIFTY_ICONSET}/icon_32x32.png
            COMMAND ${CMAKE_COMMAND} -E copy ${SWIFTY_ICON_SOURCE}/swifty-64.png ${SWIFTY_ICONSET}/icon_32x32@2x.png
            COMMAND ${CMAKE_COMMAND} -E copy ${SWIFTY_ICON_SOURCE}/swifty-128.png ${SWIFTY_ICONSET}/icon_128x128.png
            COMMAND ${CMAKE_COMMAND} -E copy ${SWIFTY_ICON_SOURCE}/swifty-256.png ${SWIFTY_ICONSET}/icon_128x128@2x.png
            COMMAND ${CMAKE_COMMAND} -E copy ${SWIFTY_ICON_SOURCE}/swifty-256.png ${SWIFTY_ICONSET}/icon_256x256.png
            COMMAND ${CMAKE_COMMAND} -E copy ${SWIFTY_ICON_SOURCE}/swifty-512.png ${SWIFTY_ICONSET}/icon_256x256@2x.png
            COMMAND ${CMAKE_COMMAND} -E copy ${SWIFTY_ICON_SOURCE}/swifty-512.png ${SWIFTY_ICONSET}/icon_512x512.png
            COMMAND iconutil --convert icns --output ${SWIFTY_ICNS} ${SWIFTY_ICONSET}
            DEPENDS ${SWIFTY_ICON_SOURCE}/swifty-32.png ${SWIFTY_ICON_SOURCE}/swifty-64.png
                ${SWIFTY_ICON_SOURCE}/swifty-128.png ${SWIFTY_ICON_SOURCE}/swifty-256.png
                ${SWIFTY_ICON_SOURCE}/swifty-512.png
            COMMENT "Making the app icon")
        set(SWIFTY_ICON_FILES ${SWIFTY_ICNS})
    endif()
    set_source_files_properties(${SWIFTY_ICON_FILES} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    target_sources(swifty PRIVATE ${SWIFTY_ICON_FILES})
    if(SWIFTY_ICON_FONT)
        set_source_files_properties(${SWIFTY_ICON_FONT} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/fonts)
        target_sources(swifty PRIVATE ${SWIFTY_ICON_FONT})
    endif()
    if(SWIFTY_INTERFACE_FONT)
        set_source_files_properties(${SWIFTY_INTERFACE_FONT} ${SWIFTY_INTERFACE_FONT_LICENSE}
            PROPERTIES MACOSX_PACKAGE_LOCATION Resources/fonts)
        target_sources(swifty PRIVATE ${SWIFTY_INTERFACE_FONT} ${SWIFTY_INTERFACE_FONT_LICENSE})
    endif()
    set_source_files_properties(LICENSE THIRD_PARTY_LICENSES.md PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    target_sources(swifty PRIVATE LICENSE THIRD_PARTY_LICENSES.md)
    set_source_files_properties(${SWIFTY_THIRD_PARTY_LICENSES} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    target_sources(swifty PRIVATE ${SWIFTY_THIRD_PARTY_LICENSES})
    set_target_properties(swifty PROPERTIES
        MACOSX_BUNDLE ON
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/Info.plist.in
        MACOSX_BUNDLE_BUNDLE_NAME Swifty
        MACOSX_BUNDLE_GUI_IDENTIFIER dev.imalekzzz.Swifty
        MACOSX_BUNDLE_ICON_FILE swifty.icns
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION})
    # the linker signs only the executable; signing the bundle seals the icon
    # and the font in with it, so the whole app checks out. a folder synced by
    # icloud tags files with finder and file-provider attributes, which
    # codesign refuses to seal, so they go first.
    # ad hoc by default. an identity of one's own keeps the signature's
    # requirement the same from build to build, so the keychain hands over
    # the stored logins without asking again after each one
    set(SWIFTY_CODESIGN_IDENTITY "-" CACHE STRING "codesign identity for the app bundle; - signs ad hoc")
    add_custom_command(TARGET swifty POST_BUILD
        COMMAND xattr -cr $<TARGET_BUNDLE_DIR:swifty>
        COMMAND codesign --force --sign ${SWIFTY_CODESIGN_IDENTITY} $<TARGET_BUNDLE_DIR:swifty>
        COMMENT "Signing the app bundle")
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# the licence and the credits go with every install, and so do the licences
# of what is built into the app or bundled with it
install(FILES LICENSE THIRD_PARTY_LICENSES.md DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/swifty)
install(FILES ${SWIFTY_THIRD_PARTY_LICENSES} DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/swifty)

install(TARGETS swifty
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    BUNDLE DESTINATION .)
if(NOT APPLE AND NOT WIN32)
    # the desktop entry's name is the wayland app id (see main.cpp), which is
    # how hyprland and the tray match the window to its icon
    install(FILES packaging/swifty.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    foreach(size 32 48 64 128 256 512)
        install(FILES packaging/icons/swifty-${size}.png
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps
            RENAME swifty.png)
    endforeach()
endif()
