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

#include "storeapp.h"
#include "catalog.h"
#include "flatpakbackend.h"
#include "gamecompat.h"
#include "gamesmanager.h"
#include "imageproviders.h"
#include "installer.h"
#include "lutrissource.h"
#include "permissions.h"
#include "reviews.h"

#include <QClipboard>
#include <QCommandLineParser>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDebug>
#include <QDesktopServices>
#include <QFile>
#include <QFontDatabase>
#include <QFontInfo>
#include <QIcon>
#include <QGuiApplication>
#include <QLocale>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>

#include <KWindowSystem>
#include <KX11Extras>

// pacman's output reads best in a monospaced font; fontconfig's "monospace" alias
// isn't always one
static QString fixedFontFamily()
{
    const QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    if (QFontInfo(font).fixedPitch())
        return font.family();
    const QStringList families = QFontDatabase::families();
    for (const char *name : {"Noto Sans Mono", "DejaVu Sans Mono", "Hack", "Liberation Mono", "Source Code Pro"}) {
        if (families.contains(QLatin1String(name)))
            return QLatin1String(name);
    }
    return font.family();
}

static const QString NotifyService = QStringLiteral("org.freedesktop.Notifications");
static const QString NotifyPath = QStringLiteral("/org/freedesktop/Notifications");

// The id inside a .flatpakref (file, or Flathub's
// https://dl.flathub.org/repo/appstream/<id>.flatpakref links)
static QString flatpakRefId(const QString &arg)
{
    const QUrl url = QUrl::fromUserInput(arg);
    if (url.isLocalFile()) {
        QFile file(url.toLocalFile());
        if (file.open(QIODevice::ReadOnly)) {
            static const QRegularExpression name(QStringLiteral("^Name\\s*=\\s*(\\S+)\\s*$"),
                                                 QRegularExpression::MultilineOption);
            const auto m = name.match(QString::fromUtf8(file.read(64 * 1024)));
            if (m.hasMatch())
                return m.captured(1);
        }
    }
    QString last = url.fileName();
    if (last.endsWith(QLatin1String(".flatpakref")))
        last.chop(11);
    return last;
}

StoreRequest StoreRequest::fromArguments(const QStringList &arguments)
{
    QCommandLineParser parser;
    QCommandLineOption app(QStringLiteral("app"), QString(), QStringLiteral("id"));
    QCommandLineOption search(QStringLiteral("search"), QString(), QStringLiteral("text"));
    QCommandLineOption games(QStringLiteral("games"));
    parser.addOptions({app, search, games});
    parser.parse(QStringList{QStringLiteral("lingmo-store")} + arguments);

    if (parser.isSet(app))
        return {App, parser.value(app)};
    if (parser.isSet(search))
        return {Search, parser.value(search)};
    if (parser.isSet(games))
        return {Games, QString()};
    // Positional: appstream://<id>, a .flatpakref file or link
    for (const QString &arg : parser.positionalArguments()) {
        if (arg.startsWith(QLatin1String("appstream:"), Qt::CaseInsensitive))
            return {App, arg};
        if (arg.endsWith(QLatin1String(".flatpakref")))
            return {App, flatpakRefId(arg)};
    }
    return {};
}

StoreApp::StoreApp(QObject *parent)
    : QObject(parent)
    , m_fake(!qEnvironmentVariableIsEmpty("LINGMO_STORE_FAKE"))
    , m_flatpak(new FlatpakBackend(m_fake, this))
    , m_catalog(new Catalog(m_flatpak, this))
    , m_installer(new Installer(m_catalog, m_flatpak, this))
    , m_games(new GamesManager(m_catalog, m_installer, this))
    , m_compat(new GameCompat(this))
    , m_reviews(new Reviews(m_catalog, m_fake, this))
    , m_permissions(new Permissions(m_flatpak->userInstallation(), this))
    , m_lutris(new LutrisSource(this))
{
    if (m_fake)
        qInfo("LINGMO_STORE_FAKE is set: nothing will be installed or removed for real");
    connect(m_installer, &Installer::finished, this, &StoreApp::onFinished);

    // Lutris games: the Flathub build of Lutris, installed through the usual Flatpak path
    m_lutris->setLutrisInstalledCheck([this] { return m_catalog->isFlatpakInstalled(QLatin1String(LutrisSource::FlatpakId)); });
    connect(m_catalog, &Catalog::installedChanged, m_lutris, &LutrisSource::installedStateChanged);
    connect(m_lutris, &LutrisSource::lutrisInstallNeeded, this, [this] {
        const QString id = QLatin1String(LutrisSource::FlatpakId);
        const QString key = m_catalog->resolve(id);
        const AppEntry *app = m_catalog->entry(key);
        m_lutrisKey = app && app->flathub ? key : QStringLiteral("flatpak:") + id;
        const bool consent = m_flatpak->state() == FlatpakBackend::NoRemote || m_flatpak->state() == FlatpakBackend::AddingRemote;
        const bool started = app && app->flathub ? m_installer->install(key, QStringLiteral("flathub"))
                                                 : m_installer->installFlatpak(id, QStringLiteral("Lutris"));
        if (!started && !consent) {
            m_lutrisKey.clear();
            m_lutris->lutrisInstallFinished(false, tr("Another installation is running. Try again when it's done."));
        }
    });
    connect(m_installer, &Installer::finished, this,
            [this](const QString &key, const QString &, const QString &action, bool ok, const QString &message) {
                if (m_lutrisKey.isEmpty() || key != m_lutrisKey || action != QLatin1String("install"))
                    return;
                m_lutrisKey.clear();
                m_lutris->lutrisInstallFinished(ok, message);
            });
    connect(m_lutris, &LutrisSource::gameFinished, this,
            [this](const QString &, const QString &name, bool ok, const QString &message) {
                if (m_window && m_window->isActive())
                    return;
                if (ok)
                    notify(tr("%1 installed").arg(name), tr("Play it from the store or from Lutris."));
                else if (!message.isEmpty())
                    notify(tr("Could not install %1").arg(name), message);
            });
    m_flatpak->probe();
    m_catalog->reload();
    m_games->detect();
    m_games->refreshSteam();
}

StoreApp::~StoreApp()
{
    delete m_engine;
}

QString StoreApp::version() const
{
    return QStringLiteral(LINGMO_STORE_VERSION);
}

void StoreApp::show()
{
    if (!m_engine) {
        m_engine = new QQmlApplicationEngine(this);
        m_engine->setNetworkAccessManagerFactory(new CachingNetworkFactory);
        m_engine->addImageProvider(QStringLiteral("appicon"), new AppIconProvider);
        QQmlContext *ctx = m_engine->rootContext();
        ctx->setContextProperty(QStringLiteral("store"), this);
        ctx->setContextProperty(QStringLiteral("catalog"), m_catalog);
        ctx->setContextProperty(QStringLiteral("installer"), m_installer);
        ctx->setContextProperty(QStringLiteral("flatpak"), m_flatpak);
        ctx->setContextProperty(QStringLiteral("games"), m_games);
        ctx->setContextProperty(QStringLiteral("compat"), m_compat);
        ctx->setContextProperty(QStringLiteral("reviews"), m_reviews);
        ctx->setContextProperty(QStringLiteral("permissions"), m_permissions);
        ctx->setContextProperty(QStringLiteral("lutris"), m_lutris);
        ctx->setContextProperty(QStringLiteral("fixedFontFamily"), fixedFontFamily());
        m_engine->load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
        if (m_engine->rootObjects().isEmpty()) {
            qWarning("Failed to load the store window");
            return;
        }
        m_window = qobject_cast<QQuickWindow *>(m_engine->rootObjects().first());
    }
    if (!m_window)
        return;
    m_window->show();
    m_window->raise();
    m_window->requestActivate();
    if (KWindowSystem::isPlatformX11())
        KX11Extras::forceActiveWindow(m_window->winId());
}

void StoreApp::handle(const StoreRequest &request)
{
    show();
    switch (request.kind) {
    case StoreRequest::App:
        Q_EMIT appRequested(request.value);
        break;
    case StoreRequest::Search:
        Q_EMIT searchRequested(request.value);
        break;
    case StoreRequest::Games:
        Q_EMIT gamesRequested();
        break;
    case StoreRequest::None:
        break;
    }
}

void StoreApp::openApp(const QString &app)
{
    handle({StoreRequest::App, app});
}

void StoreApp::search(const QString &text)
{
    handle({StoreRequest::Search, text});
}

void StoreApp::openGames()
{
    handle({StoreRequest::Games, QString()});
}

void StoreApp::activate(const QStringList &arguments)
{
    handle(StoreRequest::fromArguments(arguments));
}

void StoreApp::openUpdater()
{
    if (m_fake) {
        qInfo("[fake] would start lingmo-updater");
        return;
    }
    // The updater is single-instance: this shows its window if it's already running
    if (!QProcess::startDetached(QStringLiteral("lingmo-updater"), {}))
        qWarning("Could not start lingmo-updater");
}

void StoreApp::openUrl(const QString &url)
{
    QDesktopServices::openUrl(QUrl(url));
}

QString StoreApp::formatSize(qint64 bytes) const
{
    return bytes > 0 ? QLocale().formattedDataSize(bytes, 1) : QString();
}

bool StoreApp::hasThemeIcon(const QString &name) const
{
    return !name.isEmpty() && QIcon::hasThemeIcon(name);
}

void StoreApp::copyText(const QString &text)
{
    QGuiApplication::clipboard()->setText(text);
}

bool StoreApp::launch(const QString &key, const QString &source)
{
    const AppEntry *app = m_catalog->entry(key);
    if (!app)
        return false;
    if (m_fake) {
        qInfo().noquote() << "[fake] would launch" << key << "from" << source;
        return true;
    }
    if (source == QLatin1String("flathub") && app->flathub) {
        return QProcess::startDetached(QStringLiteral("flatpak"),
                                       {QStringLiteral("run"), app->flathub->flatpakRef.section(QLatin1Char('/'), 1, 1)});
    }
    if (!app->arch)
        return false;
    QString desktopId = app->arch->desktopId;
    if (desktopId.isEmpty())
        desktopId = app->arch->componentId;
    if (!desktopId.endsWith(QLatin1String(".desktop")))
        desktopId += QLatin1String(".desktop");
    return launchDesktopFile(QStandardPaths::locate(QStandardPaths::ApplicationsLocation, desktopId), app->arch->pkgname);
}

bool StoreApp::launchPackage(const QString &pkgname)
{
    if (m_fake) {
        qInfo().noquote() << "[fake] would launch package" << pkgname;
        return true;
    }
    // The package's own .desktop file
    QProcess pacman;
    pacman.start(QStringLiteral("pacman"), {QStringLiteral("-Qlq"), pkgname});
    pacman.waitForFinished(5000);
    const QStringList files = QString::fromUtf8(pacman.readAllStandardOutput()).split(QLatin1Char('\n'));
    for (const QString &file : files) {
        if (file.startsWith(QLatin1String("/usr/share/applications/")) && file.endsWith(QLatin1String(".desktop")))
            return launchDesktopFile(file, pkgname);
    }
    return launchDesktopFile(QString(), pkgname);
}

// Runs Exec= of a .desktop file (without the field codes), else `fallback`
bool StoreApp::launchDesktopFile(const QString &path, const QString &fallback)
{
    QString exec;
    QFile file(path);
    if (!path.isEmpty() && file.open(QIODevice::ReadOnly)) {
        bool inEntry = false;
        while (!file.atEnd()) {
            const QString line = QString::fromUtf8(file.readLine()).trimmed();
            if (line.startsWith(QLatin1Char('[')))
                inEntry = line == QLatin1String("[Desktop Entry]");
            else if (inEntry && line.startsWith(QLatin1String("Exec="))) {
                exec = line.mid(5);
                break;
            }
        }
    }
    if (exec.isEmpty())
        exec = fallback;
    static const QRegularExpression fieldCodes(QStringLiteral("\\s*%[fFuUdDnNickvm]"));
    exec.remove(fieldCodes);
    QStringList args = QProcess::splitCommand(exec);
    if (args.isEmpty())
        return false;
    const QString program = args.takeFirst();
    return QProcess::startDetached(program, args);
}

void StoreApp::onFinished(const QString &key, const QString &, const QString &action, bool ok, const QString &message)
{
    if (m_window && m_window->isActive())
        return;
    const AppEntry *app = m_catalog->entry(key);
    const QString name = app ? app->name : key.startsWith(QLatin1String("flatpak:")) ? key.section(QLatin1Char('.'), -1) : QString();
    if (action == QLatin1String("gaming")) {
        notify(ok ? tr("Ready for games") : tr("Could not prepare for games"),
               ok ? tr("Steam and the drivers are installed.") : message);
    } else if (action == QLatin1String("install")) {
        notify(ok ? tr("%1 installed").arg(name) : tr("Could not install %1").arg(name),
               ok ? tr("Find it in the launcher.") : message);
    } else {
        notify(ok ? tr("%1 removed").arg(name) : tr("Could not remove %1").arg(name), ok ? QString() : message);
    }
}

void StoreApp::notify(const QString &summary, const QString &body)
{
    QDBusInterface iface(NotifyService, NotifyPath, NotifyService, QDBusConnection::sessionBus());
    const QVariantMap hints{{QStringLiteral("desktop-entry"), QStringLiteral("lingmo-store")}};
    iface.asyncCall(QStringLiteral("Notify"), QStringLiteral("lingmo-store"), 0u,
                    QStringLiteral("system-software-install"), summary, body, QStringList(), hints, -1);
}
