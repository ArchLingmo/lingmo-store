/*
 * Copyright (C) 2026 LingmoOS Team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 */

// Lutris games: lutris.net answers recorded in tests/fixtures/lutris (real responses,
// recorded once), the merge into the store's search, and the flatpak command lines
// through a fake runner. No network, no flatpak, a scratch HOME.

#include "gamecompat.h"
#include "lutrissource.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSettings>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>

static QByteArray fixture(const QString &name)
{
    QFile file(QStringLiteral(FIXTURES_DIR "/lutris/") + name);
    if (!file.open(QIODevice::ReadOnly))
        qFatal("missing fixture %s", qPrintable(name));
    return file.readAll();
}

static QStringList slugs(const QVariantList &list)
{
    QStringList out;
    for (const QVariant &v : list)
        out << v.toMap().value(QStringLiteral("slug")).toString();
    return out;
}

class TestLutris : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;

    // A LutrisSource that never touches the network, flatpak or the real home
    struct Fixture {
        LutrisSource source;
        QList<QStringList> commands;
        bool lutrisInstalled = true;
        bool lutrisRunning = true;
        Fixture(const QString &root)
        {
            source.setCacheDir(root + QStringLiteral("/cache/lingmo-store/lutris"));
            source.setAppDir(root + QStringLiteral("/home/.var/app/net.lutris.Lutris"));
            source.setNetworkEnabled(false);
            source.setPollInterval(20);
            source.setStartGrace(0);
            source.setLutrisInstalledCheck([this] { return lutrisInstalled; });
            source.setLutrisRunningCheck([this] { return lutrisRunning; });
            source.setRunner([this](const QString &program, const QStringList &args, std::function<void(int)> done) {
                commands << (QStringList{program} + args);
                if (done)
                    done(0);
            });
        }
        void cache(const QString &kind, const QString &name, const QByteArray &data, const QString &root)
        {
            const QString dir = root + QStringLiteral("/cache/lingmo-store/lutris/") + kind;
            QDir().mkpath(dir);
            QFile file(dir + QLatin1Char('/') + name);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(data);
        }
    };

    static QString searchCacheName(const QString &query)
    {
        return QString::fromLatin1(
                   QCryptographicHash::hash(LutrisSource::foldName(query).toUtf8(), QCryptographicHash::Sha1).toHex())
            + QStringLiteral(".json");
    }

    void writeLibrary(const QString &appDir, const QString &slug, const QString &installerSlug, int installed)
    {
        QDir().mkpath(appDir + QStringLiteral("/data/lutris"));
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("writer"));
            db.setDatabaseName(appDir + QStringLiteral("/data/lutris/pga.db"));
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS games (id INTEGER PRIMARY KEY, name TEXT, slug TEXT, "
                                          "installer_slug TEXT, runner TEXT, directory TEXT, installed INTEGER)")));
            q.prepare(QStringLiteral("INSERT INTO games (name, slug, installer_slug, runner, installed) VALUES (?, ?, ?, 'wine', ?)"));
            q.addBindValue(slug);
            q.addBindValue(slug);
            q.addBindValue(installerSlug);
            q.addBindValue(installed);
            QVERIFY(q.exec());
            db.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("writer"));
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        // main() already pointed HOME and the XDG dirs at a scratch directory
        QVERIFY(QDir::homePath() != QStringLiteral("/home/") + qEnvironmentVariable("USER"));
    }

    void parseSearchBrokenRanks()
    {
        const auto games = LutrisSource::parseSearch(fixture(QStringLiteral("search-broken-ranks.json")));
        QCOMPARE(games.size(), 1);
        QCOMPARE(games.first().slug, QStringLiteral("broken-ranks"));
        QCOMPARE(games.first().name, QStringLiteral("Broken Ranks"));
        QCOMPARE(games.first().year, 2022);
        QCOMPARE(games.first().platforms, QStringList{QStringLiteral("Windows")});
        QVERIFY(games.first().cover.startsWith(QLatin1String("https://lutris.net/")));

        const QVariantList ranked = LutrisSource::rank(games, QStringLiteral("broken ranks"));
        QCOMPARE(slugs(ranked), QStringList{QStringLiteral("broken-ranks")});
        QCOMPARE(ranked.first().toMap().value(QStringLiteral("runner")).toString(), QStringLiteral("wine"));
    }

    void parseSearchLeague()
    {
        const auto games = LutrisSource::parseSearch(fixture(QStringLiteral("search-league.json")));
        QCOMPARE(games.size(), 250); // first page, in the API's alphabetical order
        // Only Windows games, best match first
        const QVariantList all = LutrisSource::rank(games, QStringLiteral("league"), 1000);
        for (const QVariant &v : all)
            QVERIFY(v.toMap().value(QStringLiteral("platforms")).toStringList().contains(QStringLiteral("Windows")));
        QVERIFY(slugs(all).contains(QStringLiteral("league-of-legends")));
        QVERIFY(!slugs(all).contains(QStringLiteral("2-player-super-league"))); // Amstrad CPC
        const QVariantList top = LutrisSource::rank(games, QStringLiteral("league of legends"), 3);
        QCOMPARE(slugs(top).first(), QStringLiteral("league-of-legends"));
        QCOMPARE(LutrisSource::rank(games, QStringLiteral("league"), 12).size(), 12);
    }

    void parseSearchBattleNet()
    {
        const auto games = LutrisSource::parseSearch(fixture(QStringLiteral("search-battle.net.json")));
        QVERIFY(!games.isEmpty());
        // "battle.net" and "Battle.net" fold to the same name: an exact match wins
        QCOMPARE(slugs(LutrisSource::rank(games, QStringLiteral("battle.net"))).first(), QStringLiteral("battlenet"));
    }

    void parseGameBrokenRanks()
    {
        const QVariantMap game = LutrisSource::parseGame(fixture(QStringLiteral("game-broken-ranks.json")), QLocale(QStringLiteral("pt_BR")));
        QCOMPARE(game.value(QStringLiteral("slug")).toString(), QStringLiteral("broken-ranks"));
        QVERIFY(game.value(QStringLiteral("genres")).toStringList().contains(QStringLiteral("MMORPG")));
        QVERIFY(game.value(QStringLiteral("online")).toBool());
        QCOMPARE(game.value(QStringLiteral("onlineGenre")).toString(), QStringLiteral("MMORPG"));
        const QVariantList installers = game.value(QStringLiteral("installers")).toList();
        QCOMPARE(slugs(installers), QStringList{QStringLiteral("broken-ranks-installer")});
        QCOMPARE(game.value(QStringLiteral("defaultInstaller")).toString(), QStringLiteral("broken-ranks-installer"));
        QVERIFY(!installers.first().toMap().value(QStringLiteral("notes")).toString().isEmpty());
        QVERIFY(!game.value(QStringLiteral("description")).toString().contains(QLatin1Char('\r')));
    }

    void parseGameLeagueInstallers()
    {
        const QByteArray json = fixture(QStringLiteral("game-league-of-legends.json"));
        const QVariantMap br = LutrisSource::parseGame(json, QLocale(QStringLiteral("pt_BR")));
        const QStringList installers = slugs(br.value(QStringLiteral("installers")).toList());
        // 17 on the site: the unpublished ones are left out
        QCOMPARE(installers.size(), 15);
        QVERIFY(!installers.contains(QStringLiteral("league-of-legends-testing-only")));
        QVERIFY(!installers.contains(QStringLiteral("league-of-legends-standard-kr-region")));
        // A Brazilian gets the Brazilian server
        QCOMPARE(br.value(QStringLiteral("defaultInstaller")).toString(), QStringLiteral("league-of-legends-standard-br"));
        const QVariantMap tr = LutrisSource::parseGame(json, QLocale(QStringLiteral("tr_TR")));
        QCOMPARE(tr.value(QStringLiteral("defaultInstaller")).toString(), QStringLiteral("league-of-legends-standard-tr"));
        // Nobody's region: the first "Standard" one, not the Vietnamese client listed first
        const QVariantMap c = LutrisSource::parseGame(json, QLocale::c());
        QVERIFY(c.value(QStringLiteral("defaultInstaller")).toString().startsWith(QLatin1String("league-of-legends-standard")));
        QCOMPARE(br.value(QStringLiteral("onlineGenre")).toString(), QStringLiteral("Moba"));
    }

    void parseGameBattleNet()
    {
        const QVariantMap game = LutrisSource::parseGame(fixture(QStringLiteral("game-battlenet.json")));
        QCOMPARE(game.value(QStringLiteral("defaultInstaller")).toString(), QStringLiteral("battlenet-standard"));
        QVERIFY(!game.value(QStringLiteral("online")).toBool());
    }

    void rejectsUnsafeSlugs()
    {
        QVERIFY(LutrisSource::validSlug(QStringLiteral("broken-ranks-installer")));
        QVERIFY(!LutrisSource::validSlug(QStringLiteral("../etc/passwd")));
        QVERIFY(!LutrisSource::validSlug(QStringLiteral("a b")));
        QVERIFY(!LutrisSource::validSlug(QStringLiteral("-rf")));
        QVERIFY(!LutrisSource::validSlug(QStringLiteral("x;rm")));
        QVERIFY(!LutrisSource::validSlug(QString()));
        // Pictures only from lutris.net
        const QByteArray evil = R"({"results":[{"id":1,"slug":"x","name":"X","platforms":[{"name":"Windows"}],
                                    "coverart":"http://evil.example/x.jpg"}]})";
        QVERIFY(LutrisSource::parseSearch(evil).first().cover.isEmpty());
    }

    void mergeIntoSearch()
    {
        Fixture f(m_dir.path() + QStringLiteral("/merge"));
        f.cache(QStringLiteral("search"), searchCacheName(QStringLiteral("battle.net")), fixture(QStringLiteral("search-battle.net.json")),
                m_dir.path() + QStringLiteral("/merge"));
        f.cache(QStringLiteral("search"), searchCacheName(QStringLiteral("broken ranks")),
                fixture(QStringLiteral("search-broken-ranks.json")), m_dir.path() + QStringLiteral("/merge"));

        // From the cache, without the network
        f.source.search(QStringLiteral("Broken Ranks"));
        QCOMPARE(f.source.searchState(QStringLiteral("broken ranks")), QStringLiteral("ready"));
        const QVariantList results = f.source.results(QStringLiteral("broken ranks"), {});
        QCOMPARE(slugs(results), QStringList{QStringLiteral("broken-ranks")});
        QCOMPARE(results.first().toMap().value(QStringLiteral("status")).toString(), QString());

        // A game the catalog already has (e.g. a Flathub build) isn't offered twice
        f.source.search(QStringLiteral("battle.net"));
        const QVariantList catalog{QVariantMap{{QStringLiteral("key"), QStringLiteral("x")}, {QStringLiteral("name"), QStringLiteral("Battle.net")}}};
        QVERIFY(slugs(f.source.results(QStringLiteral("battle.net"), {})).contains(QStringLiteral("battlenet")));
        QVERIFY(!slugs(f.source.results(QStringLiteral("battle.net"), catalog)).contains(QStringLiteral("battlenet")));

        // Short words don't go to lutris.net; offline without a cache says so
        f.source.search(QStringLiteral("ab"));
        QCOMPARE(f.source.searchState(QStringLiteral("ab")), QStringLiteral("none"));
        f.source.search(QStringLiteral("starcraft"));
        QTRY_COMPARE(f.source.searchState(QStringLiteral("starcraft")), QStringLiteral("offline"));
        QVERIFY(f.commands.isEmpty());
    }

    void commandLines()
    {
        QCOMPARE(LutrisSource::installArguments(QStringLiteral("broken-ranks-installer")),
                 (QStringList{QStringLiteral("run"), QStringLiteral("net.lutris.Lutris"), QStringLiteral("lutris:install/broken-ranks-installer")}));
        QCOMPARE(LutrisSource::playArguments(QStringLiteral("broken-ranks")),
                 (QStringList{QStringLiteral("run"), QStringLiteral("net.lutris.Lutris"), QStringLiteral("lutris:rungame/broken-ranks")}));
        const QStringList strict = LutrisSource::isolationArguments(true);
        QCOMPARE(strict.mid(0, 2), (QStringList{QStringLiteral("override"), QStringLiteral("--user")}));
        QCOMPARE(strict.last(), QStringLiteral("net.lutris.Lutris"));
        QVERIFY(strict.contains(QStringLiteral("--nofilesystem=home")));
        QVERIFY(strict.contains(QStringLiteral("--no-talk-name=org.freedesktop.Flatpak")));
        QVERIFY(strict.contains(QStringLiteral("--filesystem=~/Games/Lutris:create")));
        QCOMPARE(LutrisSource::isolationArguments(false),
                 (QStringList{QStringLiteral("override"), QStringLiteral("--user"), QStringLiteral("--reset"), QStringLiteral("net.lutris.Lutris")}));
    }

    void installsLutrisFirstThenTheGame()
    {
        const QString root = m_dir.path() + QStringLiteral("/flow");
        Fixture f(root);
        f.lutrisInstalled = false;
        f.cache(QStringLiteral("games"), QStringLiteral("broken-ranks.json"), fixture(QStringLiteral("game-broken-ranks.json")), root);
        f.source.fetchGame(QStringLiteral("broken-ranks"));
        QCOMPARE(f.source.game(QStringLiteral("broken-ranks")).value(QStringLiteral("state")).toString(), QStringLiteral("ready"));

        // An installer this game doesn't have is refused
        QVERIFY(!f.source.install(QStringLiteral("broken-ranks"), QStringLiteral("league-of-legends-standard-br")));

        QSignalSpy needed(&f.source, &LutrisSource::lutrisInstallNeeded);
        QSignalSpy finished(&f.source, &LutrisSource::gameFinished);
        QVERIFY(f.source.install(QStringLiteral("broken-ranks"), QStringLiteral("broken-ranks-installer")));
        QCOMPARE(needed.count(), 1);
        QCOMPARE(f.source.status(QStringLiteral("broken-ranks")), QStringLiteral("lutris"));
        QVERIFY(f.commands.isEmpty()); // nothing runs before Lutris is there

        // The store's Flatpak backend installed Lutris
        f.lutrisInstalled = true;
        f.source.lutrisInstallFinished(true, QString());
        QCOMPARE(f.source.status(QStringLiteral("broken-ranks")), QStringLiteral("installing"));
        QCOMPARE(f.commands.size(), 2);
        QCOMPARE(f.commands.at(0), QStringList{QStringLiteral("flatpak")} + LutrisSource::isolationArguments(true));
        QCOMPARE(f.commands.at(1), (QStringList{QStringLiteral("flatpak"), QStringLiteral("run"), QStringLiteral("net.lutris.Lutris"),
                                                QStringLiteral("lutris:install/broken-ranks-installer")}));

        // Games go to ~/Games/Lutris, in the flatpak's own configuration
        QFile config(f.source.appDir() + QStringLiteral("/data/lutris/system.yml"));
        QVERIFY(config.open(QIODevice::ReadOnly));
        QVERIFY(config.readAll().contains(QDir::homePath().toUtf8() + "/Games/Lutris"));

        // Lutris adds the game to its library: done
        writeLibrary(f.source.appDir(), QStringLiteral("broken-ranks"), QStringLiteral("broken-ranks-installer"), 1);
        QTRY_COMPARE(f.source.status(QStringLiteral("broken-ranks")), QStringLiteral("installed"));
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished.first().at(2).toBool(), true);
        QVERIFY(f.source.game(QStringLiteral("broken-ranks")).value(QStringLiteral("installed")).toBool());

        // Play
        QVERIFY(f.source.play(QStringLiteral("broken-ranks")));
        QCOMPARE(f.commands.last(), (QStringList{QStringLiteral("flatpak"), QStringLiteral("run"), QStringLiteral("net.lutris.Lutris"),
                                                 QStringLiteral("lutris:rungame/broken-ranks")}));
        QVERIFY(!f.source.play(QStringLiteral("league-of-legends"))); // not installed
    }

    void lutrisClosedBeforeFinishing()
    {
        QSettings().clear(); // the earlier test applied the isolation
        const QString root = m_dir.path() + QStringLiteral("/closed");
        Fixture f(root);
        f.lutrisRunning = false;
        f.source.setStrictIsolation(false);
        f.cache(QStringLiteral("games"), QStringLiteral("battlenet.json"), fixture(QStringLiteral("game-battlenet.json")), root);
        f.source.fetchGame(QStringLiteral("battlenet"));
        QSignalSpy finished(&f.source, &LutrisSource::gameFinished);
        QVERIFY(f.source.install(QStringLiteral("battlenet"), QStringLiteral("battlenet-standard")));
        // Without the extra isolation: no override, just Lutris
        QCOMPARE(f.commands.size(), 1);
        QCOMPARE(f.commands.first().last(), QStringLiteral("lutris:install/battlenet-standard"));
        QTRY_COMPARE(finished.count(), 1);
        QCOMPARE(finished.first().at(2).toBool(), false);
        QCOMPARE(f.source.status(QStringLiteral("battlenet")), QString());
    }

    void lutrisInstallFailed()
    {
        const QString root = m_dir.path() + QStringLiteral("/failed");
        Fixture f(root);
        f.lutrisInstalled = false;
        f.cache(QStringLiteral("games"), QStringLiteral("broken-ranks.json"), fixture(QStringLiteral("game-broken-ranks.json")), root);
        f.source.fetchGame(QStringLiteral("broken-ranks"));
        QSignalSpy finished(&f.source, &LutrisSource::gameFinished);
        QVERIFY(f.source.install(QStringLiteral("broken-ranks"), QStringLiteral("broken-ranks-installer")));
        f.source.lutrisInstallFinished(false, QStringLiteral("No internet"));
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished.first().at(3).toString(), QStringLiteral("No internet"));
        QCOMPARE(f.source.status(QStringLiteral("broken-ranks")), QString());
        QVERIFY(f.commands.isEmpty());
    }

    void libraryReadsOnlyInstalledGames()
    {
        const QString appDir = m_dir.path() + QStringLiteral("/library");
        writeLibrary(appDir, QStringLiteral("removed-game"), QStringLiteral("removed-game-installer"), 0);
        writeLibrary(appDir, QStringLiteral("broken-ranks"), QStringLiteral("broken-ranks-installer"), 1);
        const QSet<QString> library = LutrisSource::readLibrary(appDir + QStringLiteral("/data/lutris/pga.db"));
        QVERIFY(library.contains(QStringLiteral("broken-ranks")));
        QVERIFY(library.contains(QStringLiteral("broken-ranks-installer")));
        QVERIFY(!library.contains(QStringLiteral("removed-game")));
        QVERIFY(LutrisSource::readLibrary(appDir + QStringLiteral("/missing.db")).isEmpty());
    }

    void defaultsStayAwayFromNativeLutris()
    {
        LutrisSource source;
        QVERIFY(source.appDir().endsWith(QLatin1String("/.var/app/net.lutris.Lutris")));
        QVERIFY(!source.appDir().contains(QLatin1String("/.local/share/lutris")));
        QVERIFY(!source.appDir().contains(QLatin1String("/.config/lutris")));
        QCOMPARE(source.gamesFolder(), QDir::homePath() + QStringLiteral("/Games/Lutris"));
    }

    void antiCheatWarning()
    {
        const QJsonArray games = QJsonDocument::fromJson(fixture(QStringLiteral("anticheat-sample.json"))).array();
        QJsonObject lol, lostArk;
        for (const QJsonValue &v : games) {
            if (v.toObject().value(QStringLiteral("name")).toString() == QLatin1String("League of Legends"))
                lol = v.toObject();
            if (v.toObject().value(QStringLiteral("name")).toString() == QLatin1String("Lost Ark"))
                lostArk = v.toObject();
        }
        const QVariantMap blocked = GameCompat::antiCheatVerdict(lol);
        QVERIFY(blocked.value(QStringLiteral("blocked")).toBool());
        QCOMPARE(blocked.value(QStringLiteral("anticheats")).toString(), QStringLiteral("Vanguard"));
        QVERIFY(!GameCompat::antiCheatVerdict(lostArk).value(QStringLiteral("blocked")).toBool());
        QVERIFY(GameCompat::antiCheatVerdict({}).isEmpty());
    }
};

int main(int argc, char **argv)
{
    // Never the real home: Lutris paths, caches and settings all derive from these
    QTemporaryDir scratch;
    if (!scratch.isValid())
        return 1;
    const QByteArray root = scratch.path().toUtf8();
    qputenv("HOME", root + "/home");
    qputenv("XDG_CONFIG_HOME", root + "/home/.config");
    qputenv("XDG_DATA_HOME", root + "/home/.local/share");
    qputenv("XDG_CACHE_HOME", root + "/home/.cache");
    qputenv("XDG_STATE_HOME", root + "/home/.local/state");
    qputenv("XDG_RUNTIME_DIR", root + "/run");
    QDir().mkpath(scratch.path() + QStringLiteral("/home"));
    QDir().mkpath(scratch.path() + QStringLiteral("/run"));
    QFile::setPermissions(scratch.path() + QStringLiteral("/run"), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    qunsetenv("LINGMO_STORE_FAKE");
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("tst_lutris"));
    TestLutris test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_lutris.moc"
