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

#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QLocale>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <functional>

class QNetworkAccessManager;
class QNetworkReply;

// Windows games from Lutris (lutris.net), installed and played through the Flathub
// build of Lutris (net.lutris.Lutris), never through a native lutris install:
//
// - search: https://lutris.net/api/games?search=<text>&with-installers=1, ranked here
//   (the API answers in alphabetical order) and limited to Windows games;
// - a game's page: https://lutris.net/api/games/<slug> (genres, description and
//   its installers with their scripts); only published wine installers are offered;
// - install: `flatpak run net.lutris.Lutris lutris:install/<installer-slug>`, after
//   the Lutris flatpak itself was installed through the store's Flatpak backend;
// - done when the game shows up as installed in that Lutris' own library
//   (~/.var/app/net.lutris.Lutris/data/lutris/pga.db, read only);
// - play: `flatpak run net.lutris.Lutris lutris:rungame/<slug>`.
//
// Answers and cover pictures are cached under $XDG_CACHE_HOME/lingmo-store/lutris.
// LINGMO_STORE_LUTRIS_API overrides https://lutris.net (tests).
class LutrisSource : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int revision READ revision NOTIFY changed)
    Q_PROPERTY(bool lutrisInstalled READ lutrisInstalled NOTIFY changed)
    // The store's extra isolation for the Lutris flatpak (a `flatpak override --user`)
    Q_PROPERTY(bool strictIsolation READ strictIsolation WRITE setStrictIsolation NOTIFY changed)
    Q_PROPERTY(QString gamesFolder READ gamesFolder CONSTANT)

public:
    static constexpr const char *FlatpakId = "net.lutris.Lutris";
    static constexpr const char *FlatpakRef = "app/net.lutris.Lutris/x86_64/stable";

    // Starts a program; `done` gets its exit code (-1: could not start)
    using Runner = std::function<void(const QString &program, const QStringList &args, std::function<void(int)> done)>;

    explicit LutrisSource(QObject *parent = nullptr);

    int revision() const { return m_revision; }
    bool lutrisInstalled() const;
    bool strictIsolation() const { return m_strict; }
    void setStrictIsolation(bool strict);
    QString gamesFolder() const;

    // Plumbing (tests replace these)
    void setRunner(Runner runner) { m_runner = std::move(runner); }
    void setLutrisInstalledCheck(std::function<bool()> check) { m_lutrisInstalledCheck = std::move(check); }
    void setLutrisRunningCheck(std::function<bool()> check) { m_lutrisRunningCheck = std::move(check); }
    void setCacheDir(const QString &dir) { m_cacheDir = dir; }
    // Where the Lutris flatpak keeps its data (~/.var/app/net.lutris.Lutris)
    void setAppDir(const QString &dir) { m_appDir = dir; }
    void setNetworkEnabled(bool enabled) { m_networkEnabled = enabled; }
    void setPollInterval(int ms) { m_poll.setInterval(ms); }
    // Seconds Lutris gets to appear before "closed without installing" is believed
    void setStartGrace(int seconds) { m_startGrace = seconds; }
    QString appDir() const { return m_appDir; }

    // Parsing and the rules, without the network (unit tests)
    struct Game {
        int id = 0;
        QString slug, name, cover, banner;
        int year = 0;
        QStringList platforms;
        QStringList aliases;
    };
    static QList<Game> parseSearch(const QByteArray &json);
    // A game's page: {slug, name, year, cover, banner, platforms, genres, description,
    //  userCount, installers, defaultInstaller, online, onlineReason}
    static QVariantMap parseGame(const QByteArray &json, const QLocale &locale = QLocale());
    // Published, non-draft wine installers: [{slug, name, notes, description, user, updated}]
    static QVariantList wineInstallers(const QJsonArray &installers);
    // The installer picked by default: the user's country ("Standard (BR)"), else
    // one called "Standard"/"Installer"/"Default", else the first. -1 when none.
    static int defaultInstaller(const QVariantList &installers, const QLocale &locale);
    // Windows games that match `query`, best first, at most `limit`
    static QVariantList rank(const QList<Game> &games, const QString &query, int limit = 12);
    // Leaves out the Lutris games the catalog already has (e.g. a native Flathub build)
    static QVariantList withoutCatalogDuplicates(const QVariantList &lutrisGames, const QVariantList &catalogApps);
    // Genres where the game is played online and may refuse Wine (MMORPG, MOBA…)
    static QString onlineGenre(const QStringList &genres);
    static bool validSlug(const QString &slug);
    static QString foldName(const QString &name);
    // The flatpak command lines
    static QStringList installArguments(const QString &installerSlug);
    static QStringList playArguments(const QString &gameSlug);
    static QStringList isolationArguments(bool strict);
    // Installed games in a Lutris library (pga.db): slugs and installer slugs
    static QSet<QString> readLibrary(const QString &databasePath);

    // QML
    Q_INVOKABLE void search(const QString &text);
    // The ranked Windows games for `text`, minus those already in `catalogApps`
    Q_INVOKABLE QVariantList results(const QString &text, const QVariantList &catalogApps = {}) const;
    // none | loading | ready | offline | busy | error
    Q_INVOKABLE QString searchState(const QString &text) const;
    Q_INVOKABLE void fetchGame(const QString &slug);
    // parseGame() plus {state, installed, status, lutrisUrl, error}
    Q_INVOKABLE QVariantMap game(const QString &slug) const;
    // The game's own status: "" | "lutris" (installing Lutris first) | "installing" | "installed"
    Q_INVOKABLE QString status(const QString &slug) const;
    Q_INVOKABLE bool install(const QString &slug, const QString &installerSlug);
    Q_INVOKABLE bool play(const QString &slug);
    // The user stops waiting for Lutris to finish
    Q_INVOKABLE void stopTracking(const QString &slug);
    // The Flathub consent for Lutris was refused
    Q_INVOKABLE void cancelPending();
    Q_INVOKABLE void refreshLibrary();

    // Whether the Lutris flatpak is installed may have changed
    void installedStateChanged() { bump(); }
    // The store's installer finished installing (or failed to install) the Lutris flatpak
    void lutrisInstallFinished(bool ok, const QString &error);
    // Commands started so far (fake mode and tests)
    QList<QStringList> commandLog() const { return m_commandLog; }

Q_SIGNALS:
    void changed();
    // Install net.lutris.Lutris through the store's Flatpak backend, then call lutrisInstallFinished()
    void lutrisInstallNeeded();
    // A game finished installing in Lutris (or the user closed Lutris first: ok = false)
    void gameFinished(const QString &slug, const QString &name, bool ok, const QString &message);

private:
    struct SearchEntry {
        QString state;            // loading | ready | offline | busy | error
        QList<Game> games;
        qint64 fetched = 0;
    };
    struct GameEntry {
        QString state;
        QVariantMap data;
        QString error;
        qint64 fetched = 0;
    };
    struct Tracking {
        QString installerSlug;
        QString name;
        qint64 started = 0;
        bool processDone = false;
    };

    QNetworkReply *get(const QUrl &url);
    QString apiBase() const;
    QString cachePath(const QString &kind, const QString &name) const;
    bool readCache(const QString &path, qint64 maxAge, QByteArray *data, qint64 *modified = nullptr) const;
    void writeCache(const QString &path, const QByteArray &data) const;
    void startSearch();
    void onSearchReply(QNetworkReply *reply, const QString &key);
    void queueCovers(const QList<Game> &games);
    void fetchNextCover();
    QString coverFor(const QString &slug, const QString &remote) const;
    bool rateLimited() const;
    void noteRateLimit(QNetworkReply *reply);
    void run(const QStringList &args, std::function<void(int)> done = {});
    void applyIsolation(std::function<void()> then);
    void seedLutrisConfig() const;
    void startInstall(const QString &slug);
    void poll();
    bool defaultLutrisRunning() const;
    QString databasePath() const;
    void bump();

    QNetworkAccessManager *m_network;
    Runner m_runner;
    std::function<bool()> m_lutrisInstalledCheck;
    std::function<bool()> m_lutrisRunningCheck;
    QString m_cacheDir;
    QString m_appDir;
    bool m_networkEnabled = true;
    bool m_fake = false;
    bool m_strict = true;
    int m_startGrace = 20;
    int m_revision = 0;

    QHash<QString, SearchEntry> m_searches;  // folded query -> games
    QString m_pendingQuery;
    QTimer m_debounce;
    qint64 m_lastRequest = 0;
    qint64 m_backoffUntil = 0;
    QHash<QString, GameEntry> m_games;      // slug -> details
    QStringList m_coverQueue;
    QHash<QString, QString> m_coverUrls;    // slug -> remote cover
    QSet<QString> m_coverFailed;
    int m_coversInFlight = 0;

    QSet<QString> m_library;                // installed slugs and installer slugs
    QSet<QString> m_fakeInstalled;
    QHash<QString, Tracking> m_tracking;    // slug -> install in progress
    QString m_pendingSlug, m_pendingInstaller; // waiting for the Lutris flatpak
    QTimer m_poll;
    QList<QStringList> m_commandLog;
};
