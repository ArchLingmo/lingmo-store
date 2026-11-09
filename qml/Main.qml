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
import Lingmo.Store

LingmoUI.Window {
    id: rootWindow

    width: 1180
    height: 760
    minimumWidth: 920
    minimumHeight: 600
    visible: false
    title: qsTr("App Store")

    background.opacity: LingmoUI.Theme.darkMode ? 0.9 : 0.98
    header.height: 50
    contentTopMargin: 0

    // Bumped whenever the catalog or what's installed changes: pages re-query
    property int rev: 0
    property string pendingApp: ""
    property string pendingSearch: ""
    property bool pendingGames: false
    property string consentKey: ""   // Flathub app waiting for the user to enable Flathub
    property var repoConsent: null   // {repo, key, packages, name}: waiting for a repository to be enabled
    property string removeKey: ""
    property string removeSource: ""
    property string removePackageName: ""   // a native game package instead of an app
    // Closed while installing: stay in the background until it's done
    property bool quitWhenDone: false

    onClosing: (close) => {
        if (installer.busy) {
            close.accepted = false
            quitWhenDone = true
            rootWindow.hide()
            return
        }
        Qt.quit()
    }
    onVisibleChanged: if (visible) quitWhenDone = false

    LingmoUI.WindowBlur {
        view: rootWindow
        geometry: Qt.rect(rootWindow.x, rootWindow.y, rootWindow.width, rootWindow.height)
        windowRadius: rootWindow.windowRadius
        enabled: true
    }

    Connections {
        target: catalog
        function onChanged() {
            rootWindow.rev++
            rootWindow.processPending()
        }
        function onInstalledChanged() { rootWindow.rev++ }
    }

    Connections {
        target: flatpak
        function onStateChanged() {
            rootWindow.rev++
            if (flatpak.state === Flatpak.Ready && rootWindow.consentKey !== "") {
                installer.install(rootWindow.consentKey, "flathub")
                rootWindow.consentKey = ""
            }
        }
        function onRemoteAdded(ok, error) {
            if (!ok)
                rootWindow.showPassiveNotification(error, 6000)
        }
    }

    Connections {
        target: installer
        function onFlathubConsentNeeded(key) {
            rootWindow.consentKey = key
            consentDialog.open()
        }
        function onRepoConsentNeeded(repo, key, packages, name) {
            rootWindow.repoConsent = { repo: repo, key: key, packages: packages, name: name }
            repoConsentDialog.open()
        }
        function onFinished(key, source, action, ok, message) {
            rootWindow.rev++
            if (rootWindow.quitWhenDone) {
                // Give the notification a moment to go out
                quitTimer.start()
                return
            }
            const top = stack.currentItem
            const shownThere = top && ((top.appKey !== undefined && top.appKey === key)
                                       || (action === "gaming" && top.pageName === "games"))
            if (!shownThere) {
                if (!ok && message !== "")
                    rootWindow.showPassiveNotification(message, 6000)
                else if (ok && action === "install")
                    rootWindow.showPassiveNotification(qsTr("%1 is installed").arg(installer.currentName), 4000)
            }
        }
    }

    Connections {
        target: lutris
        function onGameFinished(slug, name, ok, message) {
            const top = stack.currentItem
            if (top && top.pageName === "lutris" && top.slug === slug)
                return
            if (ok)
                rootWindow.showPassiveNotification(qsTr("%1 is installed").arg(name), 4000)
            else if (message !== "")
                rootWindow.showPassiveNotification(message, 6000)
        }
    }

    Connections {
        target: store
        function onAppRequested(query) {
            rootWindow.pendingApp = query
            rootWindow.processPending()
        }
        function onSearchRequested(text) {
            rootWindow.pendingApp = ""
            rootWindow.showSearch(text)
        }
        function onGamesRequested() {
            rootWindow.showGames()
        }
    }

    function processPending() {
        if (!catalog.ready || pendingApp === "")
            return
        const query = pendingApp
        pendingApp = ""
        const key = catalog.resolve(query)
        if (key !== "") {
            openApp(key)
        } else {
            // Unknown here: search for its name instead
            let text = query.replace(/^appstream:\/*/i, "").replace(/^app\//, "").split("/")[0]
            const parts = text.split(".")
            if (parts.length > 2)
                text = parts[parts.length - 1]
            showSearch(text)
        }
    }

    function navigate(name) {
        if (name === "updates") {
            store.openUpdater()
            showPassiveNotification(qsTr("Updates are installed by the Updates app."), 3000)
            return
        }
        searchField.clearSilently()
        stack.clear()
        if (name === "home")
            stack.push("qrc:/qml/HomePage.qml")
        else if (name === "games")
            stack.push("qrc:/qml/GamesPage.qml")
        else if (name === "installed")
            stack.push("qrc:/qml/InstalledPage.qml")
        else if (name === "native")
            stack.push("qrc:/qml/NativeGamesPage.qml")
        else {
            const info = sideBar.categoryInfo(name)
            stack.push("qrc:/qml/CategoryPage.qml", { category: name, title: info ? info.title : name,
                                                      tint: info ? info.iconColor : LingmoUI.Theme.highlightColor })
        }
    }

    function showHome() {
        sideBar.select("home")
        navigate("home")
    }

    function showGames() {
        sideBar.select("games")
        navigate("games")
    }

    function showNative() {
        sideBar.select("native")
        navigate("native")
    }

    // "You need your own copy of the game"; with a package, installs it after
    property var ownCopyGame: null
    function showOwnCopy(game, install) {
        ownCopyGame = install ? game : null
        ownCopyDialog.showReject = install
        ownCopyDialog.acceptText = install ? qsTr("Install") : qsTr("OK")
        ownCopyDialog.open()
    }

    function installNative(game) {
        if (game.needsOwnCopy)
            showOwnCopy(game, true)
        else
            installer.installPackage(game.pkg, game.name)
    }

    // A collection's bundle (e.g. the Craft office suite): its missing packages in one go
    function installBundle(bundle) {
        if (!bundle || !bundle.available || bundle.packages.length === 0)
            return
        installer.installPackages(bundle.packages, bundle.name)
    }

    function showCategory(name) {
        sideBar.select(name)
        navigate(name)
    }

    // A category page on top of the current one (e.g. all games from the games page)
    function pushCategory(name) {
        const info = sideBar.categoryInfo(name)
        stack.push("qrc:/qml/CategoryPage.qml", { category: name, title: info ? info.title : name,
                                                  tint: info ? info.iconColor : LingmoUI.Theme.highlightColor })
    }

    function openApp(key) {
        const top = stack.currentItem
        if (top && top.appKey === key)
            return
        stack.push("qrc:/qml/AppPage.qml", { appKey: key })
    }

    // A Windows game from Lutris (search results)
    function openLutrisGame(slug) {
        const top = stack.currentItem
        if (top && top.pageName === "lutris" && top.slug === slug)
            return
        stack.push("qrc:/qml/LutrisGamePage.qml", { slug: slug })
    }

    function showSearch(text) {
        searchField.text = text
        searchField.forceActiveFocus()
        runSearch()
    }

    function runSearch() {
        const text = searchField.text.trim()
        const top = stack.currentItem
        const onSearch = top && top.pageName === "search"
        if (text === "") {
            if (onSearch)
                stack.pop()
            return
        }
        if (onSearch)
            top.query = text
        else
            stack.push("qrc:/qml/SearchPage.qml", { query: text })
    }

    function askRemovePackage(pkg, name) {
        removePackageName = pkg
        removeKey = name
        removeDialog.title = qsTr("Remove %1?").arg(name)
        removeDialog.open()
    }

    function askRemove(key, source, name) {
        removePackageName = ""
        removeKey = key
        removeSource = source
        removeDialog.title = qsTr("Remove %1?").arg(name)
        removeDialog.open()
    }

    headerItem: Item {
        id: headerBar

        property real originX: 0
        function updateOrigin() { originX = mapToItem(null, 0, 0).x }
        Component.onCompleted: Qt.callLater(updateOrigin)
        onWidthChanged: Qt.callLater(updateOrigin)

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: Math.max(0, sideBar.width - headerBar.originX) + LingmoUI.Units.largeSpacing * 1.5
            anchors.rightMargin: LingmoUI.Units.largeSpacing * 2
            spacing: LingmoUI.Units.largeSpacing

            // Back
            LingmoUI.IconButton {
                visible: stack.depth > 1
                iconName: "qrc:/icons/back.svg"
                size: 32
                iconSize: 18
                round: true
                onClicked: {
                    if (stack.currentItem && stack.currentItem.pageName === "search")
                        searchField.clearSilently()
                    stack.pop()
                }
            }

            Item { Layout.fillWidth: true }

            TextField {
                id: searchField
                Layout.preferredWidth: Math.min(460, headerBar.width * 0.5)
                Layout.preferredHeight: LingmoUI.Units.controlHeight
                placeholderText: qsTr("Search apps")
                leftPadding: 36
                rightPadding: 30
                selectByMouse: true
                color: LingmoUI.Theme.textColor
                placeholderTextColor: LingmoUI.Theme.secondaryTextColor
                font.pointSize: LingmoUI.Units.fontBody

                property bool silent: false
                function clearSilently() {
                    silent = true
                    text = ""
                    silent = false
                }

                background: Rectangle {
                    radius: height / 2
                    color: LingmoUI.Theme.darkMode ? Qt.rgba(1, 1, 1, 0.08) : Qt.rgba(0, 0, 0, 0.05)
                    border.width: searchField.activeFocus ? 2 : 0
                    border.color: LingmoUI.Theme.highlightColor
                    Behavior on color { ColorAnimation { duration: LingmoUI.Units.veryShortDuration } }

                    Image {
                        anchors.left: parent.left
                        anchors.leftMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        source: "qrc:/icons/search.svg"
                        width: 16
                        height: 16
                        sourceSize: Qt.size(32, 32)
                    }
                }

                Label {
                    anchors.right: parent.right
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    visible: searchField.text !== ""
                    text: "✕"
                    color: LingmoUI.Theme.secondaryTextColor

                    MouseArea {
                        anchors.fill: parent
                        anchors.margins: -6
                        cursorShape: Qt.PointingHandCursor
                        onClicked: searchField.text = ""
                    }
                }

                onTextChanged: if (!silent) searchTimer.restart()
                Keys.onEscapePressed: text = ""
                Keys.onReturnPressed: {
                    searchTimer.stop()
                    rootWindow.runSearch()
                }
            }

            Item { Layout.fillWidth: true }

            LingmoUI.Badge {
                visible: store.fakeMode
                text: qsTr("Test mode")
                tone: LingmoUI.Theme.warningColor
            }

            Item {
                Layout.preferredWidth: 32
                visible: !store.fakeMode
            }
        }

        Timer {
            id: searchTimer
            interval: 150
            onTriggered: rootWindow.runSearch()
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        LingmoUI.SettingsSideBar {
            id: sideBar
            Layout.fillHeight: true

            property string currentName: "home"

            function select(name) {
                for (let i = 0; i < model.count; ++i) {
                    if (model.get(i).name === name) {
                        currentIndex = i
                        currentName = name
                        return
                    }
                }
                currentIndex = -1
                currentName = name
            }

            // Categories and their tile colors, also used by the home page
            function categoryInfo(name) {
                for (let i = 0; i < model.count; ++i) {
                    if (model.get(i).name === name)
                        return model.get(i)
                }
                return null
            }

            onItemClicked: (index) => {
                const name = model.get(index).name
                // Updates opens another app: keep the current page selected
                if (name === "updates")
                    select(currentName)
                else
                    currentName = name
                rootWindow.navigate(name)
            }

            model: ListModel {
                ListElement { title: qsTr("Home"); name: "home"; iconSource: "qrc:/icons/home.svg"; iconColor: "#0A84FF"; section: ""; trailing: "" }
                ListElement { title: qsTr("Games"); name: "games"; iconSource: "qrc:/icons/games.svg"; iconColor: "#BF5AF2"; section: ""; trailing: "" }
                ListElement { title: qsTr("Native games"); name: "native"; iconSource: "qrc:/icons/native.svg"; iconColor: "#E0457B"; section: ""; trailing: "" }
                ListElement { title: qsTr("Productivity"); name: "office"; iconSource: "qrc:/icons/office.svg"; iconColor: "#FF9500"; section: qsTr("Categories"); trailing: "" }
                ListElement { title: qsTr("Internet"); name: "internet"; iconSource: "qrc:/icons/internet.svg"; iconColor: "#32ADE6"; section: qsTr("Categories"); trailing: "" }
                ListElement { title: qsTr("Graphics & Photo"); name: "graphics"; iconSource: "qrc:/icons/graphics.svg"; iconColor: "#FF375F"; section: qsTr("Categories"); trailing: "" }
                ListElement { title: qsTr("Engineering & CAD"); name: "engineering"; iconSource: "qrc:/icons/engineering.svg"; iconColor: "#64D2FF"; section: qsTr("Categories"); trailing: "" }
                ListElement { title: qsTr("Audio & Video"); name: "multimedia"; iconSource: "qrc:/icons/multimedia.svg"; iconColor: "#FF6B3A"; section: qsTr("Categories"); trailing: "" }
                ListElement { title: qsTr("Development"); name: "development"; iconSource: "qrc:/icons/development.svg"; iconColor: "#5E5CE6"; section: qsTr("Categories"); trailing: "" }
                ListElement { title: qsTr("Education"); name: "education"; iconSource: "qrc:/icons/education.svg"; iconColor: "#30B0C7"; section: qsTr("Categories"); trailing: "" }
                ListElement { title: qsTr("Utilities"); name: "utilities"; iconSource: "qrc:/icons/utilities.svg"; iconColor: "#8E8E93"; section: qsTr("Categories"); trailing: "" }
                ListElement { title: qsTr("Installed"); name: "installed"; iconSource: "qrc:/icons/installed.svg"; iconColor: "#34C759"; section: qsTr("Your computer"); trailing: "" }
                ListElement { title: qsTr("Updates"); name: "updates"; iconSource: "qrc:/icons/updates.svg"; iconColor: "#FF9F0A"; section: qsTr("Your computer"); trailing: "↗" }
            }

            // What's being installed right now, from any page
            footer: Component {
                LingmoUI.Card {
                    id: activity
                    visible: installer.busy
                    implicitHeight: visible ? activityLayout.implicitHeight + LingmoUI.Units.largeSpacing * 2 : 0
                    clickable: true
                    onClicked: {
                        if (installer.currentAction === "gaming")
                            rootWindow.showGames()
                        else if (installer.currentKey !== "")
                            rootWindow.openApp(installer.currentKey)
                    }

                    ColumnLayout {
                        id: activityLayout
                        anchors.fill: parent
                        anchors.margins: LingmoUI.Units.largeSpacing
                        spacing: LingmoUI.Units.smallSpacing

                        Label {
                            Layout.fillWidth: true
                            text: installer.currentAction === "remove" ? qsTr("Removing %1").arg(installer.currentName)
                                : installer.currentAction === "gaming" ? qsTr("Preparing for games")
                                : qsTr("Installing %1").arg(installer.currentName)
                            font.pointSize: LingmoUI.Units.fontBody
                            font.bold: true
                            elide: Text.ElideRight
                            color: LingmoUI.Theme.textColor
                        }
                        ProgressBar {
                            Layout.fillWidth: true
                            indeterminate: installer.progress < 0
                            value: Math.max(0, installer.progress)
                        }
                        Label {
                            Layout.fillWidth: true
                            text: installer.statusText
                            elide: Text.ElideRight
                            color: LingmoUI.Theme.secondaryTextColor
                            font.pointSize: LingmoUI.Units.fontCaption
                        }
                    }
                }
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            StackView {
                id: stack
                anchors.fill: parent
                anchors.topMargin: rootWindow.header.height
                clip: true
                initialItem: "qrc:/qml/HomePage.qml"
                // Body text one step above the default size; captions and titles set their own
                font.pointSize: LingmoUI.Units.fontBody

                pushEnter: Transition {
                    NumberAnimation { property: "opacity"; from: 0; to: 1; duration: LingmoUI.Units.shortDuration }
                }
                pushExit: Transition {
                    NumberAnimation { property: "opacity"; from: 1; to: 0; duration: LingmoUI.Units.veryShortDuration }
                }
                popEnter: Transition {
                    NumberAnimation { property: "opacity"; from: 0; to: 1; duration: LingmoUI.Units.shortDuration }
                }
                popExit: Transition {
                    NumberAnimation { property: "opacity"; from: 1; to: 0; duration: LingmoUI.Units.veryShortDuration }
                }
                replaceEnter: Transition {}
                replaceExit: Transition {}
            }
        }
    }

    StoreDialog {
        id: consentDialog
        iconName: "system-software-install"
        title: qsTr("Turn on Flathub?")
        text: qsTr("Flathub is the app store of the Linux world, with thousands of apps such as Spotify, Discord and Zoom. Its apps run isolated from the rest of the system.<br><br>Turn it on to install this app?")
        acceptText: qsTr("Turn On Flathub")
        rejectText: qsTr("Not Now")
        onAccepted: flatpak.addFlathub()
        onRejected: {
            rootWindow.consentKey = ""
            lutris.cancelPending()
        }
    }

    // Craft apps come from [archlingmo-craft], added to pacman.conf on demand
    StoreDialog {
        id: repoConsentDialog
        iconName: "system-software-install"
        title: qsTr("Enable the Craft apps repository?")
        text: qsTr("Craft apps come from a separate ArchLingmo repository. Enable it?")
        acceptText: qsTr("Enable and Install")
        rejectText: qsTr("Not Now")
        onAccepted: {
            const c = rootWindow.repoConsent
            rootWindow.repoConsent = null
            if (c)
                installer.enableRepoAndInstall(c.repo, c.key, c.packages, c.name)
        }
        onRejected: rootWindow.repoConsent = null
    }

    StoreDialog {
        id: ownCopyDialog
        iconName: "media-flash"
        title: qsTr("You need your own copy of the game")
        text: qsTr("The program is free, but the game itself (its graphics, music and levels) belongs to the publisher and is not included.<br><br>To play, copy your own cartridge to a file with a cartridge reader such as GB Operator or Sanni Cart Reader, or use the files of the version you bought.<br><br>The first time you open the program, it asks for that file and builds the game from it.")
        rejectText: qsTr("Cancel")
        onAccepted: {
            if (rootWindow.ownCopyGame)
                installer.installPackage(rootWindow.ownCopyGame.pkg, rootWindow.ownCopyGame.name)
            rootWindow.ownCopyGame = null
        }
        onRejected: rootWindow.ownCopyGame = null
    }

    StoreDialog {
        id: removeDialog
        iconName: "edit-delete"
        text: qsTr("Your own files and documents are not deleted.")
        acceptText: qsTr("Remove")
        destructive: true
        onAccepted: {
            if (rootWindow.removePackageName !== "")
                installer.removePackage(rootWindow.removePackageName, rootWindow.removeKey)
            else
                installer.remove(rootWindow.removeKey, rootWindow.removeSource)
        }
    }

    Timer {
        id: quitTimer
        interval: 1500
        onTriggered: if (rootWindow.quitWhenDone && !installer.busy) Qt.quit()
    }

    Shortcut {
        sequences: [StandardKey.Find]
        onActivated: searchField.forceActiveFocus()
    }
    Shortcut {
        sequence: "Alt+Left"
        onActivated: if (stack.depth > 1) stack.pop()
    }
}
