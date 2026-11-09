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
import Qt5Compat.GraphicalEffects
import LingmoUI.CompatibleModule 3.0 as LingmoUI

// A game's cover (cached by the store), or its initial while there is none
Item {
    id: control

    property string source: ""
    property string name: ""
    property real radius: LingmoUI.Theme.smallRadius

    Rectangle {
        id: frame
        anchors.fill: parent
        radius: control.radius
        color: LingmoUI.Theme.darkMode ? Qt.rgba(1, 1, 1, 0.08) : Qt.rgba(0, 0, 0, 0.06)
        layer.enabled: true
        layer.effect: OpacityMask {
            maskSource: Rectangle {
                width: frame.width
                height: frame.height
                radius: frame.radius
            }
        }

        Image {
            id: cover
            anchors.fill: parent
            source: control.source
            fillMode: Image.PreserveAspectCrop
            asynchronous: true
            smooth: true
            mipmap: true
            sourceSize.width: width * 2
        }

        Label {
            anchors.centerIn: parent
            visible: cover.status !== Image.Ready
            text: control.name.length > 0 ? control.name.charAt(0).toUpperCase() : "?"
            font.pointSize: Math.max(10, control.height / 4)
            font.bold: true
            color: LingmoUI.Theme.secondaryTextColor
        }
    }
}
