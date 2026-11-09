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

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <memory>
#include <optional>

class FlatpakBackend;

// Where an app can be installed from
struct AppSource {
    QString componentId;   // AppStream id as written in that catalog
    QString version;       // Arch: sync db version; Flathub: newest AppStream release
    QString desktopId;     // launchable .desktop id
    // Arch
    QString pkgname;
    QString repo;          // sync repository, empty when the package isn't in one
    // A known ArchLingmo repository that has it but isn't enabled (or synced) yet,
    // e.g. "archlingmo-craft": offered with "Enable and Install"
    QString needsRepo;
    qint64 downloadSize = -1;
    qint64 installedSize = -1;
    // Installable: in a sync repository now, or once needsRepo is enabled
    bool offered() const { return !repo.isEmpty() || !needsRepo.isEmpty(); }
    // Flathub
    QString flatpakRef;    // app/<id>/<arch>/<branch>
    bool verified = false;
};

struct AppEntry {
    QString key;           // lower-case id without ".desktop": identifies the app in the store
    QString id;            // AppStream id shown to the user (Flathub's when both exist)
    QString name;
    QString summary;
    QString description;   // AppStream markup (<p>, <ul>, <li>…): valid Qt rich text
    QString developer;
    QString license;
    QString homepage;
    QString iconUrl;       // file://, https:// or image://appicon/…
    QString iconName;      // theme icon fallback
    QStringList categories;
    QVariantList screenshots; // {thumbnail, source, caption}
    std::optional<AppSource> arch;
    std::optional<AppSource> flathub;
    int quality = 0;       // ranking hint: metadata completeness
    bool extra = false;    // installed flatpak that isn't in the catalog
    QStringList reviewIds; // every AppStream/desktop id the app is known by (ODRS ratings)
    // Curated apps of featured.json "curated" (no AppStream data in their repository)
    bool curated = false;
    bool experimental = false; // pre-alpha/alpha: shown with a warning badge and banner
    QStringList opens;        // file types it opens, for the user ("DOCX", "PDF"…)
    QString replaces;         // the well-known app it's an alternative to
    // accent- and case-folded text for search
    QString nameFold, keywordsFold, summaryFold, idFold;
};

struct CatalogData {
    QList<AppEntry> apps;
    QHash<QString, int> byKey;       // AppEntry::key
    QHash<QString, int> byPkgname;   // Arch package name
    QHash<QString, int> byFlatpakId; // lower-case Flathub id
    QSet<QString> localPackages;     // installed Arch packages (all of them)
    QSet<QString> explicitPackages;  // installed explicitly
    QStringList syncRepos;           // repositories pacman knows about
    QHash<QString, QString> nativePackages; // native game package -> "repo version", when a repo has it
    int flathubCount = 0;
    int archCount = 0;
};

// The software catalog: AppStream metadata of the Arch repositories
// (archlinux-appstream-data) and of Flathub (flatpak's appstream branch), merged
// per application, plus what's installed. Loaded in a worker thread with
// libappstream and libalpm; queried from QML.
class Catalog : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY changed)
    Q_PROPERTY(int appCount READ appCount NOTIFY changed)
    Q_PROPERTY(int flathubCount READ flathubCount NOTIFY changed)
    Q_PROPERTY(int archCount READ archCount NOTIFY changed)
    Q_PROPERTY(bool multilibEnabled READ multilibEnabled NOTIFY changed)

public:
    explicit Catalog(FlatpakBackend *flatpak, QObject *parent = nullptr);
    ~Catalog() override;

    bool loading() const { return m_loading; }
    bool ready() const { return bool(m_data); }
    int appCount() const { return m_data ? m_data->apps.size() : 0; }
    int flathubCount() const { return m_data ? m_data->flathubCount : 0; }
    int archCount() const { return m_data ? m_data->archCount : 0; }
    bool multilibEnabled() const;

    const AppEntry *entry(const QString &key) const;
    bool isPackageInstalled(const QString &pkgname) const;
    bool isFlatpakInstalled(const QString &id) const;
    QString flatpakInstallation(const QString &id) const;
    QString pacmanConf() const;
    // Whether pacman.conf lists this repository (as of the last load)
    Q_INVOKABLE bool repoEnabled(const QString &repo) const;

    // Maps what a user or another program passes (--app) to an app key: AppStream id
    // (with or without .desktop), Flathub id or ref (app/<id>/<arch>/<branch>),
    // appstream:// URL or Arch package name
    Q_INVOKABLE QString resolve(const QString &query) const;

    Q_INVOKABLE QVariantMap app(const QString &key) const;
    Q_INVOKABLE QVariantList featured() const;
    Q_INVOKABLE QVariantList heroes() const;
    // Featured collections of featured.json "collections" (e.g. the Craft suite):
    // {id, title, text, colors, experimental, apps, bundle {label, name, packages, available, installed}}
    Q_INVOKABLE QVariantList collections() const;
    Q_INVOKABLE QVariantList gameApps() const;
    Q_INVOKABLE QVariantList category(const QString &category) const;
    Q_INVOKABLE QVariantList search(const QString &text, int limit = 60) const;
    Q_INVOKABLE QVariantList installedApps() const;
    // "Native games": PC ports from decompilation projects (featured.json "nativeGames")
    Q_INVOKABLE QVariantList nativeGames() const;
    // The native-games entry of a Flathub app, if it is one (for the own-copy notice)
    QVariantMap nativeGameFor(const QString &key) const;
    // Fetches sizes that need the network or a slow query; answers with sizeReady()
    Q_INVOKABLE void fetchFlathubSize(const QString &key);

    // Fake mode: remembers what fake transactions installed or removed
    void setFakeInstalled(const QString &key, const QString &source, bool installed);
    void setFakePackageInstalled(const QString &pkgname, bool installed = true);
    void setFakeFlatpakInstalled(const QString &id, bool installed);

public Q_SLOTS:
    void reload();
    // Re-reads only what's installed (after a transaction)
    void refreshInstalled();

Q_SIGNALS:
    void loadingChanged();
    void changed();
    void installedChanged();
    void sizeReady(const QString &key, const QString &text);

private:
    QVariantMap summaryMap(const AppEntry &app) const;
    QVariantList listFor(const QStringList &ids) const;
    QString localized(const QJsonValue &value) const;
    void loadFeatured();

    FlatpakBackend *m_flatpak;
    std::shared_ptr<const CatalogData> m_data;
    bool m_loading = false;
    bool m_reloadPending = false;
    QJsonObject m_featured;
    QHash<QString, QString> m_aliases;          // Flathub key -> Arch package
    QHash<QString, QString> m_flatpakInstalled; // Flathub id -> installation ("system"/"user")
    QSet<QString> m_localPackages;
    QSet<QString> m_explicitPackages;
    QSet<QString> m_fakeArch, m_fakeArchRemoved;
    QHash<QString, QString> m_fakeFlatpak;
    QSet<QString> m_fakeFlatpakRemoved;
    bool m_fake;
};
