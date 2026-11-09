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

// A Windows game from Lutris in the search results: cover, name, year and how it runs
LingmoUI.Card {
    id: control

    property var game: ({})
    signal openRequested(string slug)

    implicitHeight: 104
    radius: LingmoUI.Theme.bigRadius
    clickable: true
    liftOnHover: true
    onClicked: control.openRequested(control.game.slug)

    RowLayout {
        anchors.fill: parent
        anchors.margins: LingmoUI.Units.largeSpacing
        spacing: LingmoUI.Units.largeSpacing

        LutrisCover {
            Layout.preferredWidth: 60
            Layout.preferredHeight: 80
            Layout.alignment: Qt.AlignVCenter
            source: control.game.cover || ""
            name: control.game.name || ""
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter
            spacing: 3

            Label {
                text: control.game.name || ""
                font.pointSize: LingmoUI.Units.fontBody
                font.bold: true
                color: LingmoUI.Theme.textColor
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
            Label {
                text: control.game.year ? qsTr("Windows game · %1").arg(control.game.year) : qsTr("Windows game")
                color: LingmoUI.Theme.secondaryTextColor
                font.pointSize: LingmoUI.Units.fontCaption
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
            Row {
                spacing: 4
                LingmoUI.Badge {
                    visible: control.game.status === "installed"
                    text: qsTr("Installed")
                    tone: LingmoUI.Theme.successColor
                }
                LingmoUI.Badge {
                    visible: control.game.status === "installing" || control.game.status === "lutris"
                    text: qsTr("Installing…")
                    tone: LingmoUI.Theme.highlightColor
                }
                LingmoUI.Badge {
                    text: qsTr("Runs with Wine")
                    tone: LingmoUI.Theme.warningColor
                }
                LingmoUI.Badge {
                    text: "Lutris"
                    subtle: true
                }
            }
        }
    }
}
