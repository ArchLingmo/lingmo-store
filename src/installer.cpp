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

#include "installer.h"
#include "catalog.h"
#include "errors.h"
#include "flatpakbackend.h"

#include <QDebug>
#include <QFile>
#include <QRegularExpression>

static const QString PacmanLock = QStringLiteral("/var/lib/pacman/db.lck");
static constexpr int MaxLogSize = 256 * 1024;

Installer::Installer(Catalog *catalog, FlatpakBackend *flatpak, QObject *parent)
    : QObject(parent)
    , m_catalog(catalog)
    , m_flatpak(flatpak)
    , m_fakeMode(qEnvironmentVariable("LINGMO_STORE_FAKE"))
{
    if (m_fakeMode == QLatin1String("1"))
        m_fakeMode = QStringLiteral("ok");

    m_logTimer.setSingleShot(true);
    m_logTimer.setInterval(200);
    connect(&m_logTimer, &QTimer::timeout, this, &Installer::logChanged);

    connect(m_flatpak, &FlatpakBackend::progress, this, [this](double fraction, const QString &text) {
        if (m_source == QLatin1String("flathub"))
            setStep(fraction, text);
    });
    connect(m_flatpak, &FlatpakBackend::finished, this, [this](bool ok, const QString &error) {
        if (m_source == QLatin1String("flathub"))
            end(ok, error);
    });
}

void Installer::begin(const QString &key, const QString &source, const QString &action, const QString &name)
{
    m_key = key;
    m_source = source;
    m_action = action;
    m_name = name;
    m_log.clear();
    m_markers.clear();
    m_pending.clear();
    m_total = 0;
    m_downloaded = 0;
    m_inHooks = false;
    m_progress = -1;
    m_status = tr("Waiting for authorization…");
    Q_EMIT busyChanged();
    Q_EMIT progressChanged();
    Q_EMIT logChanged();
}

void Installer::end(bool ok, const QString &message)
{
    const QString key = m_key, source = m_source, action = m_action;
    const bool repoAdded = !m_enablingRepo.isEmpty();
    m_enablingRepo.clear();
    if (ok && fakeMode() && key.startsWith(QLatin1String("pkg:"))) {
        for (const QString &pkg : key.mid(4).split(QLatin1Char(','), Qt::SkipEmptyParts))
            m_catalog->setFakePackageInstalled(pkg, action == QLatin1String("install"));
    }
    else if (ok && fakeMode() && key.startsWith(QLatin1String("flatpak:")))
        m_catalog->setFakeFlatpakInstalled(key.mid(8), action == QLatin1String("install"));
    else if (ok && fakeMode() && action != QLatin1String("gaming"))
        m_catalog->setFakeInstalled(key, source, action == QLatin1String("install"));
    m_action.clear();
    m_progress = ok ? 1.0 : -1;
    m_status = message;
    Q_EMIT busyChanged();
    Q_EMIT progressChanged();
    m_logTimer.stop();
    Q_EMIT logChanged();
    Q_EMIT finished(key, source, action, ok, message);
    if (repoAdded) {
        // pacman.conf may list the repository now, even if the sync or install failed
        m_catalog->reload();
    } else if (ok && !fakeMode()) {
        if (action == QLatin1String("gaming"))
            m_catalog->reload(); // [multilib] may have been enabled
        else
            m_catalog->refreshInstalled();
    }
}

void Installer::setStep(qreal progress, const QString &text)
{
    if (qFuzzyCompare(progress + 2, m_progress + 2) && text == m_status)
        return;
    m_progress = progress;
    m_status = text;
    Q_EMIT progressChanged();
}

bool Installer::install(const QString &key, const QString &source)
{
    if (busy())
        return false;
    const AppEntry *app = m_catalog->entry(key);
    if (!app && source == QLatin1String("flathub") && key.startsWith(QLatin1String("flatpak:")))
        return installFlatpak(key.mid(8), key.mid(8).section(QLatin1Char('.'), -1));
    if (!app)
        return false;
    if (source == QLatin1String("flathub")) {
        if (!app->flathub)
            return false;
        if (m_flatpak->state() == FlatpakBackend::NoRemote || m_flatpak->state() == FlatpakBackend::AddingRemote) {
            Q_EMIT flathubConsentNeeded(key);
            return false;
        }
        begin(key, source, QStringLiteral("install"), app->name);
        m_status = tr("Preparing…");
        Q_EMIT progressChanged();
        m_flatpak->install(app->flathub->flatpakRef.section(QLatin1Char('/'), 1, 1));
        return true;
    }
    if (!app->arch || !app->arch->offered())
        return false;
    if (app->arch->repo.isEmpty())
        return needsRepo(app->arch->needsRepo, key, {app->arch->pkgname}, app->name);
    begin(key, source, QStringLiteral("install"), app->name);
    runHelper({QStringLiteral("install"), app->arch->pkgname});
    return true;
}

bool Installer::installFlatpak(const QString &id, const QString &name)
{
    if (busy() || id.isEmpty())
        return false;
    const QString key = QStringLiteral("flatpak:") + id;
    if (m_flatpak->state() == FlatpakBackend::NoRemote || m_flatpak->state() == FlatpakBackend::AddingRemote) {
        Q_EMIT flathubConsentNeeded(key);
        return false;
    }
    begin(key, QStringLiteral("flathub"), QStringLiteral("install"), name);
    m_status = tr("Preparing…");
    Q_EMIT progressChanged();
    m_flatpak->install(id);
    return true;
}

bool Installer::remove(const QString &key, const QString &source)
{
    if (busy())
        return false;
    const AppEntry *app = m_catalog->entry(key);
    if (!app)
        return false;
    if (source == QLatin1String("flathub")) {
        if (!app->flathub)
            return false;
        const QString id = app->flathub->flatpakRef.section(QLatin1Char('/'), 1, 1);
        begin(key, source, QStringLiteral("remove"), app->name);
        m_flatpak->uninstall(id, m_catalog->flatpakInstallation(id));
        return true;
    }
    if (!app->arch)
        return false;
    begin(key, source, QStringLiteral("remove"), app->name);
    runHelper({QStringLiteral("remove"), app->arch->pkgname});
    return true;
}

bool Installer::installPackage(const QString &pkgname, const QString &name)
{
    if (busy() || pkgname.isEmpty())
        return false;
    begin(QStringLiteral("pkg:") + pkgname, QStringLiteral("arch"), QStringLiteral("install"), name);
    runHelper({QStringLiteral("install"), pkgname});
    return true;
}

bool Installer::installPackages(const QStringList &pkgnames, const QString &name)
{
    if (busy() || pkgnames.isEmpty())
        return false;
    const QString key = QStringLiteral("pkg:") + pkgnames.join(QLatin1Char(','));
    // Packages of a repository that isn't enabled yet: all of the bundle comes from it
    for (const QString &pkg : pkgnames) {
        const AppEntry *app = m_catalog->entry(m_catalog->resolve(pkg));
        if (app && app->arch && app->arch->pkgname == pkg && app->arch->repo.isEmpty() && !app->arch->needsRepo.isEmpty())
            return needsRepo(app->arch->needsRepo, key, pkgnames, name);
    }
    begin(key, QStringLiteral("arch"), QStringLiteral("install"), name);
    runHelper(QStringList{QStringLiteral("install")} + pkgnames);
    return true;
}

bool Installer::needsRepo(const QString &repo, const QString &key, const QStringList &packages, const QString &name)
{
    if (m_catalog->repoEnabled(repo))
        return enableRepoAndInstall(repo, key, packages, name);
    Q_EMIT repoConsentNeeded(repo, key, packages, name);
    return false;
}

bool Installer::enableRepoAndInstall(const QString &repo, const QString &key, const QStringList &packages,
                                     const QString &name)
{
    if (busy() || repo.isEmpty() || packages.isEmpty())
        return false;
    begin(key, QStringLiteral("arch"), QStringLiteral("install"), name);
    m_enablingRepo = repo;
    // The helper only knows its own fixed list of repositories and their sections
    runHelper(QStringList{QStringLiteral("enable-repo"), repo} + packages);
    return true;
}

bool Installer::removePackage(const QString &pkgname, const QString &name)
{
    if (busy() || pkgname.isEmpty())
        return false;
    begin(QStringLiteral("pkg:") + pkgname, QStringLiteral("arch"), QStringLiteral("remove"), name);
    runHelper({QStringLiteral("remove"), pkgname});
    return true;
}

bool Installer::prepareGaming(const QStringList &packages)
{
    if (busy() || packages.isEmpty())
        return false;
    begin(QString(), QStringLiteral("arch"), QStringLiteral("gaming"), tr("Games"));
    runHelper(QStringList{QStringLiteral("prepare-gaming")} + packages);
    return true;
}

void Installer::cancel()
{
    if (m_source == QLatin1String("flathub") && busy())
        m_flatpak->cancel();
}

void Installer::runHelper(const QStringList &args)
{
    if (!fakeMode() && QFile::exists(PacmanLock)) {
        m_log = tr("The package database is locked (%1).").arg(PacmanLock) + QLatin1Char('\n');
        end(false, Errors::busy());
        return;
    }

    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    m_process->setStandardInputFile(QProcess::nullDevice());
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    if (fakeMode()) {
        QFile script(QStringLiteral(":/helper/lingmo-store-helper"));
        if (!script.open(QIODevice::ReadOnly))
            qWarning("The fake helper is missing from the resources");
        env.insert(QStringLiteral("LINGMO_STORE_FAKE"), m_fakeMode);
        m_process->setProgram(QStringLiteral("sh"));
        m_process->setArguments(QStringList{QStringLiteral("-c"), QString::fromUtf8(script.readAll()),
                                            QStringLiteral("lingmo-store-helper")} + args);
        m_log += QStringLiteral("$ [fake] pkexec lingmo-store-helper %1\n").arg(args.join(QLatin1Char(' ')));
    } else {
        m_process->setProgram(QStringLiteral("pkexec"));
        m_process->setArguments(QStringList{QStringLiteral(HELPER_PATH)} + args);
        m_log += QStringLiteral("$ pkexec lingmo-store-helper %1\n").arg(args.join(QLatin1Char(' ')));
    }
    m_process->setProcessEnvironment(env);
    connect(m_process, &QProcess::readyReadStandardOutput, this, &Installer::onHelperOutput);
    connect(m_process, &QProcess::finished, this, &Installer::onHelperFinished);
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        m_process->deleteLater();
        m_process = nullptr;
        end(false, tr("Could not start pkexec. Make sure polkit is installed."));
    });
    m_process->start();
}

void Installer::onHelperOutput()
{
    QString chunk = QString::fromUtf8(m_process->readAllStandardOutput());
    chunk.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    m_pending += chunk;
    const int end = m_pending.lastIndexOf(QLatin1Char('\n'));
    if (end < 0)
        return;
    const QString complete = m_pending.left(end + 1);
    m_pending = m_pending.mid(end + 1);
    const QStringList lines = complete.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size() - 1; ++i)
        takeLine(lines.at(i));
}

// The helper's "@@" markers drive the progress but aren't shown in the details
void Installer::takeLine(const QString &line)
{
    if (line.startsWith(QLatin1String("@@")))
        m_markers += line + QLatin1Char('\n');
    else
        m_log += line + QLatin1Char('\n');
    if (m_log.size() > MaxLogSize)
        m_log = m_log.right(MaxLogSize * 3 / 4);
    if (!m_logTimer.isActive())
        m_logTimer.start();
    const QString trimmed = line.trimmed();
    if (!trimmed.isEmpty())
        parseLine(trimmed);
}

// pacman's non-interactive output -> overall progress:
// sync 0-10%, download 12-55%, checks 55-65%, install/remove 65-95%, hooks 95-100%
void Installer::parseLine(const QString &line)
{
    static const QRegularExpression counted(QStringLiteral("^\\((\\d+)/(\\d+)\\)\\s+(.*)$"));
    static const QRegularExpression packagesLine(QStringLiteral("^Packages \\((\\d+)\\)"));
    const bool removing = m_action == QLatin1String("remove");

    if (line == QLatin1String("@@STEP multilib")) {
        setStep(0.02, tr("Enabling the 32-bit games repository (multilib)…"));
    } else if (line == QLatin1String("@@STEP repo")) {
        setStep(0.02, tr("Enabling the %1 repository…").arg(m_enablingRepo));
    } else if (line == QLatin1String("@@STEP sync") || line.startsWith(QLatin1String(":: Synchronizing package databases"))) {
        setStep(0.05, tr("Updating the list of available software…"));
    } else if (line.startsWith(QLatin1String(":: Starting full system upgrade"))) {
        setStep(0.10, tr("Checking what needs to be installed…"));
    } else if (const auto m = packagesLine.match(line); m.hasMatch()) {
        m_total = m.captured(1).toInt();
    } else if (line.startsWith(QLatin1String(":: Retrieving packages"))) {
        setStep(0.12, tr("Downloading…"));
    } else if (line.endsWith(QLatin1String("downloading...")) && m_total > 0 && m_progress >= 0.1) {
        m_downloaded = qMin(m_downloaded + 1, m_total);
        setStep(0.12 + 0.43 * m_downloaded / m_total, tr("Downloading (%1 of %2)…").arg(m_downloaded).arg(m_total));
    } else if (line.startsWith(QLatin1String(":: Processing package changes"))) {
        setStep(0.65, removing ? tr("Removing…") : tr("Installing…"));
    } else if (line.startsWith(QLatin1String(":: Running post-transaction hooks"))) {
        m_inHooks = true;
        setStep(0.95, tr("Finishing…"));
    } else if (const auto m = counted.match(line); m.hasMatch()) {
        const int i = m.captured(1).toInt();
        const int n = qMax(1, m.captured(2).toInt());
        const QString what = m.captured(3);
        if (m_inHooks) {
            setStep(0.95 + 0.05 * i / n, tr("Finishing…"));
        } else if (what.startsWith(QLatin1String("checking")) || what.startsWith(QLatin1String("loading"))) {
            setStep(qMax(m_progress, 0.55 + 0.10 * i / n * 0.9), tr("Verifying…"));
        } else if (what.startsWith(QLatin1String("installing")) || what.startsWith(QLatin1String("upgrading"))
                   || what.startsWith(QLatin1String("reinstalling")) || what.startsWith(QLatin1String("removing"))) {
            setStep(0.65 + 0.30 * i / n, removing ? tr("Removing (%1 of %2)…").arg(i).arg(n)
                                                  : tr("Installing (%1 of %2)…").arg(i).arg(n));
        }
    }
}

QString Installer::helperError(int exitCode) const
{
    if (exitCode == 126 || exitCode == 127)
        return Errors::authCancelled();
    if (m_markers.contains(QLatin1String("@@ERROR lock")) || m_log.contains(QLatin1String("unable to lock database")))
        return Errors::busy();
    if (m_markers.contains(QLatin1String("@@ERROR protected")))
        return tr("This is an essential part of the system and can't be removed.");
    if (m_markers.contains(QLatin1String("@@ERROR config")) && !m_enablingRepo.isEmpty())
        return tr("Could not enable the %1 repository in /etc/pacman.conf. Nothing was changed.").arg(m_enablingRepo);
    if (m_markers.contains(QLatin1String("@@ERROR config")))
        return tr("Could not enable the multilib repository in /etc/pacman.conf. Nothing was changed.");
    if (m_log.contains(QLatin1String("breaks dependency")) || m_log.contains(QLatin1String("required by")))
        return tr("Other programs need this one, so it can't be removed on its own.");
    if (m_log.contains(QLatin1String("error: target not found")))
        return tr("This package is no longer in the repositories. Update the system and try again.");
    if (m_log.contains(QLatin1String("The requested URL returned error: 404")))
        return tr("The servers no longer have this version. Update the system with Updates and try again.");
    if (m_log.contains(QLatin1String("failed retrieving file")) || m_log.contains(QLatin1String("failed to synchronize"))
        || m_log.contains(QLatin1String("Could not resolve host")) || m_log.contains(QLatin1String("Connection timed out"))
        || m_log.contains(QLatin1String("failed to retrieve some files")))
        return Errors::offline();
    if (m_log.contains(QLatin1String("not enough free disk space")))
        return Errors::noSpace();
    if (m_log.contains(QLatin1String("conflicting")) || m_log.contains(QLatin1String("are in conflict")))
        return tr("It conflicts with software already installed. Open the details to see which.");
    return (m_action == QLatin1String("remove") ? Errors::removeFailed() : Errors::installFailed())
        + QLatin1Char(' ') + tr("Open the details to see what went wrong.");
}

void Installer::onHelperFinished(int exitCode, QProcess::ExitStatus status)
{
    onHelperOutput();
    if (!m_pending.isEmpty()) {
        takeLine(m_pending);
        m_pending.clear();
    }
    m_process->deleteLater();
    m_process = nullptr;
    if (status == QProcess::NormalExit && exitCode == 0) {
        end(true, QString());
        return;
    }
    qWarning() << "lingmo-store-helper failed with" << exitCode;
    end(false, status == QProcess::NormalExit ? helperError(exitCode) : tr("The operation was interrupted."));
}
