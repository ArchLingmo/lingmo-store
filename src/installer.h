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
#include <QProcess>
#include <QStringList>
#include <QTimer>

class Catalog;
class FlatpakBackend;

// Runs one software change at a time: Flathub apps through FlatpakBackend, Arch
// packages through the root helper (pkexec lingmo-store-helper), whose pacman output
// becomes a progress bar. With LINGMO_STORE_FAKE set nothing is changed: the helper
// runs from the resources in its simulation mode and flatpak is simulated too.
class Installer : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString currentKey READ currentKey NOTIFY busyChanged)
    Q_PROPERTY(QString currentSource READ currentSource NOTIFY busyChanged)
    Q_PROPERTY(QString currentAction READ currentAction NOTIFY busyChanged)
    Q_PROPERTY(QString currentName READ currentName NOTIFY busyChanged)
    Q_PROPERTY(bool canCancel READ canCancel NOTIFY busyChanged)
    Q_PROPERTY(qreal progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY progressChanged)
    Q_PROPERTY(QString log READ log NOTIFY logChanged)
    Q_PROPERTY(bool fakeMode READ fakeMode CONSTANT)

public:
    Installer(Catalog *catalog, FlatpakBackend *flatpak, QObject *parent = nullptr);

    bool busy() const { return !m_action.isEmpty(); }
    QString currentKey() const { return m_key; }
    QString currentSource() const { return m_source; }
    QString currentAction() const { return m_action; }
    QString currentName() const { return m_name; }
    bool canCancel() const { return m_source == QLatin1String("flathub"); }
    qreal progress() const { return m_progress; }
    QString statusText() const { return m_status; }
    QString log() const { return m_log; }
    bool fakeMode() const { return !m_fakeMode.isEmpty(); }

    // source: "arch" or "flathub"
    Q_INVOKABLE bool install(const QString &key, const QString &source);
    Q_INVOKABLE bool remove(const QString &key, const QString &source);
    // A Flathub app by id, for when the catalog doesn't list it (key "flatpak:<id>")
    Q_INVOKABLE bool installFlatpak(const QString &id, const QString &name);
    // A package that isn't an AppStream app (native games from [archlingmo])
    Q_INVOKABLE bool installPackage(const QString &pkgname, const QString &name);
    Q_INVOKABLE bool removePackage(const QString &pkgname, const QString &name);
    // Several packages in one transaction (a collection's bundle, e.g. the Craft office suite)
    Q_INVOKABLE bool installPackages(const QStringList &pkgnames, const QString &name);
    // Adds a known ArchLingmo repository (e.g. archlingmo-craft) to pacman.conf, syncs
    // it and installs the packages from it: one helper call, one authorization.
    // key: the app's key, or "pkg:<pkg>,…" for a bundle. Ask the user first
    // (repoConsentNeeded): install() and installPackages() do.
    Q_INVOKABLE bool enableRepoAndInstall(const QString &repo, const QString &key, const QStringList &packages,
                                          const QString &name);
    // Enables [multilib] when needed and installs the gaming packages in one go
    Q_INVOKABLE bool prepareGaming(const QStringList &packages);
    Q_INVOKABLE void cancel();

Q_SIGNALS:
    void busyChanged();
    void progressChanged();
    void logChanged();
    // action: "install", "remove" or "gaming"
    void finished(const QString &key, const QString &source, const QString &action, bool ok, const QString &message);
    // A Flathub app was asked for but the flathub remote isn't set up yet
    void flathubConsentNeeded(const QString &key);
    // The packages come from a repository that isn't in pacman.conf yet: ask, then
    // call enableRepoAndInstall() with the same arguments
    void repoConsentNeeded(const QString &repo, const QString &key, const QStringList &packages, const QString &name);

private:
    void begin(const QString &key, const QString &source, const QString &action, const QString &name);
    void end(bool ok, const QString &message);
    void setStep(qreal progress, const QString &text);
    void runHelper(const QStringList &args);
    void onHelperOutput();
    void onHelperFinished(int exitCode, QProcess::ExitStatus status);
    void takeLine(const QString &line);
    void parseLine(const QString &line);
    QString helperError(int exitCode) const;
    // Runs enable-repo directly when pacman.conf already lists the repository (its
    // database just isn't synced), else asks for consent. False when not needed.
    bool needsRepo(const QString &repo, const QString &key, const QStringList &packages, const QString &name);

    Catalog *m_catalog;
    FlatpakBackend *m_flatpak;
    QString m_fakeMode;
    QString m_key, m_source, m_action, m_name;
    qreal m_progress = -1;
    QString m_status;
    QString m_log;
    QString m_pending;
    QString m_markers;
    QProcess *m_process = nullptr;
    int m_total = 0;
    int m_downloaded = 0;
    bool m_inHooks = false;
    QString m_enablingRepo; // enable-repo running: reload the catalog afterwards
    QTimer m_logTimer;
};
