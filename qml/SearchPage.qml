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

// Search results: the catalog's apps, then Windows games from Lutris
Flickable {
    id: page

    readonly property string pageName: "search"
    property string query: ""
    readonly property var results: { rootWindow.rev; return catalog.search(query, 80) }
    readonly property var games: { lutris.revision; return lutris.results(query, results) }
    readonly property string gamesState: { lutris.revision; return lutris.searchState(query) }
    readonly property bool gamesLoading: gamesState === "loading"
    readonly property int margin: LingmoUI.Units.largeSpacing * 2

    onQueryChanged: lutris.search(query)
    Component.onCompleted: lutris.search(query)

    contentWidth: width
    contentHeight: column.implicitHeight + margin
    clip: true
    boundsBehavior: Flickable.StopAtBounds
    ScrollBar.vertical: ScrollBar {}

    LingmoUI.WheelHandler {
        target: page
    }

    ColumnLayout {
        id: column
        x: page.margin
        y: LingmoUI.Units.smallSpacing
        width: page.width - page.margin * 2
        spacing: LingmoUI.Units.largeSpacing

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 0

            Label {
                text: qsTr("Results for “%1”").arg(page.query)
                font.pointSize: LingmoUI.Units.fontTitle
                font.bold: true
                color: LingmoUI.Theme.textColor
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
            Label {
                visible: page.results.length > 0
                text: qsTr("%n app(s) found", "", page.results.length)
                color: LingmoUI.Theme.secondaryTextColor
            }
        }

        AppGrid {
            Layout.fillWidth: true
            visible: page.results.length > 0
            apps: page.results
            onAppClicked: (key) => rootWindow.openApp(key)
        }

        // Nothing in the catalog
        LingmoUI.EmptyState {
            visible: page.results.length === 0 && (page.games.length === 0 || !catalog.ready)
            Layout.fillWidth: true
            Layout.topMargin: 40
            Layout.bottomMargin: 20
            busy: !catalog.ready
            iconName: "edit-find"
            title: catalog.ready ? qsTr("No apps found") : qsTr("Loading the app catalog…")
            text: catalog.ready ? qsTr("Check the spelling or try another word, like “music” or “photos”.") : ""
        }

        // Windows games from Lutris
        SectionHeader {
            visible: page.games.length > 0 || page.gamesLoading
            text: qsTr("Windows games (Lutris)")
            subtitle: qsTr("Installed and played with Lutris from Flathub, through Wine.")
        }

        RowLayout {
            visible: page.gamesLoading && page.games.length === 0
            spacing: LingmoUI.Units.largeSpacing
            LingmoUI.Spinner {
                running: parent.visible
            }
            Label {
                text: qsTr("Looking for Windows games on Lutris…")
                color: LingmoUI.Theme.secondaryTextColor
            }
        }

        GridLayout {
            id: gamesGrid
            visible: page.games.length > 0
            Layout.fillWidth: true
            readonly property int minimumCardWidth: 290
            columns: Math.max(1, Math.floor((width + columnSpacing) / (minimumCardWidth + columnSpacing)))
            columnSpacing: LingmoUI.Units.largeSpacing
            rowSpacing: LingmoUI.Units.largeSpacing

            Repeater {
                model: page.games

                LutrisGameCard {
                    game: modelData
                    Layout.fillWidth: true
                    Layout.preferredWidth: gamesGrid.minimumCardWidth
                    onOpenRequested: (slug) => rootWindow.openLutrisGame(slug)
                }
            }
            // Empty cells: one or two games keep the width of the app cards above
            Repeater {
                model: Math.max(0, gamesGrid.columns - page.games.length)
                Item {
                    Layout.fillWidth: true
                    Layout.preferredWidth: gamesGrid.minimumCardWidth
                    implicitHeight: 1
                }
            }
        }

        Label {
            visible: page.results.length > 0 && page.games.length === 0
                     && (page.gamesState === "offline" || page.gamesState === "busy" || page.gamesState === "error")
            text: page.gamesState === "offline" ? qsTr("Windows games from Lutris can't be searched without internet.")
                                                : qsTr("Lutris did not answer. Try again in a moment.")
            color: LingmoUI.Theme.secondaryTextColor
            font.pointSize: LingmoUI.Units.fontCaption
            wrapMode: Text.Wrap
            Layout.fillWidth: true
        }
    }
}
