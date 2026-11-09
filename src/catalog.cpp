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

#include "catalog.h"
#include "flatpakbackend.h"

#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <QThread>
#include <QUrl>

#include <algorithm>

#include <appstream.h>
#ifdef HAVE_ALPM
#include <alpm.h>
#endif

namespace {

// The repositories whose AppStream data archlinux-appstream-data ships
const QStringList OfficialRepos = {QStringLiteral("core"), QStringLiteral("extra"), QStringLiteral("multilib")};

// Packages the store never offers to remove
const QSet<QString> ProtectedPackages = {
    QStringLiteral("base"), QStringLiteral("linux"), QStringLiteral("systemd"), QStringLiteral("glibc"),
    QStringLiteral("pacman"), QStringLiteral("sudo"), QStringLiteral("polkit"), QStringLiteral("mesa"),
    QStringLiteral("xorg-server"), QStringLiteral("sddm"), QStringLiteral("kwin"), QStringLiteral("kwin-x11"),
    QStringLiteral("flatpak"), QStringLiteral("networkmanager"), QStringLiteral("dbus"),
};

QString fold(const QString &text)
{
    const QString decomposed = text.normalized(QString::NormalizationForm_D);
    QString out;
    out.reserve(decomposed.size());
    for (const QChar c : decomposed) {
        if (c.category() != QChar::Mark_NonSpacing)
            out.append(c.toLower());
    }
    return out;
}

QString keyFor(QString id)
{
    id = id.trimmed();
    if (id.endsWith(QLatin1String(".desktop"), Qt::CaseInsensitive))
        id.chop(8);
    return id.toLower();
}

QString utf8(const gchar *s)
{
    return s ? QString::fromUtf8(s) : QString();
}

QString sizeText(qint64 bytes)
{
    return bytes > 0 ? QLocale().formattedDataSize(bytes, 1) : QString();
}

QStringList pacmanRepos(const QString &conf)
{
    QProcess proc;
    QStringList args;
    if (!conf.isEmpty())
        args << QStringLiteral("--config") << conf;
    args << QStringLiteral("--repo-list");
    proc.start(QStringLiteral("pacman-conf"), args);
    if (!proc.waitForFinished(10000) || proc.exitCode() != 0)
        return {};
    return QString::fromUtf8(proc.readAllStandardOutput()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}

struct PackageInfo {
    QString repo;
    QString version;
    qint64 size = -1;
    qint64 isize = -1;
};

// Read-only view of pacman's databases
class Pacman
{
public:
    explicit Pacman(const QStringList &repos)
    {
#ifdef HAVE_ALPM
        alpm_errno_t err;
        m_handle = alpm_initialize("/", "/var/lib/pacman/", &err);
        if (!m_handle) {
            qWarning() << "libalpm:" << alpm_strerror(err);
            return;
        }
        for (const QString &repo : repos) {
            if (alpm_db_t *db = alpm_register_syncdb(m_handle, repo.toUtf8().constData(), ALPM_SIG_USE_DEFAULT)) {
                m_all.append({repo, db});
                if (OfficialRepos.contains(repo))
                    m_sync.append({repo, db});
            }
        }
#else
        Q_UNUSED(repos)
#endif
    }
    ~Pacman()
    {
#ifdef HAVE_ALPM
        if (m_handle)
            alpm_release(m_handle);
#endif
    }

    // In the official repositories only (unless `anyRepo`)
    std::optional<PackageInfo> find(const QString &name, bool anyRepo = false) const
    {
#ifdef HAVE_ALPM
        const QByteArray n = name.toUtf8();
        for (const auto &[repo, db] : anyRepo ? m_all : m_sync) {
            if (alpm_pkg_t *pkg = alpm_db_get_pkg(db, n.constData())) {
                return PackageInfo{repo, QString::fromUtf8(alpm_pkg_get_version(pkg)),
                                   qint64(alpm_pkg_get_size(pkg)), qint64(alpm_pkg_get_isize(pkg))};
            }
        }
#else
        Q_UNUSED(name)
        Q_UNUSED(anyRepo)
#endif
        return std::nullopt;
    }

    void installed(QSet<QString> *all, QSet<QString> *explicitly) const
    {
#ifdef HAVE_ALPM
        if (!m_handle)
            return;
        for (alpm_list_t *i = alpm_db_get_pkgcache(alpm_get_localdb(m_handle)); i; i = alpm_list_next(i)) {
            auto *pkg = static_cast<alpm_pkg_t *>(i->data);
            const QString name = QString::fromUtf8(alpm_pkg_get_name(pkg));
            all->insert(name);
            if (explicitly && alpm_pkg_get_reason(pkg) == ALPM_PKG_REASON_EXPLICIT)
                explicitly->insert(name);
        }
#else
        Q_UNUSED(all)
        Q_UNUSED(explicitly)
#endif
    }

private:
#ifdef HAVE_ALPM
    alpm_handle_t *m_handle = nullptr;
    QList<std::pair<QString, alpm_db_t *>> m_sync;
    QList<std::pair<QString, alpm_db_t *>> m_all;
#endif
};

// Newest release version in the metadata
QString latestVersion(AsComponent *cpt)
{
    AsReleaseList *releases = as_component_get_releases_plain(cpt);
    if (!releases || as_release_list_len(releases) == 0)
        return {};
    return utf8(as_release_get_version(as_release_list_index(releases, 0)));
}

void pickIcon(AsComponent *cpt, AppEntry *app)
{
    GPtrArray *icons = as_component_get_icons(cpt);
    QString cached, remote, stock;
    guint cachedWidth = 0;
    for (guint i = 0; i < icons->len; ++i) {
        auto *icon = AS_ICON(g_ptr_array_index(icons, i));
        switch (as_icon_get_kind(icon)) {
        case AS_ICON_KIND_CACHED: {
            const QString file = utf8(as_icon_get_filename(icon));
            const guint width = as_icon_get_width(icon) * qMax(1u, as_icon_get_scale(icon));
            // HiDPI variants are listed even when the catalog doesn't ship them
            if (!file.isEmpty() && width > cachedWidth && width <= 256 && QFileInfo::exists(file)) {
                cached = file;
                cachedWidth = width;
            }
            break;
        }
        case AS_ICON_KIND_REMOTE:
            if (remote.isEmpty())
                remote = utf8(as_icon_get_url(icon));
            break;
        case AS_ICON_KIND_STOCK:
            stock = utf8(as_icon_get_name(icon));
            break;
        default:
            break;
        }
    }
    if (!cached.isEmpty()) {
        // PNGs load straight away; Arch ships JPEG XL, decoded by the icon provider
        app->iconUrl = cached.endsWith(QLatin1String(".png"))
            ? QUrl::fromLocalFile(cached).toString()
            : QStringLiteral("image://appicon/") + QString::fromUtf8(QUrl::toPercentEncoding(cached));
    } else if (!remote.isEmpty()) {
        app->iconUrl = remote;
    }
    app->iconName = stock;
}

QVariantList screenshotsOf(AsComponent *cpt)
{
    QVariantList list;
    GPtrArray *shots = as_component_get_screenshots_all(cpt);
    for (guint i = 0; shots && i < shots->len && list.size() < 8; ++i) {
        auto *shot = AS_SCREENSHOT(g_ptr_array_index(shots, i));
        if (as_screenshot_get_media_kind(shot) != AS_SCREENSHOT_MEDIA_KIND_IMAGE)
            continue;
        GPtrArray *images = as_screenshot_get_images_all(shot);
        QString source, thumb;
        int thumbDistance = INT_MAX;
        for (guint j = 0; images && j < images->len; ++j) {
            auto *image = AS_IMAGE(g_ptr_array_index(images, j));
            const QString url = utf8(as_image_get_url(image));
            if (url.isEmpty())
                continue;
            if (as_image_get_kind(image) == AS_IMAGE_KIND_SOURCE) {
                if (source.isEmpty())
                    source = url;
                continue;
            }
            // Thumbnails around 752px wide look sharp in the gallery
            const int distance = qAbs(int(as_image_get_width(image)) - 752);
            if (distance < thumbDistance) {
                thumb = url;
                thumbDistance = distance;
            }
        }
        if (source.isEmpty() && thumb.isEmpty())
            continue;
        list << QVariantMap{
            {QStringLiteral("thumbnail"), thumb.isEmpty() ? source : thumb},
            {QStringLiteral("source"), source.isEmpty() ? thumb : source},
            {QStringLiteral("caption"), utf8(as_screenshot_get_caption(shot))},
        };
    }
    return list;
}

void fillMetadata(AsComponent *cpt, AppEntry *app)
{
    app->name = utf8(as_component_get_name(cpt));
    app->summary = utf8(as_component_get_summary(cpt));
    app->description = utf8(as_component_get_description(cpt));
    if (AsDeveloper *dev = as_component_get_developer(cpt))
        app->developer = utf8(as_developer_get_name(dev));
    app->license = utf8(as_component_get_project_license(cpt));
    app->homepage = utf8(as_component_get_url(cpt, AS_URL_KIND_HOMEPAGE));
    app->categories.clear();
    if (GPtrArray *cats = as_component_get_categories(cpt)) {
        for (guint i = 0; i < cats->len; ++i)
            app->categories << utf8(static_cast<const gchar *>(g_ptr_array_index(cats, i)));
    }
    QStringList keywords;
    if (GPtrArray *kw = as_component_get_keywords(cpt)) {
        for (guint i = 0; i < kw->len; ++i)
            keywords << utf8(static_cast<const gchar *>(g_ptr_array_index(kw, i)));
    }
    app->screenshots = screenshotsOf(cpt);
    pickIcon(cpt, app);
    app->keywordsFold = fold(keywords.join(QLatin1Char(' ')));
}

QString desktopIdOf(AsComponent *cpt)
{
    if (AsLaunchable *launchable = as_component_get_launchable(cpt, AS_LAUNCHABLE_KIND_DESKTOP_ID)) {
        GPtrArray *entries = as_launchable_get_entries(launchable);
        if (entries && entries->len > 0)
            return utf8(static_cast<const gchar *>(g_ptr_array_index(entries, 0)));
    }
    return {};
}

// The ids ratings may be filed under: the component id, its .desktop launchable
// and the ids it says it provides (renamed apps)
void addReviewIds(AsComponent *cpt, QStringList *ids)
{
    auto add = [ids](const QString &id) {
        if (!id.isEmpty() && !ids->contains(id, Qt::CaseInsensitive))
            ids->append(id);
    };
    add(utf8(as_component_get_id(cpt)));
    add(desktopIdOf(cpt));
    if (AsProvided *provided = as_component_get_provided_for_kind(cpt, AS_PROVIDED_KIND_ID)) {
        GPtrArray *items = as_provided_get_items(provided);
        for (guint i = 0; items && i < items->len; ++i)
            add(utf8(static_cast<const gchar *>(g_ptr_array_index(items, i))));
    }
}

// An app of featured.json "curated": its store entry is written there, because the
// repository it comes from ([archlingmo]) has no AppStream data
struct CuratedApp {
    AppEntry entry;        // metadata, already in the user's language
    QString pkgname;
    QString version;       // the version featured.json describes (fake mode only)
    QString repo;          // the ArchLingmo repository that carries it, when not [archlingmo]
};

struct LoadConfig {
    QList<CuratedApp> curated;
    QStringList fakeRepoPackages; // fake mode: pretend their repository has these ("all": every curated one)
    QStringList extraDirs;
    QString locale;
    QString pacmanConf;
    QHash<QString, QString> aliases; // Flathub key -> Arch package
    QStringList nativePackages;      // packages of the native games collection (any repository)
};

std::shared_ptr<CatalogData> loadCatalog(const LoadConfig &config)
{
    QElapsedTimer timer;
    timer.start();
    auto data = std::make_shared<CatalogData>();

    data->syncRepos = pacmanRepos(config.pacmanConf);
    Pacman pacman(data->syncRepos);
    pacman.installed(&data->localPackages, &data->explicitPackages);

    AsPool *pool = as_pool_new();
    as_pool_set_locale(pool, config.locale.toUtf8().constData());
    as_pool_set_flags(pool, AsPoolFlags(AS_POOL_FLAG_LOAD_OS_CATALOG | AS_POOL_FLAG_LOAD_FLATPAK));
    for (const QString &dir : config.extraDirs)
        as_pool_add_extra_data_location(pool, dir.toUtf8().constData(), AS_FORMAT_STYLE_CATALOG);
    GError *error = nullptr;
    if (!as_pool_load(pool, nullptr, &error)) {
        // Partial data is still useful
        qWarning() << "AppStream:" << (error ? error->message : "load failed");
    }
    if (error)
        g_error_free(error);

    QHash<QString, QString> aliasByPackage; // Arch package -> Flathub key
    for (auto it = config.aliases.cbegin(); it != config.aliases.cend(); ++it)
        aliasByPackage.insert(it.value(), it.key());

    QList<AppEntry> &apps = data->apps;
    QHash<QString, int> &byKey = data->byKey;
    QHash<QString, int> nameIndex; // "<last id segment>|<name>" of Flathub apps

    AsComponentBox *box = as_pool_get_components_by_kind(pool, AS_COMPONENT_KIND_DESKTOP_APP);
    const guint count = as_component_box_len(box);

    // Flathub first: its metadata (icons, screenshots) is usually the richer one
    for (int pass = 0; pass < 2; ++pass) {
        for (guint i = 0; i < count; ++i) {
            AsComponent *cpt = as_component_box_index(box, i);
            AsBundle *bundle = as_component_get_bundle(cpt, AS_BUNDLE_KIND_FLATPAK);
            const QString id = utf8(as_component_get_id(cpt));
            const QString key = keyFor(id);
            if (id.isEmpty())
                continue;

            if (pass == 0) {
                if (!bundle)
                    continue;
                const QString ref = utf8(as_bundle_get_id(bundle));
                if (!ref.startsWith(QLatin1String("app/")) || byKey.contains(key))
                    continue;
                AppEntry app;
                app.key = key;
                app.id = id.endsWith(QLatin1String(".desktop")) ? id.chopped(8) : id;
                fillMetadata(cpt, &app);
                AppSource src;
                src.componentId = id;
                src.flatpakRef = ref;
                src.version = latestVersion(cpt);
                src.desktopId = desktopIdOf(cpt);
                src.verified = qstrcmp(as_component_get_custom_value(cpt, "flathub::verification::verified"), "true") == 0;
                app.flathub = src;
                addReviewIds(cpt, &app.reviewIds);
                byKey.insert(key, apps.size());
                data->byFlatpakId.insert(ref.section(QLatin1Char('/'), 1, 1).toLower(), apps.size());
                const QString lastSegment = key.section(QLatin1Char('.'), -1);
                nameIndex.insert(lastSegment + QLatin1Char('|') + app.name.toLower(), apps.size());
                apps.append(app);
                continue;
            }

            // Arch repositories
            if (bundle)
                continue;
            const QString origin = utf8(as_component_get_origin(cpt));
            const QString pkgname = utf8(as_component_get_pkgname(cpt));
            if (pkgname.isEmpty() || !origin.startsWith(QLatin1String("archlinux-")))
                continue;
            const auto info = pacman.find(pkgname);
            const bool installed = data->localPackages.contains(pkgname);
            if (!info && !installed)
                continue; // e.g. [multilib] disabled
            AppSource src;
            src.componentId = id;
            src.pkgname = pkgname;
            src.desktopId = desktopIdOf(cpt);
            if (info) {
                src.repo = info->repo;
                src.version = info->version;
                src.downloadSize = info->size;
                src.installedSize = info->isize;
            }

            int index = byKey.value(key, -1);
            if (index < 0 && aliasByPackage.contains(pkgname))
                index = byKey.value(aliasByPackage.value(pkgname), -1);
            if (index < 0) {
                // Same app under different ids (e.g. discord.desktop / com.discordapp.Discord)
                index = nameIndex.value(pkgname.toLower() + QLatin1Char('|') + utf8(as_component_get_name(cpt)).toLower(), -1);
            }
            if (index >= 0) {
                AppEntry &app = apps[index];
                if (!app.arch) {
                    app.arch = src;
                    addReviewIds(cpt, &app.reviewIds);
                    if (app.iconUrl.isEmpty() && app.iconName.isEmpty())
                        pickIcon(cpt, &app);
                    data->byPkgname.insert(pkgname, index);
                }
                continue;
            }
            if (byKey.contains(key))
                continue;
            AppEntry app;
            app.key = key;
            app.id = id.endsWith(QLatin1String(".desktop")) ? id.chopped(8) : id;
            fillMetadata(cpt, &app);
            app.arch = src;
            addReviewIds(cpt, &app.reviewIds);
            byKey.insert(key, apps.size());
            if (!data->byPkgname.contains(pkgname))
                data->byPkgname.insert(pkgname, apps.size());
            apps.append(app);
        }
    }

    // Curated aliases whose Arch side has no AppStream entry of its own
    for (auto it = config.aliases.cbegin(); it != config.aliases.cend(); ++it) {
        const int index = byKey.value(it.key(), -1);
        if (index < 0 || apps[index].arch || it.value().isEmpty())
            continue;
        if (const auto info = pacman.find(it.value())) {
            AppSource src;
            src.pkgname = it.value();
            src.repo = info->repo;
            src.version = info->version;
            src.downloadSize = info->size;
            src.installedSize = info->isize;
            apps[index].arch = src;
            data->byPkgname.insert(it.value(), index);
        }
    }

    // Curated apps: offered when a repository has them (or they're installed)
    for (const CuratedApp &curated : config.curated) {
        AppEntry app = curated.entry;
        if (byKey.contains(app.key))
            continue; // AppStream knows it after all: its own data wins
        std::optional<PackageInfo> info = pacman.find(curated.pkgname, true);
        const QString home = curated.repo.isEmpty() ? QStringLiteral("archlingmo") : curated.repo;
        // (a separate repository has to be enabled in the pacman.conf copy first)
        if (!info && (curated.repo.isEmpty() || data->syncRepos.contains(home))
            && (config.fakeRepoPackages.contains(QStringLiteral("all")) || config.fakeRepoPackages.contains(curated.pkgname)))
            info = PackageInfo{home, curated.version + QStringLiteral("-1"), -1, -1};
        AppSource src;
        src.pkgname = curated.pkgname;
        src.componentId = app.id;
        src.desktopId = app.id + QStringLiteral(".desktop");
        if (info) {
            src.repo = info->repo;
            src.version = info->version;
            src.downloadSize = info->size;
            src.installedSize = info->isize;
        } else if (!curated.repo.isEmpty()) {
            // Its own repository isn't enabled yet (or its database not synced): the
            // store offers to enable it (lingmo-store-helper enable-repo)
            src.needsRepo = curated.repo;
            src.version = curated.version;
        }
        app.arch = src;
        byKey.insert(app.key, apps.size());
        data->byPkgname.insert(curated.pkgname, apps.size());
        apps.append(app);
    }

    for (const QString &pkg : config.nativePackages) {
        if (const auto info = pacman.find(pkg, true))
            data->nativePackages.insert(pkg, info->repo + QLatin1Char(' ') + info->version);
    }

    for (AppEntry &app : apps) {
        app.nameFold = fold(app.name);
        app.summaryFold = fold(app.summary);
        app.idFold = fold(app.id + QLatin1Char(' ') + (app.arch ? app.arch->pkgname : QString()));
        app.quality = (app.screenshots.isEmpty() ? 0 : 3) + (app.description.size() > 120 ? 2 : 0)
            + (app.iconUrl.isEmpty() ? 0 : 2) + (app.developer.isEmpty() ? 0 : 1)
            + (app.flathub && app.flathub->verified ? 2 : 0) + (app.arch && app.flathub ? 1 : 0);
        if (app.flathub)
            ++data->flathubCount;
        if (app.arch)
            ++data->archCount;
    }

    g_object_unref(box);
    g_object_unref(pool);
    qInfo().noquote() << QStringLiteral("Catalog: %1 apps (%2 Flathub, %3 Arch) in %4 ms")
                             .arg(apps.size()).arg(data->flathubCount).arg(data->archCount).arg(timer.elapsed());
    return data;
}

} // namespace

Catalog::Catalog(FlatpakBackend *flatpak, QObject *parent)
    : QObject(parent)
    , m_flatpak(flatpak)
    , m_fake(!qEnvironmentVariableIsEmpty("LINGMO_STORE_FAKE"))
{
    loadFeatured();
    connect(m_flatpak, &FlatpakBackend::probed, this, [this](const QHash<QString, QString> &installed) {
        m_flatpakInstalled = installed;
        Q_EMIT installedChanged();
    });
    connect(m_flatpak, &FlatpakBackend::catalogUpdated, this, &Catalog::reload);
    connect(m_flatpak, &FlatpakBackend::sizeFetched, this, [this](const QString &id, qint64 download, qint64 installed) {
        const int index = m_data ? m_data->byFlatpakId.value(id.toLower(), -1) : -1;
        if (index >= 0)
            Q_EMIT sizeReady(m_data->apps.at(index).key,
                             installed > 0 || download > 0 ? tr("About %1").arg(sizeText(qMax(installed, download))) : QString());
    });
}

Catalog::~Catalog() = default;

void Catalog::loadFeatured()
{
    QFile file(QStringLiteral(":/data/featured.json"));
    if (file.open(QIODevice::ReadOnly))
        m_featured = QJsonDocument::fromJson(file.readAll()).object();
    const QJsonObject aliases = m_featured.value(QStringLiteral("aliases")).toObject();
    for (auto it = aliases.constBegin(); it != aliases.constEnd(); ++it)
        m_aliases.insert(keyFor(it.key()), it.value().toString());
}

QString Catalog::pacmanConf() const
{
    if (m_fake && !qEnvironmentVariableIsEmpty("LINGMO_STORE_PACMAN_CONF"))
        return qEnvironmentVariable("LINGMO_STORE_PACMAN_CONF");
    return QStringLiteral("/etc/pacman.conf");
}

bool Catalog::repoEnabled(const QString &repo) const
{
    return m_data && m_data->syncRepos.contains(repo);
}

bool Catalog::multilibEnabled() const
{
    return m_data && m_data->syncRepos.contains(QStringLiteral("multilib"));
}

void Catalog::reload()
{
    if (m_loading) {
        m_reloadPending = true;
        return;
    }
    m_loading = true;
    Q_EMIT loadingChanged();

    LoadConfig config;
    config.extraDirs = m_flatpak->extraAppstreamDirs();
    config.locale = QLocale().name();
    config.pacmanConf = pacmanConf() == QLatin1String("/etc/pacman.conf") ? QString() : pacmanConf();
    config.aliases = m_aliases;
    for (const QJsonValue &v : m_featured.value(QStringLiteral("curated")).toArray()) {
        const QJsonObject o = v.toObject();
        CuratedApp c;
        c.pkgname = o.value(QStringLiteral("pkg")).toString();
        c.version = o.value(QStringLiteral("version")).toString();
        c.repo = o.value(QStringLiteral("repo")).toString();
        AppEntry &app = c.entry;
        app.id = o.value(QStringLiteral("id")).toString();
        app.key = keyFor(app.id);
        if (c.pkgname.isEmpty() || app.key.isEmpty())
            continue;
        app.curated = true;
        app.name = o.value(QStringLiteral("name")).toString();
        app.summary = localized(o.value(QStringLiteral("summary")));
        app.description = localized(o.value(QStringLiteral("description")));
        app.developer = o.value(QStringLiteral("developer")).toString();
        app.license = o.value(QStringLiteral("license")).toString();
        app.homepage = o.value(QStringLiteral("homepage")).toString();
        app.iconUrl = o.value(QStringLiteral("icon")).toString();
        app.iconName = o.value(QStringLiteral("iconName")).toString();
        app.experimental = o.value(QStringLiteral("experimental")).toBool();
        app.replaces = localized(o.value(QStringLiteral("replaces")));
        app.reviewIds = {app.id, app.id + QStringLiteral(".desktop")};
        for (const QJsonValue &cat : o.value(QStringLiteral("categories")).toArray())
            app.categories << cat.toString();
        for (const QJsonValue &type : o.value(QStringLiteral("opens")).toArray())
            app.opens << type.toString();
        for (const QJsonValue &shot : o.value(QStringLiteral("screenshots")).toArray()) {
            const QJsonObject so = shot.toObject();
            app.screenshots << QVariantMap{
                {QStringLiteral("thumbnail"), so.value(QStringLiteral("url")).toString()},
                {QStringLiteral("source"), so.value(QStringLiteral("url")).toString()},
                {QStringLiteral("caption"), localized(so.value(QStringLiteral("caption")))},
            };
        }
        // Both languages: "editor de texto" and "word processor" find it
        const QJsonObject kw = o.value(QStringLiteral("keywords")).toObject();
        QStringList words;
        for (auto it = kw.constBegin(); it != kw.constEnd(); ++it)
            words << it.value().toString();
        app.keywordsFold = fold(words.join(QLatin1Char(' ')) + QLatin1Char(' ') + app.opens.join(QLatin1Char(' '))
                                + QLatin1Char(' ') + app.replaces);
        config.curated << c;
    }
    if (m_fake)
        config.fakeRepoPackages = qEnvironmentVariable("LINGMO_STORE_FAKE_ARCHLINGMO").split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QJsonValue &v : m_featured.value(QStringLiteral("nativeGames")).toArray()) {
        const QString pkg = v.toObject().value(QStringLiteral("pkg")).toString();
        if (!pkg.isEmpty())
            config.nativePackages << pkg;
    }

    QPointer<Catalog> self(this);
    QThread *thread = QThread::create([self, config] {
        auto data = loadCatalog(config);
        QMetaObject::invokeMethod(qApp, [self, data] {
            if (!self)
                return;
            self->m_data = data;
            self->m_localPackages = data->localPackages;
            self->m_explicitPackages = data->explicitPackages;
            self->m_loading = false;
            Q_EMIT self->loadingChanged();
            Q_EMIT self->changed();
            Q_EMIT self->installedChanged();
            if (self->m_reloadPending) {
                self->m_reloadPending = false;
                self->reload();
            }
        }, Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void Catalog::refreshInstalled()
{
    m_flatpak->probe();
    QPointer<Catalog> self(this);
    QThread *thread = QThread::create([self] {
        QSet<QString> all, explicitly;
        Pacman({}).installed(&all, &explicitly);
        QMetaObject::invokeMethod(qApp, [self, all, explicitly] {
            if (!self)
                return;
            self->m_localPackages = all;
            self->m_explicitPackages = explicitly;
            Q_EMIT self->installedChanged();
        }, Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void Catalog::setFakeInstalled(const QString &key, const QString &source, bool installed)
{
    const AppEntry *app = entry(key);
    if (!app)
        return;
    if (source == QLatin1String("arch") && app->arch) {
        if (installed) {
            m_fakeArch.insert(app->arch->pkgname);
            m_fakeArchRemoved.remove(app->arch->pkgname);
        } else {
            m_fakeArch.remove(app->arch->pkgname);
            m_fakeArchRemoved.insert(app->arch->pkgname);
        }
    } else if (source == QLatin1String("flathub") && app->flathub) {
        const QString id = app->flathub->flatpakRef.section(QLatin1Char('/'), 1, 1);
        if (installed) {
            m_fakeFlatpak.insert(id, m_flatpak->userInstallation() ? QStringLiteral("user") : QStringLiteral("system"));
            m_fakeFlatpakRemoved.remove(id);
        } else {
            m_fakeFlatpak.remove(id);
            m_fakeFlatpakRemoved.insert(id);
        }
    }
    Q_EMIT installedChanged();
}

void Catalog::setFakeFlatpakInstalled(const QString &id, bool installed)
{
    if (installed) {
        m_fakeFlatpak.insert(id, m_flatpak->userInstallation() ? QStringLiteral("user") : QStringLiteral("system"));
        m_fakeFlatpakRemoved.remove(id);
    } else {
        m_fakeFlatpak.remove(id);
        m_fakeFlatpakRemoved.insert(id);
    }
    Q_EMIT installedChanged();
}

void Catalog::setFakePackageInstalled(const QString &pkgname, bool installed)
{
    if (installed) {
        m_fakeArch.insert(pkgname);
        m_fakeArchRemoved.remove(pkgname);
    } else {
        m_fakeArch.remove(pkgname);
        m_fakeArchRemoved.insert(pkgname);
    }
    Q_EMIT installedChanged();
}

const AppEntry *Catalog::entry(const QString &key) const
{
    if (!m_data)
        return nullptr;
    const int index = m_data->byKey.value(key, -1);
    return index >= 0 ? &m_data->apps.at(index) : nullptr;
}

bool Catalog::isPackageInstalled(const QString &pkgname) const
{
    if (m_fakeArchRemoved.contains(pkgname))
        return false;
    return m_localPackages.contains(pkgname) || m_fakeArch.contains(pkgname);
}

bool Catalog::isFlatpakInstalled(const QString &id) const
{
    return !flatpakInstallation(id).isEmpty();
}

QString Catalog::flatpakInstallation(const QString &id) const
{
    if (m_fakeFlatpakRemoved.contains(id))
        return {};
    return m_flatpakInstalled.value(id, m_fakeFlatpak.value(id));
}

QString Catalog::resolve(const QString &input) const
{
    if (!m_data)
        return {};
    QString query = input.trimmed();
    if (query.startsWith(QLatin1String("appstream:"), Qt::CaseInsensitive)) {
        query = query.mid(10);
        while (query.startsWith(QLatin1Char('/')))
            query.remove(0, 1);
    }
    // Flatpak refs: app/<id>/<arch>/<branch>
    if (query.startsWith(QLatin1String("app/")))
        query = query.section(QLatin1Char('/'), 1, 1);

    const QString key = keyFor(query);
    if (m_data->byKey.contains(key))
        return key;
    if (const int i = m_data->byFlatpakId.value(key, -1); i >= 0)
        return m_data->apps.at(i).key;
    if (const int i = m_data->byPkgname.value(query, -1); i >= 0)
        return m_data->apps.at(i).key;
    if (const int i = m_data->byPkgname.value(query.toLower(), -1); i >= 0)
        return m_data->apps.at(i).key;
    // Last resort: an exact name ("Firefox")
    const QString name = fold(query);
    for (const AppEntry &app : m_data->apps) {
        if (app.nameFold == name)
            return app.key;
    }
    return {};
}

QVariantMap Catalog::summaryMap(const AppEntry &app) const
{
    const bool archInstalled = app.arch && isPackageInstalled(app.arch->pkgname);
    const bool flatpakInstalled = app.flathub && isFlatpakInstalled(app.flathub->flatpakRef.section(QLatin1Char('/'), 1, 1));
    const bool archAvailable = app.arch && app.arch->offered();
    return {
        {QStringLiteral("key"), app.key},
        {QStringLiteral("id"), app.id},
        {QStringLiteral("name"), app.name},
        {QStringLiteral("summary"), app.summary},
        {QStringLiteral("iconUrl"), app.iconUrl},
        {QStringLiteral("iconName"), app.iconName},
        {QStringLiteral("hasArch"), archAvailable || archInstalled},
        {QStringLiteral("hasFlathub"), bool(app.flathub)},
        {QStringLiteral("verified"), app.flathub && app.flathub->verified},
        {QStringLiteral("installed"), archInstalled || flatpakInstalled},
        {QStringLiteral("experimental"), app.experimental},
        {QStringLiteral("needsRepo"), app.arch && app.arch->repo.isEmpty() ? app.arch->needsRepo : QString()},
    };
}

QVariantMap Catalog::app(const QString &key) const
{
    const AppEntry *app = entry(key);
    if (!app)
        return {};
    QVariantMap map = summaryMap(*app);
    map.insert(QStringLiteral("description"), app->description);
    map.insert(QStringLiteral("developer"), app->developer);
    map.insert(QStringLiteral("license"), app->license);
    const bool proprietary = app->license.isEmpty() || app->license.contains(QLatin1String("proprietary"), Qt::CaseInsensitive);
    map.insert(QStringLiteral("licenseFree"), !proprietary && as_license_is_free_license(app->license.toUtf8().constData()));
    map.insert(QStringLiteral("homepage"), app->homepage);
    map.insert(QStringLiteral("screenshots"), app->screenshots);
    map.insert(QStringLiteral("categories"), app->categories);
    map.insert(QStringLiteral("curated"), app->curated);
    map.insert(QStringLiteral("opens"), app->opens);
    map.insert(QStringLiteral("replaces"), app->replaces);

    QString recommended;
    if (app->arch) {
        const AppSource &a = *app->arch;
        const bool installed = isPackageInstalled(a.pkgname);
        map.insert(QStringLiteral("arch"), QVariantMap{
            {QStringLiteral("available"), a.offered()},
            {QStringLiteral("installed"), installed},
            {QStringLiteral("version"), a.version},
            {QStringLiteral("repo"), a.repo.isEmpty() ? a.needsRepo : a.repo},
            // set while that repository still has to be enabled: "Enable and Install"
            {QStringLiteral("needsRepo"), a.repo.isEmpty() ? a.needsRepo : QString()},
            {QStringLiteral("pkgname"), a.pkgname},
            {QStringLiteral("downloadSize"), sizeText(a.downloadSize)},
            {QStringLiteral("installedSize"), sizeText(a.installedSize)},
            {QStringLiteral("protected"), ProtectedPackages.contains(a.pkgname) || a.pkgname.startsWith(QLatin1String("lingmo"))},
        });
    }
    if (app->flathub) {
        const AppSource &f = *app->flathub;
        const QString id = f.flatpakRef.section(QLatin1Char('/'), 1, 1);
        map.insert(QStringLiteral("flathub"), QVariantMap{
            {QStringLiteral("available"), m_flatpak->state() == FlatpakBackend::Ready || m_flatpak->state() == FlatpakBackend::Refreshing
                                             || m_flatpak->state() == FlatpakBackend::NoRemote},
            {QStringLiteral("installed"), isFlatpakInstalled(id)},
            {QStringLiteral("installation"), flatpakInstallation(id)},
            {QStringLiteral("version"), f.version},
            {QStringLiteral("verified"), f.verified},
            {QStringLiteral("flatpakId"), id},
            {QStringLiteral("ref"), f.flatpakRef},
        });
    }

    // Which one the store suggests: whatever is installed, else the Arch package when
    // it is in the official repositories and not older than Flathub's
    const bool archOk = app->arch && app->arch->offered();
    const bool archInstalled = app->arch && isPackageInstalled(app->arch->pkgname);
    const bool flatInstalled = app->flathub && isFlatpakInstalled(app->flathub->flatpakRef.section(QLatin1Char('/'), 1, 1));
    if (archInstalled && !flatInstalled) {
        recommended = QStringLiteral("arch");
    } else if (flatInstalled && !archInstalled) {
        recommended = QStringLiteral("flathub");
    } else if (archOk && app->flathub) {
        QString archVersion = app->arch->version.section(QLatin1Char(':'), -1); // epoch
        archVersion = archVersion.section(QLatin1Char('-'), 0, 0);             // pkgrel
        const QString flatVersion = app->flathub->version;
        const bool archCurrent = flatVersion.isEmpty()
            || as_vercmp_simple(archVersion.toUtf8().constData(), flatVersion.toUtf8().constData()) >= 0;
        recommended = archCurrent ? QStringLiteral("arch") : QStringLiteral("flathub");
        map.insert(QStringLiteral("archOutdated"), !archCurrent);
    } else if (archOk || archInstalled) {
        recommended = QStringLiteral("arch");
    } else if (app->flathub) {
        recommended = QStringLiteral("flathub");
    }
    map.insert(QStringLiteral("recommended"), recommended);
    const QVariantMap native = nativeGameFor(key);
    if (!native.isEmpty())
        map.insert(QStringLiteral("nativeGame"), native);
    return map;
}

QVariantList Catalog::listFor(const QStringList &ids) const
{
    QVariantList list;
    for (const QString &id : ids) {
        const QString key = resolve(id);
        if (const AppEntry *app = entry(key))
            list << summaryMap(*app);
    }
    return list;
}

QString Catalog::localized(const QJsonValue &value) const
{
    if (value.isString())
        return value.toString();
    const QJsonObject texts = value.toObject();
    const QLocale locale;
    for (const QString &name : {locale.name(), QLocale::languageToCode(locale.language()), QStringLiteral("en")}) {
        if (texts.contains(name))
            return texts.value(name).toString();
    }
    return {};
}

QVariantList Catalog::featured() const
{
    QStringList ids;
    for (const QJsonValue &v : m_featured.value(QStringLiteral("featured")).toArray())
        ids << v.toString();
    return listFor(ids);
}

QVariantList Catalog::gameApps() const
{
    QStringList ids;
    for (const QJsonValue &v : m_featured.value(QStringLiteral("games")).toArray())
        ids << v.toString();
    return listFor(ids);
}

QVariantList Catalog::heroes() const
{
    QVariantList list;
    for (const QJsonValue &v : m_featured.value(QStringLiteral("hero")).toArray()) {
        const QJsonObject hero = v.toObject();
        const AppEntry *app = entry(resolve(hero.value(QStringLiteral("id")).toString()));
        if (!app)
            continue;
        QVariantMap map = summaryMap(*app);
        map.insert(QStringLiteral("title"), localized(hero.value(QStringLiteral("title"))));
        map.insert(QStringLiteral("text"), localized(hero.value(QStringLiteral("text"))));
        map.insert(QStringLiteral("colors"), hero.value(QStringLiteral("colors")).toVariant());
        list << map;
    }
    return list;
}

QVariantList Catalog::collections() const
{
    QVariantList list;
    if (!m_data)
        return list;
    for (const QJsonValue &v : m_featured.value(QStringLiteral("collections")).toArray()) {
        const QJsonObject c = v.toObject();
        QStringList ids;
        for (const QJsonValue &id : c.value(QStringLiteral("apps")).toArray())
            ids << id.toString();
        QVariantList apps;
        for (const QVariant &app : listFor(ids)) {
            if (app.toMap().value(QStringLiteral("hasArch")).toBool() || app.toMap().value(QStringLiteral("hasFlathub")).toBool())
                apps << app;
        }
        if (apps.isEmpty())
            continue; // nothing installable yet (e.g. not published to the repository)
        QVariantMap map{
            {QStringLiteral("id"), c.value(QStringLiteral("id")).toString()},
            {QStringLiteral("title"), localized(c.value(QStringLiteral("title")))},
            {QStringLiteral("text"), localized(c.value(QStringLiteral("text")))},
            {QStringLiteral("colors"), c.value(QStringLiteral("colors")).toVariant()},
            {QStringLiteral("experimental"), c.value(QStringLiteral("experimental")).toBool()},
            {QStringLiteral("apps"), apps},
        };
        // One click for several apps: their Arch packages in one transaction
        const QJsonObject bundle = c.value(QStringLiteral("bundle")).toObject();
        if (!bundle.isEmpty()) {
            QStringList packages, missing, names;
            QString needsRepo;
            bool available = true;
            for (const QJsonValue &id : bundle.value(QStringLiteral("apps")).toArray()) {
                const AppEntry *app = entry(resolve(id.toString()));
                if (!app || !app->arch || !app->arch->offered()) {
                    available = false;
                    continue;
                }
                if (app->arch->repo.isEmpty())
                    needsRepo = app->arch->needsRepo;
                packages << app->arch->pkgname;
                names << app->name;
                if (!isPackageInstalled(app->arch->pkgname))
                    missing << app->arch->pkgname;
            }
            map.insert(QStringLiteral("bundle"), QVariantMap{
                {QStringLiteral("label"), localized(bundle.value(QStringLiteral("label")))},
                {QStringLiteral("name"), localized(bundle.value(QStringLiteral("name")))},
                {QStringLiteral("apps"), names},
                {QStringLiteral("packages"), missing},
                {QStringLiteral("available"), available && !packages.isEmpty()},
                {QStringLiteral("installed"), !packages.isEmpty() && missing.isEmpty()},
                {QStringLiteral("needsRepo"), needsRepo},
            });
        }
        list << map;
    }
    return list;
}

QVariantList Catalog::category(const QString &category) const
{
    if (!m_data)
        return {};
    static const QHash<QString, QStringList> groups = {
        {QStringLiteral("office"), {QStringLiteral("Office")}},
        {QStringLiteral("internet"), {QStringLiteral("Network")}},
        {QStringLiteral("graphics"), {QStringLiteral("Graphics")}},
        {QStringLiteral("engineering"), {QStringLiteral("Engineering")}},
        {QStringLiteral("multimedia"), {QStringLiteral("AudioVideo"), QStringLiteral("Audio"), QStringLiteral("Video")}},
        {QStringLiteral("games"), {QStringLiteral("Game")}},
        {QStringLiteral("development"), {QStringLiteral("Development")}},
        {QStringLiteral("education"), {QStringLiteral("Education"), QStringLiteral("Science")}},
        {QStringLiteral("utilities"), {QStringLiteral("Utility"), QStringLiteral("System")}},
    };
    const QStringList wanted = groups.value(category);
    QSet<QString> featuredKeys;
    for (const QJsonValue &v : m_featured.value(QStringLiteral("featured")).toArray())
        featuredKeys.insert(keyFor(v.toString()));
    for (const QJsonValue &v : m_featured.value(QStringLiteral("games")).toArray())
        featuredKeys.insert(keyFor(v.toString()));

    QList<const AppEntry *> matches;
    for (const AppEntry &app : m_data->apps) {
        if (!app.flathub && !(app.arch && app.arch->offered()))
            continue;
        if (std::any_of(wanted.cbegin(), wanted.cend(), [&](const QString &c) { return app.categories.contains(c); }))
            matches << &app;
    }
    std::sort(matches.begin(), matches.end(), [&](const AppEntry *a, const AppEntry *b) {
        const int fa = featuredKeys.contains(a->key), fb = featuredKeys.contains(b->key);
        if (fa != fb)
            return fa > fb;
        if (a->quality != b->quality)
            return a->quality > b->quality;
        return a->name.localeAwareCompare(b->name) < 0;
    });
    QVariantList list;
    list.reserve(matches.size());
    for (const AppEntry *app : std::as_const(matches))
        list << summaryMap(*app);
    return list;
}

// Everyday Portuguese words -> the English words AppStream metadata mostly uses
static const QHash<QString, QStringList> &searchSynonyms()
{
    static const QHash<QString, QStringList> synonyms = {
        {QStringLiteral("foto"), {QStringLiteral("photo"), QStringLiteral("image"), QStringLiteral("picture")}},
        {QStringLiteral("fotos"), {QStringLiteral("photo"), QStringLiteral("image"), QStringLiteral("picture")}},
        {QStringLiteral("imagem"), {QStringLiteral("image"), QStringLiteral("photo"), QStringLiteral("picture")}},
        {QStringLiteral("imagens"), {QStringLiteral("image"), QStringLiteral("photo"), QStringLiteral("picture")}},
        {QStringLiteral("musica"), {QStringLiteral("music"), QStringLiteral("audio")}},
        {QStringLiteral("musicas"), {QStringLiteral("music"), QStringLiteral("audio")}},
        {QStringLiteral("video"), {QStringLiteral("video"), QStringLiteral("movie")}},
        {QStringLiteral("videos"), {QStringLiteral("video"), QStringLiteral("movie")}},
        {QStringLiteral("filme"), {QStringLiteral("movie"), QStringLiteral("video")}},
        {QStringLiteral("filmes"), {QStringLiteral("movie"), QStringLiteral("video")}},
        {QStringLiteral("jogo"), {QStringLiteral("game")}},
        {QStringLiteral("jogos"), {QStringLiteral("game")}},
        {QStringLiteral("navegador"), {QStringLiteral("browser"), QStringLiteral("web")}},
        {QStringLiteral("internet"), {QStringLiteral("web"), QStringLiteral("browser")}},
        {QStringLiteral("planilha"), {QStringLiteral("spreadsheet")}},
        {QStringLiteral("planilhas"), {QStringLiteral("spreadsheet")}},
        {QStringLiteral("texto"), {QStringLiteral("text"), QStringLiteral("word"), QStringLiteral("document")}},
        {QStringLiteral("documento"), {QStringLiteral("document"), QStringLiteral("word")}},
        {QStringLiteral("documentos"), {QStringLiteral("document"), QStringLiteral("word")}},
        {QStringLiteral("apresentacao"), {QStringLiteral("presentation"), QStringLiteral("slides")}},
        {QStringLiteral("apresentacoes"), {QStringLiteral("presentation"), QStringLiteral("slides")}},
        {QStringLiteral("escritorio"), {QStringLiteral("office")}},
        {QStringLiteral("desenho"), {QStringLiteral("drawing"), QStringLiteral("paint")}},
        {QStringLiteral("desenhar"), {QStringLiteral("drawing"), QStringLiteral("paint")}},
        {QStringLiteral("pintura"), {QStringLiteral("paint"), QStringLiteral("painting")}},
        {QStringLiteral("gravar"), {QStringLiteral("record"), QStringLiteral("recorder"), QStringLiteral("burn")}},
        {QStringLiteral("gravador"), {QStringLiteral("recorder"), QStringLiteral("record")}},
        {QStringLiteral("tela"), {QStringLiteral("screen")}},
        {QStringLiteral("conversa"), {QStringLiteral("chat"), QStringLiteral("messaging")}},
        {QStringLiteral("mensagem"), {QStringLiteral("messaging"), QStringLiteral("messenger"), QStringLiteral("chat")}},
        {QStringLiteral("mensagens"), {QStringLiteral("messaging"), QStringLiteral("messenger"), QStringLiteral("chat")}},
        {QStringLiteral("correio"), {QStringLiteral("mail"), QStringLiteral("email")}},
        {QStringLiteral("programacao"), {QStringLiteral("programming"), QStringLiteral("code"), QStringLiteral("ide")}},
        {QStringLiteral("programar"), {QStringLiteral("programming"), QStringLiteral("code"), QStringLiteral("ide")}},
        {QStringLiteral("codigo"), {QStringLiteral("code"), QStringLiteral("programming")}},
        {QStringLiteral("calculadora"), {QStringLiteral("calculator")}},
        {QStringLiteral("mapa"), {QStringLiteral("map")}},
        {QStringLiteral("mapas"), {QStringLiteral("map")}},
        {QStringLiteral("senha"), {QStringLiteral("password")}},
        {QStringLiteral("senhas"), {QStringLiteral("password")}},
        {QStringLiteral("compactar"), {QStringLiteral("archive"), QStringLiteral("zip"), QStringLiteral("compress")}},
        {QStringLiteral("baixar"), {QStringLiteral("download")}},
        {QStringLiteral("edicao"), {QStringLiteral("editing"), QStringLiteral("editor")}},
        {QStringLiteral("editar"), {QStringLiteral("editor"), QStringLiteral("editing")}},
        {QStringLiteral("leitor"), {QStringLiteral("reader"), QStringLiteral("viewer")}},
        {QStringLiteral("visualizador"), {QStringLiteral("viewer")}},
        {QStringLiteral("camera"), {QStringLiteral("camera"), QStringLiteral("webcam")}},
        {QStringLiteral("reuniao"), {QStringLiteral("meeting"), QStringLiteral("video call"), QStringLiteral("conference")}},
        {QStringLiteral("chamada"), {QStringLiteral("call")}},
        {QStringLiteral("emulador"), {QStringLiteral("emulator")}},
        {QStringLiteral("livro"), {QStringLiteral("book"), QStringLiteral("ebook")}},
        {QStringLiteral("livros"), {QStringLiteral("book"), QStringLiteral("ebook")}},
        {QStringLiteral("anotacoes"), {QStringLiteral("notes")}},
        {QStringLiteral("notas"), {QStringLiteral("notes")}},
    };
    return synonyms;
}

QVariantList Catalog::search(const QString &text, int limit) const
{
    if (!m_data)
        return {};
    const QString query = fold(text).simplified();
    if (query.isEmpty())
        return {};
    // Little words only get in the way ("editor de fotos")
    static const QSet<QString> stopWords = {
        QStringLiteral("de"), QStringLiteral("da"), QStringLiteral("do"), QStringLiteral("das"), QStringLiteral("dos"),
        QStringLiteral("e"), QStringLiteral("o"), QStringLiteral("a"), QStringLiteral("os"), QStringLiteral("as"),
        QStringLiteral("para"), QStringLiteral("pra"), QStringLiteral("com"), QStringLiteral("um"), QStringLiteral("uma"),
        QStringLiteral("no"), QStringLiteral("na"), QStringLiteral("em"), QStringLiteral("the"), QStringLiteral("of"),
        QStringLiteral("for"), QStringLiteral("and"), QStringLiteral("app"), QStringLiteral("programa"),
    };
    QStringList tokens;
    for (const QString &token : query.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        if (!stopWords.contains(token))
            tokens << token;
    }
    if (tokens.isEmpty())
        tokens = query.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    QSet<QString> featuredKeys;
    for (const QJsonValue &v : m_featured.value(QStringLiteral("featured")).toArray())
        featuredKeys.insert(keyFor(v.toString()));

    auto wordStart = [](const QString &haystack, const QString &needle) {
        int from = 0;
        while ((from = haystack.indexOf(needle, from)) >= 0) {
            if (from == 0 || !haystack.at(from - 1).isLetterOrNumber())
                return true;
            ++from;
        }
        return false;
    };
    // Words that also name a kind of app
    static const QHash<QString, QStringList> categoryHints = {
        {QStringLiteral("foto"), {QStringLiteral("Photography"), QStringLiteral("RasterGraphics")}},
        {QStringLiteral("fotos"), {QStringLiteral("Photography"), QStringLiteral("RasterGraphics")}},
        {QStringLiteral("imagem"), {QStringLiteral("RasterGraphics"), QStringLiteral("Photography")}},
        {QStringLiteral("imagens"), {QStringLiteral("RasterGraphics"), QStringLiteral("Photography")}},
        {QStringLiteral("photo"), {QStringLiteral("Photography"), QStringLiteral("RasterGraphics")}},
        {QStringLiteral("musica"), {QStringLiteral("Music"), QStringLiteral("Player")}},
        {QStringLiteral("music"), {QStringLiteral("Music"), QStringLiteral("Player")}},
        {QStringLiteral("video"), {QStringLiteral("Video")}},
        {QStringLiteral("videos"), {QStringLiteral("Video")}},
        {QStringLiteral("jogo"), {QStringLiteral("Game")}},
        {QStringLiteral("jogos"), {QStringLiteral("Game")}},
        {QStringLiteral("game"), {QStringLiteral("Game")}},
        {QStringLiteral("navegador"), {QStringLiteral("WebBrowser")}},
        {QStringLiteral("browser"), {QStringLiteral("WebBrowser")}},
        {QStringLiteral("planilha"), {QStringLiteral("Spreadsheet")}},
        {QStringLiteral("escritorio"), {QStringLiteral("Office")}},
        {QStringLiteral("office"), {QStringLiteral("Office")}},
        {QStringLiteral("programacao"), {QStringLiteral("IDE"), QStringLiteral("Development")}},
        {QStringLiteral("email"), {QStringLiteral("Email")}},
        {QStringLiteral("editor"), {QStringLiteral("TextEditor"), QStringLiteral("RasterGraphics"), QStringLiteral("VectorGraphics"), QStringLiteral("AudioVideoEditing")}},
    };
    auto inCategory = [](const AppEntry &app, const QString &token) {
        const QStringList hints = categoryHints.value(token);
        return std::any_of(hints.cbegin(), hints.cend(), [&](const QString &c) { return app.categories.contains(c); });
    };
    auto tokenScore = [&](const AppEntry &app, const QString &token, bool synonym) {
        if (!synonym) {
            if (app.nameFold.startsWith(token))
                return 40;
            if (wordStart(app.nameFold, token))
                return 30;
            if (token.size() > 2 && app.nameFold.contains(token))
                return 18;
        } else if (wordStart(app.nameFold, token)) {
            return 14;
        }
        if (wordStart(app.keywordsFold, token))
            return synonym ? 8 : 12;
        if (!synonym && app.idFold.contains(token))
            return 8;
        if (wordStart(app.summaryFold, token))
            return synonym ? 5 : 6;
        if (!synonym && token.size() > 3 && app.summaryFold.contains(token))
            return 3;
        return 0;
    };

    QList<std::pair<int, const AppEntry *>> scored;
    for (const AppEntry &app : m_data->apps) {
        if (!app.flathub && !(app.arch && app.arch->offered()) && !(app.arch && isPackageInstalled(app.arch->pkgname)))
            continue;
        int score = 0;
        int matched = 0;
        for (const QString &token : tokens) {
            int best = tokenScore(app, token, false);
            if (best == 0) {
                for (const QString &synonym : searchSynonyms().value(token))
                    best = qMax(best, tokenScore(app, synonym, true));
            }
            if (inCategory(app, token))
                best = best > 0 ? best + 10 : 6;
            if (best > 0) {
                ++matched;
                score += best;
            }
        }
        // Every word must match; with several words, one may miss
        const int n = tokens.size();
        if (matched == 0 || (matched < n && (n < 2 || matched < n - 1)))
            continue;
        if (matched == n)
            score += 25;
        else
            score /= 2; // a word missed: behind the full matches
        if (app.nameFold == query)
            score += 100;
        if (featuredKeys.contains(app.key))
            score += 15;
        score += app.quality;
        scored.append({score, &app});
    }
    std::sort(scored.begin(), scored.end(), [](const auto &a, const auto &b) {
        if (a.first != b.first)
            return a.first > b.first;
        return a.second->name.localeAwareCompare(b.second->name) < 0;
    });
    QVariantList list;
    for (int i = 0; i < scored.size() && i < limit; ++i)
        list << summaryMap(*scored.at(i).second);
    return list;
}

QVariantList Catalog::installedApps() const
{
    QVariantList list;
    if (!m_data)
        return list;
    QSet<QString> seenFlatpaks;
    QList<const AppEntry *> apps;
    for (const AppEntry &app : m_data->apps) {
        const bool arch = app.arch && isPackageInstalled(app.arch->pkgname);
        const QString flatpakId = app.flathub ? app.flathub->flatpakRef.section(QLatin1Char('/'), 1, 1) : QString();
        const bool flatpak = !flatpakId.isEmpty() && isFlatpakInstalled(flatpakId);
        if (flatpak)
            seenFlatpaks.insert(flatpakId);
        // Only what the user installed: packages pulled in as dependencies aren't apps to them
        if (flatpak || (arch && (m_explicitPackages.contains(app.arch->pkgname) || m_fakeArch.contains(app.arch->pkgname))))
            apps << &app;
    }
    std::sort(apps.begin(), apps.end(), [](const AppEntry *a, const AppEntry *b) {
        return a->name.localeAwareCompare(b->name) < 0;
    });
    for (const AppEntry *app : std::as_const(apps)) {
        QVariantMap map = summaryMap(*app);
        const bool arch = app->arch && isPackageInstalled(app->arch->pkgname);
        const QString flatpakId = app->flathub ? app->flathub->flatpakRef.section(QLatin1Char('/'), 1, 1) : QString();
        map.insert(QStringLiteral("installedArch"), arch);
        map.insert(QStringLiteral("installedFlathub"), !flatpakId.isEmpty() && isFlatpakInstalled(flatpakId));
        map.insert(QStringLiteral("protected"), app->arch && (ProtectedPackages.contains(app->arch->pkgname)
                                                              || app->arch->pkgname.startsWith(QLatin1String("lingmo"))));
        map.insert(QStringLiteral("size"), arch ? sizeText(app->arch->installedSize) : QString());
        list << map;
    }
    return list;
}

QVariantList Catalog::nativeGames() const
{
    QVariantList list;
    for (const QJsonValue &v : m_featured.value(QStringLiteral("nativeGames")).toArray()) {
        const QJsonObject game = v.toObject();
        const QString source = game.value(QStringLiteral("source")).toString();
        QVariantMap map{
            {QStringLiteral("name"), game.value(QStringLiteral("name")).toString()},
            {QStringLiteral("console"), game.value(QStringLiteral("console")).toString()},
            {QStringLiteral("project"), game.value(QStringLiteral("project")).toString()},
            {QStringLiteral("source"), source},
            {QStringLiteral("needsOwnCopy"), game.value(QStringLiteral("needsOwnCopy")).toBool()},
            {QStringLiteral("note"), localized(game.value(QStringLiteral("note")))},
        };
        if (source == QLatin1String("flathub")) {
            const AppEntry *app = entry(resolve(game.value(QStringLiteral("id")).toString()));
            if (!app || !app->flathub)
                continue; // not in the catalog (yet): nothing to show
            map.insert(QStringLiteral("key"), app->key);
            map.insert(QStringLiteral("iconUrl"), app->iconUrl);
            map.insert(QStringLiteral("iconName"), app->iconName);
            map.insert(QStringLiteral("available"), true);
            map.insert(QStringLiteral("installed"), isFlatpakInstalled(app->flathub->flatpakRef.section(QLatin1Char('/'), 1, 1)));
        } else {
            const QString pkg = game.value(QStringLiteral("pkg")).toString();
            QString where = m_data ? m_data->nativePackages.value(pkg) : QString();
            // Testing: LINGMO_STORE_FAKE_NATIVE=soh,zelda3 pretends [archlingmo] has them
            if (where.isEmpty() && m_fake
                && qEnvironmentVariable("LINGMO_STORE_FAKE_NATIVE").split(QLatin1Char(',')).contains(pkg))
                where = QStringLiteral("archlingmo 1.0-1");
            map.insert(QStringLiteral("key"), QStringLiteral("pkg:") + pkg);
            map.insert(QStringLiteral("pkg"), pkg);
            map.insert(QStringLiteral("available"), !where.isEmpty());
            map.insert(QStringLiteral("repo"), where.section(QLatin1Char(' '), 0, 0));
            map.insert(QStringLiteral("version"), where.section(QLatin1Char(' '), 1));
            map.insert(QStringLiteral("installed"), isPackageInstalled(pkg));
        }
        list << map;
    }
    return list;
}

QVariantMap Catalog::nativeGameFor(const QString &key) const
{
    for (const QJsonValue &v : m_featured.value(QStringLiteral("nativeGames")).toArray()) {
        const QJsonObject game = v.toObject();
        if (game.contains(QStringLiteral("id")) && keyFor(game.value(QStringLiteral("id")).toString()) == key) {
            return {
                {QStringLiteral("needsOwnCopy"), game.value(QStringLiteral("needsOwnCopy")).toBool()},
                {QStringLiteral("note"), localized(game.value(QStringLiteral("note")))},
                {QStringLiteral("console"), game.value(QStringLiteral("console")).toString()},
            };
        }
    }
    return {};
}

void Catalog::fetchFlathubSize(const QString &key)
{
    const AppEntry *app = entry(key);
    if (app && app->flathub)
        m_flatpak->fetchSize(app->flathub->flatpakRef.section(QLatin1Char('/'), 1, 1));
}
