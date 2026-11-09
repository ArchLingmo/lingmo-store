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

// What an app can reach, shown before it's installed: the Flathub sandbox's
// permissions in plain words, or a clear note that Arch packages have no sandbox
LingmoUI.Card {
    id: control

    property string source: ""      // "arch" or "flathub"
    property var app: ({})
    property var info: ({})         // permissions.forApp() for Flathub apps
    property bool showAll: false
    property string title: qsTr("What this app can access")

    readonly property string repo: app.arch ? (app.arch.repo || "") : ""
    readonly property bool officialRepo: ["core", "extra", "multilib", "core-testing", "extra-testing",
                                          "multilib-testing"].indexOf(repo) >= 0
    // Curated apps are the developers' own packages, mirrored: not built by the team
    readonly property bool mirrored: !!app.curated
    readonly property string level: source === "arch" ? ((officialRepo || repo === "archlingmo") && !mirrored ? "info" : "medium")
                                                       : (info.level || "")
    readonly property var items: info.items || []
    readonly property int importantCount: items.filter(i => i.level !== "low").length
    readonly property var shownItems: showAll ? items : items.filter(i => i.level !== "low")

    function levelColor(l) {
        switch (l) {
        case "high": return LingmoUI.Theme.errorColor
        case "medium": return LingmoUI.Theme.warningColor
        case "low": return LingmoUI.Theme.successColor
        default: return LingmoUI.Theme.infoColor
        }
    }
    function levelName(l) {
        switch (l) {
        case "high": return qsTr("High risk")
        case "medium": return qsTr("Sensitive")
        default: return qsTr("Low risk")
        }
    }
    implicitHeight: controlContent.implicitHeight + padding * 2

    ColumnLayout {
        id: controlContent
        anchors.fill: parent
        anchors.margins: control.padding
        spacing: LingmoUI.Units.largeSpacing

        Label {
            text: control.title
            font.bold: true
            font.pointSize: LingmoUI.Units.fontSubheading
            color: LingmoUI.Theme.textColor
        }

        // Verdict
        LingmoUI.Banner {
            visible: control.source === "arch" || control.info.state === "ready"
            Layout.fillWidth: true
            closable: false
            type: control.level === "high" ? LingmoUI.Banner.Error
                : control.level === "medium" ? LingmoUI.Banner.Warning
                : control.level === "low" ? LingmoUI.Banner.Success : LingmoUI.Banner.Info
            iconName: control.source === "arch" ? "dialog-information"
                    : control.level === "high" ? "security-low"
                    : control.level === "medium" ? "security-medium" : "security-high"
            title: control.source === "arch" ? qsTr("Runs without a sandbox") : (control.info.title || "")
            text: {
                if (control.source !== "arch")
                    return control.info.text || ""
                if (control.officialRepo)
                    return qsTr("Programs from the Arch repositories run directly on the system, with the same access you have to your files and devices. This one comes from the official Arch Linux repositories (%1), built and signed by its maintainers.").arg(control.repo)
                if (control.mirrored && control.repo.indexOf("archlingmo") === 0)
                    return qsTr("Programs from the Arch repositories run directly on the system, with the same access you have to your files and devices. This one is the developer's own package (%1), copied unchanged into an ArchLingmo repository (%2) after its checksum and contents were checked; it was not built by the ArchLingmo team.").arg(control.app.developer || "").arg(control.repo)
                if (control.repo === "archlingmo")
                    return qsTr("Programs from the Arch repositories run directly on the system, with the same access you have to your files and devices. This one comes from the ArchLingmo repository, built and signed by the ArchLingmo team.")
                return qsTr("Programs from the Arch repositories run directly on the system, with the same access you have to your files and devices. This one comes from the “%1” repository, which is not an official Arch Linux repository.").arg(control.repo || qsTr("local"))
            }
        }

        // Loading / unavailable
        RowLayout {
            visible: control.source === "flathub" && (control.info.state === "loading" || control.info.state === "none"
                                                      || control.info.state === undefined)
            spacing: LingmoUI.Units.largeSpacing
            LingmoUI.Spinner {
                running: parent.visible
            }
            Label {
                text: qsTr("Checking the permissions…")
                color: LingmoUI.Theme.secondaryTextColor
            }
        }
        Label {
            visible: control.source === "flathub" && (control.info.state === "offline" || control.info.state === "error")
            text: control.info.state === "offline"
                  ? qsTr("The permissions can't be checked without internet. They will show up here once you're back online.")
                  : qsTr("Could not read this app's permissions right now.")
            color: LingmoUI.Theme.secondaryTextColor
            wrapMode: Text.Wrap
            Layout.fillWidth: true
        }

        // The list
        Repeater {
            model: control.source === "flathub" && control.info.state === "ready" ? control.shownItems : []

            delegate: RowLayout {
                Layout.fillWidth: true
                spacing: LingmoUI.Units.largeSpacing

                Rectangle {
                    Layout.preferredWidth: 36
                    Layout.preferredHeight: 36
                    Layout.alignment: Qt.AlignTop
                    radius: LingmoUI.Theme.smallRadius
                    readonly property color tint: control.levelColor(modelData.level)
                    color: Qt.rgba(tint.r, tint.g, tint.b, LingmoUI.Theme.darkMode ? 0.2 : 0.12)

                    LingmoUI.IconItem {
                        anchors.centerIn: parent
                        width: LingmoUI.Units.iconSizes.smallMedium
                        height: width
                        source: modelData.icon
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 1

                    Label {
                        text: modelData.title
                        font.bold: modelData.level !== "low"
                        color: LingmoUI.Theme.textColor
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                    }
                    Label {
                        text: modelData.detail
                        color: LingmoUI.Theme.secondaryTextColor
                        wrapMode: Text.Wrap
                        font.pointSize: LingmoUI.Units.fontCaption
                        Layout.fillWidth: true
                    }
                }
                LingmoUI.Badge {
                    visible: modelData.level !== "low"
                    Layout.alignment: Qt.AlignTop
                    text: control.levelName(modelData.level)
                    tone: control.levelColor(modelData.level)
                }
            }
        }

        Label {
            readonly property int hidden: control.items.length - control.importantCount
            visible: control.source === "flathub" && control.info.state === "ready" && hidden > 0
            text: control.showAll ? qsTr("Show only the important ones")
                                  : qsTr("Show %n more permission(s)", "", hidden)
            color: LingmoUI.Theme.highlightColor
            font.bold: true
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: control.showAll = !control.showAll
            }
        }

        Label {
            visible: control.source === "flathub" && !!control.info.stale
            text: qsTr("Checked earlier; it couldn't be checked again now.")
            color: LingmoUI.Theme.secondaryTextColor
            font.pointSize: LingmoUI.Units.fontCaption
            Layout.fillWidth: true
            wrapMode: Text.Wrap
        }
    }
}
