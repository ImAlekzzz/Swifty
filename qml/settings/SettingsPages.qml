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

pragma ComponentBehavior: Bound
import QtQuick
import Swifty

// the settings themselves (SwiftySettingsView), one page per rail
// destination: what the visualizers and the backdrop cost, the sleep fade and
// audio quality, the folders of music of the person's own, how the app fits
// the desktop, how much it moves and draws, the
// lyrics, artist pictures, and about. every change applies at once.
Item {
    id: root

    property int page: 0
    readonly property list<string> fades: ["none", "finalSeconds", "entire"]

    component Page: Column {
        width: parent ? parent.width : 0
        spacing: 24
    }

    /// a login's state: a label, and the state beside a dot that says
    /// whether all is well
    component StatusRow: Item {
        id: status

        property string label
        property string value
        property bool good: false

        width: parent ? parent.width : 0
        height: 40

        StyledText {
            x: 12
            anchors.verticalCenter: parent.verticalCenter
            text: status.label
        }
        Row {
            anchors.right: parent.right
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8

            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: 8
                height: 8
                radius: 4
                color: status.good ? Appearance.colors.primary : Appearance.colors.error
            }
            StyledText {
                anchors.verticalCenter: parent.verticalCenter
                text: status.value
                color: Appearance.colors.onSurfaceVariant
            }
        }
    }

    implicitHeight: pages.implicitHeight

    // only the chosen page is visible, and a column leaves the rest no room
    Column {
        id: pages
        width: parent.width

        Page {
            visible: root.page === 0

            ContentSection {
                title: "Visualizer"
                symbol: "graphic_eq"

                ConfigSwitch {
                    text: "Show audio visualizers"
                    checked: Settings.visualizersEnabled
                    onToggledTo: (checked) => Settings.visualizersEnabled = checked
                }
                ConfigSelection {
                    text: "Style"
                    enabled: Settings.visualizersEnabled
                    currentIndex: Settings.circularVisualizer ? 1 : 0
                    segments: [
                        Segment { text: "Normal" },
                        Segment { text: "Circular" }
                    ]
                    onActivated: (index) => Settings.circularVisualizer = index === 1
                }
                ConfigSelection {
                    text: "Shape"
                    enabled: Settings.visualizersEnabled
                    currentIndex: Settings.visualizerShape === "wave" ? 1 : 0
                    segments: [
                        Segment { text: "Bars" },
                        Segment { text: "Wave" }
                    ]
                    onActivated: (index) => Settings.visualizerShape = index === 1 ? "wave" : "bars"
                }
                ConfigSwitch {
                    text: "Round artwork"
                    // with visualizers on, the circular style rounds the
                    // cover inside its ring
                    enabled: !Settings.visualizersEnabled
                    checked: Settings.roundArtwork || (Settings.visualizersEnabled && Settings.circularVisualizer)
                    onToggledTo: (checked) => Settings.roundArtwork = checked
                }
                ConfigSlider {
                    text: "Sensitivity"
                    enabled: Settings.visualizersEnabled
                    from: 0.6
                    to: 1.8
                    stepSize: 0.05
                    value: Settings.visualizerSensitivity
                    valueText: Math.round(Settings.visualizerSensitivity * 100) + "%"
                    onMoved: (value) => Settings.visualizerSensitivity = value
                }
                NoteText {
                    text: "turn this off if u want lower cpu cost"
                }
            }
        }

        Page {
            visible: root.page === 1

            ContentSection {
                title: "Background"
                symbol: "wallpaper"

                ConfigSwitch {
                    text: "Live background"
                    checked: Settings.livingBackdrop
                    onToggledTo: (checked) => Settings.livingBackdrop = checked
                }
                ConfigSlider {
                    text: "Blur"
                    from: 0
                    to: 60
                    stepSize: 1
                    value: Settings.backdropBlur
                    onMoved: (value) => Settings.backdropBlur = value
                }
                ConfigSlider {
                    text: "Reactivity"
                    enabled: Settings.livingBackdrop
                    from: 0
                    to: 3
                    stepSize: 0.1
                    value: Settings.backdropReactivity
                    valueText: Math.round(Settings.backdropReactivity * 100) + "%"
                    onMoved: (value) => Settings.backdropReactivity = value
                }
                NoteText {
                    text: "turn this off if u want lower cpu cost"
                }
            }
        }

        Page {
            visible: root.page === 2

            ContentSection {
                title: "Lyrics"
                symbol: "lyrics"

                ConfigSwitch {
                    text: "Look lyrics up online"
                    checked: Settings.onlineLyrics
                    onToggledTo: (checked) => Settings.onlineLyrics = checked
                }
                NoteText {
                    text: "asks LRCLIB (and MusicBrainz, to fix up names) for synced lyrics before using a .lrc file or the lyrics inside the file. this sends the title, artist, album and length of what plays to them. found lyrics are kept on this computer"
                }
            }

            ContentSection {
                title: "Sleep Timer"
                symbol: "bedtime"

                ConfigSelection {
                    text: "Fade Out"
                    currentIndex: Math.max(0, root.fades.indexOf(Settings.sleepTimerFade))
                    segments: [
                        Segment { text: "None" },
                        Segment { text: "Last 10 Seconds" },
                        Segment { text: "Whole Timer" }
                    ]
                    onActivated: (index) => Settings.sleepTimerFade = root.fades[index]
                }
                NoteText {
                    text: "fading lowers the volume as the timer runs down and restores it once playback stops."
                }
            }
        }

        Page {
            visible: root.page === 3

            ContentSection {
                title: "Local Files"
                symbol: "folder_open"

                MusicPicker {
                    id: musicPicker
                }

                Repeater {
                    model: LocalFiles.sources

                    Item {
                        id: source

                        required property string modelData

                        width: parent ? parent.width : 0
                        height: 48

                        MaterialSymbol {
                            id: sourceSymbol
                            x: 12
                            anchors.verticalCenter: parent.verticalCenter
                            text: source.modelData.match(/\.[A-Za-z0-9]{2,4}$/) ? "audio_file" : "folder"
                            iconSize: 20
                            color: Appearance.colors.onSurfaceVariant
                        }
                        StyledText {
                            anchors.left: sourceSymbol.right
                            anchors.leftMargin: 12
                            anchors.right: removeSource.left
                            anchors.rightMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            text: source.modelData
                            elide: Text.ElideMiddle
                        }
                        IconButton {
                            id: removeSource
                            anchors.right: parent.right
                            anchors.rightMargin: 4
                            anchors.verticalCenter: parent.verticalCenter
                            symbol: "close"
                            size: 32
                            iconSize: 18
                            tooltip: "Remove"
                            onClicked: LocalFiles.removeSource(source.modelData)
                        }
                    }
                }

                Flow {
                    x: 8
                    width: parent.width - 16
                    spacing: 8
                    topPadding: LocalFiles.sources.length > 0 ? 4 : 0
                    bottomPadding: 8

                    TextButton {
                        variant: TextButton.Tonal
                        symbol: "create_new_folder"
                        text: "Add Folder"
                        onClicked: musicPicker.pickFolder()
                    }
                    TextButton {
                        variant: TextButton.Text
                        symbol: "audio_file"
                        text: "Add Files"
                        onClicked: musicPicker.pickFiles()
                    }
                    TextButton {
                        variant: TextButton.Text
                        symbol: "refresh"
                        text: LocalFiles.scanning ? "Looking…" : "Look Again"
                        enabled: LocalFiles.sources.length > 0 && !LocalFiles.scanning
                        onClicked: LocalFiles.rescan()
                    }
                }

                NoteText {
                    text: "each songs title, artists, album and cover come from its own tags, and a .lrc file "
                          + "beside a song gives its lyrics. you can also drop files or folders onto the window"
                }
            }

            ContentSection {
                title: "Artist Pictures"
                symbol: "account_circle"

                ConfigSwitch {
                    text: "Look artist pictures up online"
                    checked: Settings.onlineArtistImages
                    onToggledTo: (checked) => Settings.onlineArtistImages = checked
                }
                NoteText {
                    text: "asks Discogs for a photo of each artist (it needs the token below). when it has none, or cannot be reached, a picture named artist (as a .jpg, .png, .webp, .gif, .bmp, .svg, .ico and other common formats) in the folder with the artist's name is used, and after that the cover of one of their songs. this sends artist names to Discogs"
                }
                Item {
                    width: parent.width
                    height: token.implicitHeight + 16
                    visible: Settings.onlineArtistImages

                    OutlinedTextField {
                        id: token
                        x: 8
                        y: 8
                        width: parent.width - 16
                        placeholder: "Discogs token"
                        text: Settings.discogsToken
                        onAccepted: Settings.discogsToken = text
                    }
                }
                NoteText {
                    visible: Settings.onlineArtistImages
                    text: "Discogs asks for a free personal token: discogs.com/settings/developers, Generate new token. without one no picture is looked up. press enter to save it"
                }
            }

            ContentSection {
                title: "Discord"
                symbol: "headphones"

                ConfigSwitch {
                    text: "Show what I'm playing on Discord"
                    checked: Settings.discordPresence
                    onToggledTo: (checked) => Settings.discordPresence = checked
                }
                NoteText {
                    text: "shows the song, its artists and how far into it you are on your Discord profile, through the Discord app on this computer. nothing is shown while paused, or when Discord is not running"
                }
                Item {
                    width: parent.width
                    height: discordId.implicitHeight + 16
                    visible: Settings.discordPresence

                    OutlinedTextField {
                        id: discordId
                        x: 8
                        y: 8
                        width: parent.width - 16
                        placeholder: Settings.discordDefaultClientId
                        text: Settings.discordClientId
                        onAccepted: Settings.discordClientId = text
                    }
                }
                NoteText {
                    visible: Settings.discordPresence
                    text: "it shows as \"Listening to Swifty\". to show another name, create an application at discord.com/developers/applications, name it what you want to read after \"Listening to\", and paste its Application ID in the field (the grey number is Swifty's own). press enter to save it; empty goes back to Swifty's"
                }
            }
        }

        Page {
            visible: root.page === 4

            ContentSection {
                title: "Colours"
                symbol: "palette"

                ConfigSelection {
                    text: "Light or dark"
                    // Matugen exports can contain both light and dark variants.
                    currentIndex: Math.max(0, ["system", "light", "dark"].indexOf(Settings.themeMode))
                    segments: [
                        Segment { text: "System"; symbol: "brightness_auto" },
                        Segment { text: "Light"; symbol: "light_mode" },
                        Segment { text: "Dark"; symbol: "dark_mode" }
                    ]
                    onActivated: (index) => Settings.themeMode = ["system", "light", "dark"][index]
                }

                Flow {
                    x: 8
                    width: parent.width - 16
                    spacing: 10
                    topPadding: 6
                    bottomPadding: 10

                    Repeater {
                        model: Theme.themeOptions(Theme.dark)

                        delegate: ThemeSwatch {
                            required property themeOption modelData
                            option: modelData
                            selected: modelData.id === Settings.themeSource
                            onClicked: Settings.themeSource = modelData.id
                        }
                    }
                }

                Item {
                    width: parent.width
                    height: path.implicitHeight + automatic.implicitHeight + 24
                    visible: Settings.themeSource === "system"

                    OutlinedTextField {
                        id: path
                        x: 8
                        y: 8
                        width: parent.width - 16
                        placeholder: "Automatic (optional JSON file path)"
                        text: Settings.matugenColorsPath
                        onAccepted: Settings.matugenColorsPath = text
                    }
                    TextButton {
                        id: automatic
                        x: 8
                        y: path.y + path.height + 8
                        variant: TextButton.Tonal
                        symbol: "refresh"
                        text: "Auto-detect"
                        onClicked: {
                            path.text = ""
                            Settings.matugenColorsPath = ""
                            Theme.refreshSystemScheme()
                        }
                    }
                    Connections {
                        target: path.inputItem
                        function onActiveFocusChanged() {
                            if (!path.inputItem.activeFocus) {
                                Settings.matugenColorsPath = path.text
                            }
                        }
                    }
                }
                NoteText {
                    text: Settings.themeSource !== "system"
                        ? "these are hardcoded themes"
                        : (Theme.usingSystemScheme
                            ? "following " + Theme.systemSchemePath + "; updates when the file changes."
                            : Settings.matugenColorsPath.length > 0
                              ? "no valid Matugen scheme found at " + Theme.systemSchemePath
                              : "automatically looks for Matugen JSON in common config and cache folders. an optional path overrides discovery.")
                }
            }

            ContentSection {
                // a mac window keeps its traffic lights, and has no tray
                visible: !App.macOS
                title: "Window"
                symbol: "web_asset"

                ConfigSelection {
                    objectName: "windowControlsMode"
                    visible: App.windows
                    text: "Window controls"
                    currentIndex: Settings.nativeTitleBar ? 0 : 1
                    segments: [
                        Segment { text: "Native title bar" },
                        Segment { text: "In-app" }
                    ]
                    onActivated: (index) => Settings.nativeTitleBar = index === 0
                }
                ConfigSwitch {
                    text: "Window buttons"
                    visible: !App.windows
                    checked: Settings.showWindowControls
                    onToggledTo: (checked) => Settings.showWindowControls = checked
                }
                ConfigSwitch {
                    text: App.windows ? "Always show tray icon" : "Tray icon"
                    checked: Settings.trayIcon
                    onToggledTo: (checked) => Settings.trayIcon = checked
                }
                NoteText {
                    text: App.windows
                        ? "closing the window keeps Swifty running in the tray. choose Quit Swifty from the tray menu to exit."
                        : "window buttons are for desktops that draw none of their own, Hyprland closes and moves windows itself"
                }
            }
        }

        Page {
            visible: root.page === 5

            ContentSection {
                title: "Motion"
                symbol: "animation"

                ConfigSwitch {
                    text: "Reduce motion"
                    checked: Settings.reduceMotion || Settings.performanceMode
                    enabled: !Settings.performanceMode
                    onToggledTo: (checked) => Settings.reduceMotion = checked
                }
                ConfigSwitch {
                    text: "Performance mode"
                    checked: Settings.performanceMode
                    onToggledTo: (checked) => Settings.performanceMode = checked
                }
                NoteText {
                    text: "reduced motion stops anything from constantly animating on its own, interactions still animate tho. "
                        + "performance mode goes way further, NOTHING animates! drastically improves performance but looks ugly imo"
                }
            }

            ContentSection {
                title: "Shapes"
                symbol: "category"

                ConfigSwitch {
                    text: "No shapes"
                    checked: Settings.noShapes
                    onToggledTo: (checked) => Settings.noShapes = checked
                }
                NoteText {
                    text: "less shapes if you prefer minimalistic look"
                }
            }
        }

        // the logins: who is signed in, the library's and playback's state,
        // connecting them again, and logging out
        Page {
            visible: root.page === 6

            ContentSection {
                title: "Debugging"
                symbol: "bug_report"

                ConfigSwitch {
                    text: "Debugging shortcuts"
                    checked: Settings.debuggingEnabled
                    onToggledTo: (checked) => Settings.debuggingEnabled = checked
                }
                NoteText {
                    text: "Adds a seconds dial to the sleep timer, so a fade can be watched without waiting out a long ass timer"
                }

                Flow {
                    x: 8
                    width: parent.width - 16
                    spacing: 8
                    topPadding: 4

                    TextButton {
                        variant: TextButton.Tonal
                        symbol: "hourglass_empty"
                        text: "Play Loading Test"
                        onClicked: Player.playLoadingTest()
                    }
                }
                NoteText {
                    text: "plays a song that never finishes loading, so the visualizers loading animation can be watched"
                }
            }

            ContentSection {
                id: about

                property bool revealed: false

                title: "About"
                symbol: "info"

                Row {
                    x: 12
                    width: parent.width - 24
                    topPadding: 8
                    bottomPadding: 8
                    spacing: 16

                    // the app's icon
                    Item {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 72
                        height: 72

                        Image {
                            anchors.centerIn: parent
                            width: 46
                            height: 46
                            source: "qrc:/qt/qml/Swifty/resources/swifty-icon.png"
                            sourceSize: Qt.size(96, 96)
                            smooth: true
                            mipmap: true
                        }
                    }

                    Column {
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 2

                        StyledText {
                            text: "Swifty " + App.version
                            font.weight: Font.Medium

                            TapHandler {
                                onTapped: about.revealed = true
                            }
                            HoverHandler {
                                cursorShape: Qt.PointingHandCursor
                            }
                        }
                        StyledText {
                            text: "developed by addenator, forked by ImAlekzzz"
                            font.pixelSize: Appearance.fontSize.smaller
                            color: Appearance.colors.onSurfaceVariant
                        }
                        StyledText {
                            topPadding: 8
                            visible: opacity > 0
                            opacity: about.revealed ? 1 : 0
                            scale: about.revealed ? 1 : 0.92
                            transformOrigin: Item.Left
                            text: "no secret dev options here..."
                            font.weight: Font.DemiBold
                            color: Appearance.colors.primary

                            Behavior on opacity {
                                NumberAnimation { duration: Appearance.duration(250); easing.type: Easing.OutCubic }
                            }
                            Behavior on scale {
                                ElementMoveSmall {}
                            }
                        }
                    }
                }
            }
        }
    }
}
