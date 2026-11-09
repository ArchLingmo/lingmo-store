/*
 * Copyright (C) 2026 LingmoOS Team.
 *
 * Author:     devalexandre <alexandre@dev2learn.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import LingmoUI.CompatibleModule 3.0 as LingmoUI

// A Windows game from Lutris: what it is, which installer, what Lutris (Flathub) can
// reach, then Install (Lutris first when it's missing) and Play
Flickable {
    id: page

    readonly property string pageName: "lutris"
    property string slug: ""
    // Main.qml compares this with the installer's key
    readonly property string appKey: "lutris:" + slug
    readonly property var game: { lutris.revision; return lutris.game(slug) }
    readonly property bool ready: game.state === "ready"
    readonly property var installers: game.installers || []
    property string chosen: ""
    readonly property string installerSlug: chosen !== "" ? chosen : (game.defaultInstaller || "")
    readonly property string status: game.status || ""
    readonly property var perms: { permissions.revision; return permissions.forApp("net.lutris.Lutris") }
    readonly property var antiCheat: { compat.blockedGames; return compat.antiCheatStatus(game.name || "") }
    readonly property bool lutrisBusy: status === "lutris" && installer.busy
    property string lastError: ""
    property bool descriptionExpanded: false
    readonly property int margin: LingmoUI.Units.largeSpacing * 2

    contentWidth: width
    contentHeight: column.implicitHeight + margin * 2
    clip: true
    boundsBehavior: Flickable.StopAtBounds
    ScrollBar.vertical: ScrollBar {}

    LingmoUI.WheelHandler {
        target: page
    }

    Component.onCompleted: {
        lutris.fetchGame(slug)
        permissions.fetch("net.lutris.Lutris", "app/net.lutris.Lutris/x86_64/stable", lutris.lutrisInstalled)
        compat.loadWatchlist()
    }

    Connections {
        target: lutris
        function onGameFinished(slug, name, ok, message) {
            if (slug === page.slug)
                page.lastError = ok ? "" : message
        }
    }

    function safetyText() {
        if (lutris.strictIsolation)
            return qsTr("Lutris with extra isolation")
        if (perms.state !== "ready")
            return ""
        return perms.level === "high" ? qsTr("Wide access, see below")
             : perms.level === "medium" ? qsTr("Isolated, some access") : qsTr("Isolated from the system")
    }
    function safetyColor() {
        if (lutris.strictIsolation)
            return LingmoUI.Theme.successColor
        return perms.level === "high" ? LingmoUI.Theme.errorColor
             : perms.level === "medium" ? LingmoUI.Theme.warningColor : LingmoUI.Theme.successColor
    }
    function shorten(text, max) {
        return text.length > max ? text.substring(0, max).replace(/\s+\S*$/, "") + "…" : text
    }

    ColumnLayout {
        id: column
        x: page.margin
        y: LingmoUI.Units.smallSpacing
        width: page.width - page.margin * 2
        spacing: LingmoUI.Units.largeSpacing * 1.5

        // Loading / not available
        LingmoUI.EmptyState {
            visible: !page.ready
            Layout.fillWidth: true
            Layout.topMargin: 80
            busy: page.game.state === "loading" || page.game.state === "none"
            iconName: "applications-games"
            title: page.game.state === "offline" ? qsTr("No internet connection")
                 : page.game.state === "missing" ? qsTr("This game was not found on Lutris")
                 : page.game.state === "busy" || page.game.state === "error" ? qsTr("Lutris did not answer. Try again in a moment.")
                 : qsTr("Loading the game…")
        }

        // Header: cover, name, action
        RowLayout {
            visible: page.ready
            Layout.fillWidth: true
            spacing: LingmoUI.Units.largeSpacing * 2

            LutrisCover {
                Layout.preferredWidth: 120
                Layout.preferredHeight: 160
                Layout.alignment: Qt.AlignTop
                radius: LingmoUI.Theme.bigRadius
                source: page.game.coverFile || ""
                name: page.game.name || ""
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignVCenter
                spacing: LingmoUI.Units.smallSpacing

                Label {
                    text: page.game.name || ""
                    font.pointSize: LingmoUI.Units.fontTitle
                    font.bold: true
                    color: LingmoUI.Theme.textColor
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }
                Label {
                    text: {
                        const parts = [qsTr("Windows game")]
                        if (page.game.year)
                            parts.push(page.game.year)
                        const genres = page.game.genres || []
                        if (genres.length > 0)
                            parts.push(genres.join(", "))
                        return parts.join(" · ")
                    }
                    color: LingmoUI.Theme.highlightColor
                    font.bold: true
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }
                Row {
                    spacing: 4
                    LingmoUI.Badge {
                        text: qsTr("Runs with Wine")
                        tone: LingmoUI.Theme.warningColor
                    }
                    LingmoUI.Badge {
                        visible: !!page.game.online
                        text: qsTr("Online")
                        tone: LingmoUI.Theme.infoColor
                    }
                    LingmoUI.Badge {
                        text: "Lutris"
                        subtle: true
                    }
                }
                Label {
                    text: qsTr("Installed and played with Lutris from Flathub, which runs Windows games through Wine. The install steps are written by the Lutris community, not by the game's publisher.")
                    color: LingmoUI.Theme.secondaryTextColor
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }
            }

            // Actions
            ColumnLayout {
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredWidth: 240
                spacing: LingmoUI.Units.smallSpacing

                // Installing Lutris itself
                ColumnLayout {
                    visible: page.status === "lutris"
                    spacing: LingmoUI.Units.smallSpacing
                    Layout.fillWidth: true

                    Label {
                        text: page.lutrisBusy ? qsTr("Installing Lutris…") : qsTr("Waiting to install Lutris…")
                        font.bold: true
                        color: LingmoUI.Theme.textColor
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                    Label {
                        visible: page.lutrisBusy
                        text: installer.statusText
                        color: LingmoUI.Theme.secondaryTextColor
                        font.pointSize: LingmoUI.Units.fontCaption
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                    ProgressBar {
                        Layout.fillWidth: true
                        indeterminate: !page.lutrisBusy || installer.progress < 0
                        value: Math.max(0, installer.progress)
                    }
                }

                // Lutris shows its own installer window
                ColumnLayout {
                    visible: page.status === "installing"
                    spacing: LingmoUI.Units.smallSpacing
                    Layout.fillWidth: true

                    RowLayout {
                        spacing: LingmoUI.Units.smallSpacing
                        Layout.alignment: Qt.AlignRight
                        LingmoUI.Spinner {
                            size: 18
                            running: page.status === "installing"
                        }
                        Label {
                            text: qsTr("Installing with Lutris…")
                            font.bold: true
                            color: LingmoUI.Theme.textColor
                        }
                    }
                    Label {
                        text: qsTr("Continue in the Lutris window. This page changes to Play when the game is ready.")
                        color: LingmoUI.Theme.secondaryTextColor
                        font.pointSize: LingmoUI.Units.fontCaption
                        horizontalAlignment: Text.AlignRight
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                    }
                    LingmoUI.ActionButton {
                        text: qsTr("Stop waiting")
                        subtle: true
                        Layout.alignment: Qt.AlignRight
                        onClicked: lutris.stopTracking(page.slug)
                    }
                }

                LingmoUI.ActionButton {
                    visible: page.ready && page.status === "installed"
                    primary: true
                    text: qsTr("Play")
                    Layout.alignment: Qt.AlignRight
                    implicitWidth: 150
                    onClicked: lutris.play(page.slug)
                }
                LingmoUI.ActionButton {
                    visible: page.ready && page.status === ""
                    primary: true
                    text: lutris.lutrisInstalled ? qsTr("Install") : qsTr("Install with Lutris")
                    Layout.alignment: Qt.AlignRight
                    implicitWidth: Math.max(150, contentItem ? contentItem.implicitWidth + LingmoUI.Units.largeSpacing * 4 : 150)
                    enabled: page.installerSlug !== "" && (lutris.lutrisInstalled || !installer.busy)
                    onClicked: {
                        page.lastError = ""
                        lutris.install(page.slug, page.installerSlug)
                    }
                }
                Label {
                    visible: page.ready && page.status === "" && !lutris.lutrisInstalled
                    text: qsTr("Lutris (from Flathub) is installed first.")
                    color: LingmoUI.Theme.secondaryTextColor
                    font.pointSize: LingmoUI.Units.fontCaption
                    horizontalAlignment: Text.AlignRight
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }
                Label {
                    visible: page.ready && page.status === "installed"
                    text: qsTr("Installed in Lutris")
                    color: LingmoUI.Theme.secondaryTextColor
                    font.pointSize: LingmoUI.Units.fontCaption
                    Layout.alignment: Qt.AlignRight
                }
                RowLayout {
                    visible: page.ready && page.safetyText() !== ""
                    Layout.alignment: Qt.AlignRight
                    spacing: 4

                    LingmoUI.IconItem {
                        source: "qrc:/icons/shield.svg"
                        color: page.safetyColor()
                        Layout.preferredWidth: 14
                        Layout.preferredHeight: 14
                    }
                    Label {
                        text: page.safetyText()
                        color: page.safetyColor()
                        font.pointSize: LingmoUI.Units.fontCaption
                        wrapMode: Text.Wrap
                        horizontalAlignment: Text.AlignRight
                        Layout.maximumWidth: 220
                    }
                }
            }
        }

        LingmoUI.Banner {
            visible: page.lastError !== "" && page.status === ""
            Layout.fillWidth: true
            type: LingmoUI.Banner.Error
            closable: false
            text: page.lastError
        }

        // Anti-cheat that keeps Linux out (Are We Anti-Cheat Yet)
        LingmoUI.Banner {
            visible: page.ready && !!page.antiCheat.blocked
            Layout.fillWidth: true
            type: LingmoUI.Banner.Error
            closable: false
            iconName: "dialog-error"
            title: qsTr("Online play is blocked on Linux")
            text: page.antiCheat.text || ""

            LingmoUI.ActionButton {
                text: qsTr("Details")
                onClicked: store.openUrl(page.antiCheat.url)
            }
        }

        // Online games may refuse Wine
        LingmoUI.Banner {
            visible: page.ready && !!page.game.online && !page.antiCheat.blocked
            Layout.fillWidth: true
            type: LingmoUI.Banner.Warning
            closable: false
            iconName: "dialog-warning"
            title: qsTr("Online game")
            text: qsTr("%1 is played online (%2). Online games may refuse to run under Wine, and some publishers suspend accounts that play with it. Check the game's rules before playing.")
                  .arg(page.game.name || "").arg(page.game.onlineGenre || "")
        }

        // Which installer
        ColumnLayout {
            visible: page.ready
            Layout.fillWidth: true
            spacing: LingmoUI.Units.largeSpacing

            Label {
                text: page.installers.length > 1 ? qsTr("Choose the installer") : qsTr("Installer")
                font.bold: true
                font.pointSize: LingmoUI.Units.fontSubheading
                color: LingmoUI.Theme.textColor
            }
            Label {
                visible: page.installers.length === 0
                text: qsTr("Lutris has no Windows installer for this game yet.")
                color: LingmoUI.Theme.secondaryTextColor
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }

            GridLayout {
                id: installerGrid
                Layout.fillWidth: true
                columns: width > 700 ? 2 : 1
                columnSpacing: LingmoUI.Units.largeSpacing
                rowSpacing: LingmoUI.Units.largeSpacing

                Repeater {
                    model: page.installers

                    SourceOption {
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        title: modelData.name
                        text: {
                            const lines = []
                            const notes = modelData.notes || modelData.description || ""
                            if (notes !== "")
                                lines.push(page.shorten(notes, 160))
                            lines.push(modelData.updated ? qsTr("By %1 · updated %2").arg(modelData.user).arg(modelData.updated)
                                                         : qsTr("By %1").arg(modelData.user))
                            return lines.join("\n")
                        }
                        selected: page.installerSlug === modelData.slug
                        recommended: page.installers.length > 1 && page.game.defaultInstaller === modelData.slug
                        enabled: page.status === ""
                        onClicked: page.chosen = modelData.slug
                    }
                }
            }
        }

        // What Lutris (Flathub) can reach, and what the store takes away
        PermissionsCard {
            visible: page.ready
            Layout.fillWidth: true
            source: "flathub"
            title: qsTr("What Lutris can access")
            info: page.perms
        }

        LingmoUI.Card {
            id: isolation
            visible: page.ready
            Layout.fillWidth: true
            implicitHeight: isolationLayout.implicitHeight + padding * 2

            RowLayout {
                id: isolationLayout
                anchors.fill: parent
                anchors.margins: isolation.padding
                spacing: LingmoUI.Units.largeSpacing

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: LingmoUI.Units.smallSpacing

                    Label {
                        text: qsTr("Extra isolation for Lutris (recommended)")
                        font.bold: true
                        font.pointSize: LingmoUI.Units.fontSubheading
                        color: LingmoUI.Theme.textColor
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                    }
                    Label {
                        text: qsTr("As published on Flathub, Lutris can read and change all your files and start programs outside its sandbox. With this on, the store limits it to the %1 folder and to Downloads (read only), and stops it from starting programs outside the sandbox. Games keep the internet, sound, graphics and controllers. Turn it off only if a game needs another folder.")
                              .arg("~/Games/Lutris")
                        color: LingmoUI.Theme.secondaryTextColor
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                    }
                }
                Switch {
                    Layout.alignment: Qt.AlignTop
                    checked: lutris.strictIsolation
                    onToggled: lutris.strictIsolation = checked
                }
            }
        }

        // About the game
        LingmoUI.Card {
            id: about
            visible: page.ready && (page.game.description || "") !== ""
            Layout.fillWidth: true
            implicitHeight: aboutLayout.implicitHeight + padding * 2

            ColumnLayout {
                id: aboutLayout
                anchors.fill: parent
                anchors.margins: about.padding
                spacing: LingmoUI.Units.largeSpacing

                Label {
                    text: qsTr("About this game")
                    font.bold: true
                    font.pointSize: LingmoUI.Units.fontSubheading
                    color: LingmoUI.Theme.textColor
                }
                Label {
                    text: page.descriptionExpanded ? (page.game.description || "") : page.shorten(page.game.description || "", 600)
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    color: LingmoUI.Theme.textColor
                    lineHeight: 1.15
                    Layout.fillWidth: true
                }
                Label {
                    visible: (page.game.description || "").length > 600
                    text: page.descriptionExpanded ? qsTr("Show less") : qsTr("Show more")
                    color: LingmoUI.Theme.highlightColor
                    font.bold: true
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: page.descriptionExpanded = !page.descriptionExpanded
                    }
                }
                Label {
                    text: qsTr("See it on lutris.net")
                    color: LingmoUI.Theme.highlightColor
                    font.bold: true
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: store.openUrl(page.game.lutrisUrl)
                    }
                }
            }
        }
    }
}
