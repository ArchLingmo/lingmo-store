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

#pragma once

#include <QObject>
#include <QPointer>
#include <QStringList>

class Catalog;
class FlatpakBackend;
class GameCompat;
class GamesManager;
class Installer;
class LutrisSource;
class Permissions;
class Reviews;
class QQmlApplicationEngine;
class QQuickWindow;

// What the command line (or a second instance) asks the window to show
struct StoreRequest {
    enum Kind { None, App, Search, Games } kind = None;
    QString value;
    static StoreRequest fromArguments(const QStringList &arguments);
};

// The store: owns the backends and the window. Exported on the session bus as
// com.lingmo.Store /Store so a second `lingmo-store --app …` reuses this window.
class StoreApp : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.lingmo.Store")
    Q_PROPERTY(bool fakeMode READ fakeMode CONSTANT)
    Q_PROPERTY(QString version READ version CONSTANT)

public:
    explicit StoreApp(QObject *parent = nullptr);
    ~StoreApp() override;

    bool fakeMode() const { return m_fake; }
    QString version() const;
    void handle(const StoreRequest &request);

    // QML helpers
    Q_INVOKABLE void openUpdater();
    Q_INVOKABLE bool launch(const QString &key, const QString &source);
    Q_INVOKABLE bool launchPackage(const QString &pkgname);
    Q_INVOKABLE void openUrl(const QString &url);
    Q_INVOKABLE QString formatSize(qint64 bytes) const;
    Q_INVOKABLE bool hasThemeIcon(const QString &name) const;
    Q_INVOKABLE void copyText(const QString &text);

public Q_SLOTS:
    // D-Bus API
    void show();
    void openApp(const QString &app);
    void search(const QString &text);
    void openGames();
    // Same arguments as the command line, e.g. ["--app", "com.google.Chrome"]
    void activate(const QStringList &arguments);

Q_SIGNALS:
    void appRequested(const QString &query);
    void searchRequested(const QString &text);
    void gamesRequested();

private:
    void onFinished(const QString &key, const QString &source, const QString &action, bool ok, const QString &message);
    void notify(const QString &summary, const QString &body);
    static bool launchDesktopFile(const QString &path, const QString &fallback);

    bool m_fake;
    FlatpakBackend *m_flatpak;
    Catalog *m_catalog;
    Installer *m_installer;
    GamesManager *m_games;
    GameCompat *m_compat;
    Reviews *m_reviews;
    Permissions *m_permissions;
    LutrisSource *m_lutris;
    QString m_lutrisKey; // the installer key of the Lutris flatpak while it's being installed for a game
    QQmlApplicationEngine *m_engine = nullptr;
    QPointer<QQuickWindow> m_window;
    StoreRequest m_pending;
};
