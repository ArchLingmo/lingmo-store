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

#include "lutrissource.h"
#include "errors.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUrlQuery>

#include <algorithm>

namespace {

const QString DefaultApi = QStringLiteral("https://lutris.net");
constexpr qint64 SearchMaxAge = 24 * 3600;
constexpr qint64 GameMaxAge = 24 * 3600;
constexpr qint64 CoverMaxAge = 30 * 24 * 3600;
constexpr qint64 MinRequestGapMs = 1000;   // never more than one API request a second
constexpr qint64 DefaultBackoff = 60;      // seconds, after "429 Too Many Requests"
constexpr qint64 MaxCoverBytes = 3 * 1024 * 1024;
constexpr int MaxCoversInFlight = 2;

bool isOffline(QNetworkReply::NetworkError error)
{
    switch (error) {
    case QNetworkReply::HostNotFoundError:
    case QNetworkReply::TimeoutError:
    case QNetworkReply::ConnectionRefusedError:
    case QNetworkReply::TemporaryNetworkFailureError:
    case QNetworkReply::NetworkSessionFailedError:
    case QNetworkReply::UnknownNetworkError:
    case QNetworkReply::OperationCanceledError:
        return true;
    default:
        return false;
    }
}

QStringList names(const QJsonValue &value)
{
    QStringList out;
    for (const QJsonValue &v : value.toArray()) {
        const QString name = v.isObject() ? v.toObject().value(QStringLiteral("name")).toString() : v.toString();
        if (!name.isEmpty())
            out << name;
    }
    return out;
}

// Pictures only from lutris.net itself
QString lutrisImage(const QJsonValue &value)
{
    const QUrl url(value.toString());
    if (url.scheme() != QLatin1String("https") || url.host() != QLatin1String("lutris.net"))
        return {};
    return url.toString();
}

QString hashed(const QString &text)
{
    return QString::fromLatin1(QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha1).toHex());
}

} // namespace

LutrisSource::LutrisSource(QObject *parent)
    : QObject(parent)
    , m_network(new QNetworkAccessManager(this))
    , m_cacheDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/lutris"))
    , m_appDir(QDir::homePath() + QStringLiteral("/.var/app/") + QLatin1String(FlatpakId))
    , m_fake(!qEnvironmentVariableIsEmpty("LINGMO_STORE_FAKE"))
{
    m_network->setTransferTimeout(20000);
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(600);
    connect(&m_debounce, &QTimer::timeout, this, &LutrisSource::startSearch);
    m_poll.setInterval(3000);
    connect(&m_poll, &QTimer::timeout, this, &LutrisSource::poll);

    QSettings settings;
    m_strict = settings.value(QStringLiteral("lutris/strictIsolation"), true).toBool();

    m_runner = [this](const QString &program, const QStringList &args, std::function<void(int)> done) {
        if (m_fake) {
            qInfo().noquote() << "[fake] would run" << program << args.join(QLatin1Char(' '));
            if (done)
                QTimer::singleShot(0, this, [done] { done(0); });
            return;
        }
        // Lutris keeps running after the store closes: never a child of the store
        if (args.value(0) == QLatin1String("run")) {
            const bool ok = QProcess::startDetached(program, args);
            if (done)
                QTimer::singleShot(0, this, [done, ok] { done(ok ? 0 : -1); });
            return;
        }
        auto *proc = new QProcess(this);
        proc->setProcessChannelMode(QProcess::ForwardedErrorChannel);
        connect(proc, &QProcess::finished, this, [proc, done](int code, QProcess::ExitStatus status) {
            proc->deleteLater();
            if (done)
                done(status == QProcess::NormalExit ? code : -1);
        });
        connect(proc, &QProcess::errorOccurred, this, [proc, done](QProcess::ProcessError error) {
            if (error != QProcess::FailedToStart)
                return;
            proc->deleteLater();
            if (done)
                done(-1);
        });
        proc->start(program, args);
    };
    m_lutrisRunningCheck = [this] { return defaultLutrisRunning(); };
    refreshLibrary();
}

bool LutrisSource::lutrisInstalled() const
{
    return m_lutrisInstalledCheck ? m_lutrisInstalledCheck() : false;
}

QString LutrisSource::gamesFolder() const
{
    return QDir::homePath() + QStringLiteral("/Games/Lutris");
}

void LutrisSource::bump()
{
    ++m_revision;
    Q_EMIT changed();
}

// ---- Parsing ------------------------------------------------------------------

QString LutrisSource::foldName(const QString &name)
{
    QString out;
    for (const QChar c : name.normalized(QString::NormalizationForm_D)) {
        if (c.isLetterOrNumber())
            out.append(c.toLower());
        else if (c.isSpace() && !out.endsWith(QLatin1Char(' ')))
            out.append(QLatin1Char(' '));
    }
    return out.trimmed();
}

bool LutrisSource::validSlug(const QString &slug)
{
    static const QRegularExpression re(QStringLiteral("^[a-z0-9][a-z0-9-]{0,199}$"));
    return re.match(slug).hasMatch();
}

QList<LutrisSource::Game> LutrisSource::parseSearch(const QByteArray &json)
{
    QList<Game> games;
    const QJsonArray results = QJsonDocument::fromJson(json).object().value(QStringLiteral("results")).toArray();
    for (const QJsonValue &v : results) {
        const QJsonObject o = v.toObject();
        Game g;
        g.slug = o.value(QStringLiteral("slug")).toString();
        g.name = o.value(QStringLiteral("name")).toString().trimmed();
        if (!validSlug(g.slug) || g.name.isEmpty())
            continue;
        g.id = o.value(QStringLiteral("id")).toInt();
        g.year = o.value(QStringLiteral("year")).toInt();
        g.cover = lutrisImage(o.value(QStringLiteral("coverart")));
        g.banner = lutrisImage(o.value(QStringLiteral("banner_url")));
        g.platforms = names(o.value(QStringLiteral("platforms")));
        g.aliases = names(o.value(QStringLiteral("aliases")));
        games << g;
    }
    return games;
}

QVariantList LutrisSource::wineInstallers(const QJsonArray &installers)
{
    QVariantList out;
    for (const QJsonValue &v : installers) {
        const QJsonObject o = v.toObject();
        const QString slug = o.value(QStringLiteral("slug")).toString();
        if (!validSlug(slug) || o.value(QStringLiteral("runner")).toString() != QLatin1String("wine"))
            continue;
        if (o.value(QStringLiteral("draft")).toBool() || !o.value(QStringLiteral("published")).toBool(true))
            continue;
        QString name = o.value(QStringLiteral("version")).toString().trimmed();
        if (name.isEmpty())
            name = o.value(QStringLiteral("name")).toString();
        out << QVariantMap{
            {QStringLiteral("slug"), slug},
            {QStringLiteral("name"), name},
            {QStringLiteral("notes"), o.value(QStringLiteral("notes")).toString().trimmed()},
            {QStringLiteral("description"), o.value(QStringLiteral("description")).toString().trimmed()},
            {QStringLiteral("user"), o.value(QStringLiteral("user")).toString()},
            {QStringLiteral("updated"), o.value(QStringLiteral("updated_at")).toString().left(10)},
        };
    }
    return out;
}

int LutrisSource::defaultInstaller(const QVariantList &installers, const QLocale &locale)
{
    if (installers.isEmpty())
        return -1;
    // "Standard (BR)": the user's own region first
    const QString country = QLocale::territoryToCode(locale.territory());
    if (country.size() == 2) {
        const QRegularExpression region(QStringLiteral("\\b%1\\b").arg(country), QRegularExpression::CaseInsensitiveOption);
        for (int i = 0; i < installers.size(); ++i) {
            if (region.match(installers.at(i).toMap().value(QStringLiteral("name")).toString()).hasMatch())
                return i;
        }
    }
    static const QRegularExpression plain(QStringLiteral("^(standard|installer|default|latest|online|launcher)\\b"),
                                          QRegularExpression::CaseInsensitiveOption);
    for (int i = 0; i < installers.size(); ++i) {
        if (plain.match(installers.at(i).toMap().value(QStringLiteral("name")).toString()).hasMatch())
            return i;
    }
    return 0;
}

QString LutrisSource::onlineGenre(const QStringList &genres)
{
    static const QRegularExpression online(QStringLiteral("^(mmo|massively|moba|battle royale)"),
                                           QRegularExpression::CaseInsensitiveOption);
    for (const QString &genre : genres) {
        if (online.match(genre).hasMatch())
            return genre;
    }
    return {};
}

QVariantMap LutrisSource::parseGame(const QByteArray &json, const QLocale &locale)
{
    const QJsonObject o = QJsonDocument::fromJson(json).object();
    const QString slug = o.value(QStringLiteral("slug")).toString();
    if (!validSlug(slug))
        return {};
    const QStringList genres = names(o.value(QStringLiteral("genres")));
    const QVariantList installers = wineInstallers(o.value(QStringLiteral("installers")).toArray());
    const int chosen = defaultInstaller(installers, locale);
    QString description = o.value(QStringLiteral("description")).toString();
    description.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    description = description.trimmed();
    const QString online = onlineGenre(genres);
    return {
        {QStringLiteral("slug"), slug},
        {QStringLiteral("name"), o.value(QStringLiteral("name")).toString()},
        {QStringLiteral("year"), o.value(QStringLiteral("year")).toInt()},
        {QStringLiteral("cover"), lutrisImage(o.value(QStringLiteral("coverart")))},
        {QStringLiteral("banner"), lutrisImage(o.value(QStringLiteral("banner_url")))},
        {QStringLiteral("platforms"), names(o.value(QStringLiteral("platforms")))},
        {QStringLiteral("genres"), genres},
        {QStringLiteral("description"), description},
        {QStringLiteral("userCount"), o.value(QStringLiteral("user_count")).toInt()},
        {QStringLiteral("installers"), installers},
        {QStringLiteral("defaultInstaller"),
         chosen >= 0 ? installers.at(chosen).toMap().value(QStringLiteral("slug")).toString() : QString()},
        {QStringLiteral("online"), !online.isEmpty()},
        {QStringLiteral("onlineGenre"), online},
    };
}

QVariantList LutrisSource::rank(const QList<Game> &games, const QString &query, int limit)
{
    const QString q = foldName(query);
    const QString qCompact = QString(q).remove(QLatin1Char(' '));
    const QStringList tokens = q.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (tokens.isEmpty())
        return {};
    QList<std::pair<int, const Game *>> scored;
    for (const Game &g : games) {
        if (!g.platforms.contains(QLatin1String("Windows")))
            continue;
        const QString name = foldName(g.name);
        const QString compact = QString(name).remove(QLatin1Char(' '));
        int score = 0;
        if (name == q || compact == qCompact)
            score = 100;
        else if (name.startsWith(q) || compact.startsWith(qCompact))
            score = 60;
        else {
            const QStringList words = name.split(QLatin1Char(' '), Qt::SkipEmptyParts);
            const bool allWords = std::all_of(tokens.cbegin(), tokens.cend(), [&](const QString &t) {
                return std::any_of(words.cbegin(), words.cend(), [&](const QString &w) { return w.startsWith(t); });
            });
            if (allWords)
                score = 40;
            else if (compact.contains(qCompact))
                score = 20;
            else
                score = 5; // matched an alias on the server
        }
        if (!g.cover.isEmpty())
            score += 4;
        if (g.year > 0)
            score += 2;
        scored.append({score, &g});
    }
    std::stable_sort(scored.begin(), scored.end(), [](const auto &a, const auto &b) {
        if (a.first != b.first)
            return a.first > b.first;
        return a.second->name.localeAwareCompare(b.second->name) < 0;
    });
    QVariantList out;
    for (int i = 0; i < scored.size() && out.size() < limit; ++i) {
        const Game &g = *scored.at(i).second;
        out << QVariantMap{
            {QStringLiteral("slug"), g.slug},
            {QStringLiteral("name"), g.name},
            {QStringLiteral("year"), g.year},
            {QStringLiteral("platforms"), g.platforms},
            {QStringLiteral("remoteCover"), g.cover},
            {QStringLiteral("runner"), QStringLiteral("wine")},
        };
    }
    return out;
}

QVariantList LutrisSource::withoutCatalogDuplicates(const QVariantList &lutrisGames, const QVariantList &catalogApps)
{
    QSet<QString> known;
    for (const QVariant &v : catalogApps) {
        const QString name = foldName(v.toMap().value(QStringLiteral("name")).toString()).remove(QLatin1Char(' '));
        if (!name.isEmpty())
            known.insert(name);
    }
    QVariantList out;
    for (const QVariant &v : lutrisGames) {
        if (!known.contains(foldName(v.toMap().value(QStringLiteral("name")).toString()).remove(QLatin1Char(' '))))
            out << v;
    }
    return out;
}

QStringList LutrisSource::installArguments(const QString &installerSlug)
{
    return {QStringLiteral("run"), QLatin1String(FlatpakId), QStringLiteral("lutris:install/") + installerSlug};
}

QStringList LutrisSource::playArguments(const QString &gameSlug)
{
    return {QStringLiteral("run"), QLatin1String(FlatpakId), QStringLiteral("lutris:rungame/") + gameSlug};
}

QStringList LutrisSource::isolationArguments(bool strict)
{
    if (!strict)
        return {QStringLiteral("override"), QStringLiteral("--user"), QStringLiteral("--reset"), QLatin1String(FlatpakId)};
    // Lutris on Flathub may read and change the whole home folder and start programs
    // outside its sandbox (org.freedesktop.Flatpak). Wine games need neither: they
    // get ~/Games/Lutris, and Downloads read-only for installers the user saved.
    return {
        QStringLiteral("override"),
        QStringLiteral("--user"),
        QStringLiteral("--nofilesystem=home"),
        QStringLiteral("--nofilesystem=~/Games"),
        QStringLiteral("--nofilesystem=/media"),
        QStringLiteral("--nofilesystem=/run/media"),
        QStringLiteral("--nofilesystem=~/.var/app/com.valvesoftware.Steam"),
        QStringLiteral("--filesystem=~/Games/Lutris:create"),
        QStringLiteral("--filesystem=xdg-download:ro"),
        QStringLiteral("--no-talk-name=org.freedesktop.Flatpak"),
        QLatin1String(FlatpakId),
    };
}

QSet<QString> LutrisSource::readLibrary(const QString &databasePath)
{
    QSet<QString> installed;
    if (!QFileInfo::exists(databasePath))
        return installed;
    const QString connection = QStringLiteral("lutris-library-%1").arg(quintptr(&installed));
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setDatabaseName(databasePath);
        // Lutris owns this file: never write to it
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=500"));
        if (db.open()) {
            QSqlQuery query(db);
            if (query.exec(QStringLiteral("SELECT slug, installer_slug FROM games WHERE installed = 1"))) {
                while (query.next()) {
                    for (int i = 0; i < 2; ++i) {
                        const QString value = query.value(i).toString();
                        if (!value.isEmpty())
                            installed.insert(value);
                    }
                }
            }
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connection);
    return installed;
}

// ---- Cache and network -----------------------------------------------------------

QString LutrisSource::apiBase() const
{
    QString base = qEnvironmentVariable("LINGMO_STORE_LUTRIS_API", DefaultApi);
    while (base.endsWith(QLatin1Char('/')))
        base.chop(1);
    return base;
}

QString LutrisSource::cachePath(const QString &kind, const QString &name) const
{
    const QString dir = m_cacheDir + QLatin1Char('/') + kind;
    QDir().mkpath(dir);
    return dir + QLatin1Char('/') + name;
}

bool LutrisSource::readCache(const QString &path, qint64 maxAge, QByteArray *data, qint64 *modified) const
{
    const QFileInfo info(path);
    if (!info.exists())
        return false;
    const qint64 age = info.lastModified().secsTo(QDateTime::currentDateTime());
    if (maxAge >= 0 && age > maxAge)
        return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    *data = file.readAll();
    if (modified)
        *modified = info.lastModified().toSecsSinceEpoch();
    return true;
}

void LutrisSource::writeCache(const QString &path, const QByteArray &data) const
{
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(data);
        file.commit();
    }
}

QNetworkReply *LutrisSource::get(const QUrl &url)
{
    m_lastRequest = QDateTime::currentMSecsSinceEpoch();
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("lingmo-store/" LINGMO_STORE_VERSION));
    request.setRawHeader("Accept", "application/json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    return m_network->get(request);
}

bool LutrisSource::rateLimited() const
{
    return QDateTime::currentSecsSinceEpoch() < m_backoffUntil;
}

void LutrisSource::noteRateLimit(QNetworkReply *reply)
{
    if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 429)
        return;
    qint64 wait = reply->rawHeader("Retry-After").toLongLong();
    if (wait <= 0 || wait > 3600)
        wait = DefaultBackoff;
    m_backoffUntil = QDateTime::currentSecsSinceEpoch() + wait;
    qInfo() << "lutris.net asked to slow down; waiting" << wait << "s";
}

// ---- Search ------------------------------------------------------------------------

void LutrisSource::search(const QString &text)
{
    const QString key = foldName(text);
    if (key.size() < 3)
        return;
    // Fresh in memory or on disk: no request
    auto it = m_searches.constFind(key);
    if (it != m_searches.constEnd() && it->state == QLatin1String("ready")
        && QDateTime::currentSecsSinceEpoch() - it->fetched < SearchMaxAge)
        return;
    QByteArray cached;
    qint64 modified = 0;
    if (readCache(cachePath(QStringLiteral("search"), hashed(key) + QStringLiteral(".json")), SearchMaxAge, &cached, &modified)) {
        SearchEntry &entry = m_searches[key];
        entry.state = QStringLiteral("ready");
        entry.games = parseSearch(cached);
        entry.fetched = modified;
        queueCovers(entry.games);
        bump();
        return;
    }
    m_pendingQuery = text.simplified();
    if (!m_searches.contains(key) || m_searches.value(key).state != QLatin1String("ready")) {
        m_searches[key].state = QStringLiteral("loading");
        bump();
    }
    // Wait for the user to stop typing, and keep a gap between requests
    const qint64 gap = MinRequestGapMs - (QDateTime::currentMSecsSinceEpoch() - m_lastRequest);
    m_debounce.start(qMax<qint64>(m_debounce.interval(), gap));
}

void LutrisSource::startSearch()
{
    const QString text = m_pendingQuery;
    const QString key = foldName(text);
    if (key.size() < 3)
        return;
    QByteArray stale;
    qint64 modified = 0;
    const QString path = cachePath(QStringLiteral("search"), hashed(key) + QStringLiteral(".json"));
    auto fallBack = [&](const QString &state) {
        SearchEntry &entry = m_searches[key];
        if (readCache(path, -1, &stale, &modified)) {
            entry.games = parseSearch(stale);
            entry.fetched = modified;
            entry.state = QStringLiteral("ready");
        } else {
            entry.state = state;
        }
        bump();
    };
    if (!m_networkEnabled) {
        fallBack(QStringLiteral("offline"));
        return;
    }
    if (rateLimited()) {
        fallBack(QStringLiteral("busy"));
        return;
    }
    QUrl url(apiBase() + QStringLiteral("/api/games"));
    QUrlQuery params;
    params.addQueryItem(QStringLiteral("search"), text);
    params.addQueryItem(QStringLiteral("with-installers"), QStringLiteral("1"));
    url.setQuery(params);
    QNetworkReply *reply = get(url);
    connect(reply, &QNetworkReply::finished, this, [this, reply, key] { onSearchReply(reply, key); });
}

void LutrisSource::onSearchReply(QNetworkReply *reply, const QString &key)
{
    reply->deleteLater();
    noteRateLimit(reply);
    const QByteArray body = reply->readAll();
    SearchEntry &entry = m_searches[key];
    const QString path = cachePath(QStringLiteral("search"), hashed(key) + QStringLiteral(".json"));
    if (reply->error() == QNetworkReply::NoError && QJsonDocument::fromJson(body).isObject()) {
        writeCache(path, body);
        entry.games = parseSearch(body);
        entry.fetched = QDateTime::currentSecsSinceEpoch();
        entry.state = QStringLiteral("ready");
        queueCovers(entry.games);
    } else {
        QByteArray stale;
        qint64 modified = 0;
        if (readCache(path, -1, &stale, &modified)) {
            entry.games = parseSearch(stale);
            entry.fetched = modified;
            entry.state = QStringLiteral("ready");
        } else {
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            entry.state = status == 429 ? QStringLiteral("busy")
                        : isOffline(reply->error()) ? QStringLiteral("offline") : QStringLiteral("error");
        }
        qInfo().noquote() << "Lutris search failed:" << reply->errorString();
    }
    bump();
}

QVariantList LutrisSource::results(const QString &text, const QVariantList &catalogApps) const
{
    const auto it = m_searches.constFind(foldName(text));
    if (it == m_searches.constEnd())
        return {};
    QVariantList list = withoutCatalogDuplicates(rank(it->games, text), catalogApps);
    for (QVariant &v : list) {
        QVariantMap map = v.toMap();
        const QString slug = map.value(QStringLiteral("slug")).toString();
        map.insert(QStringLiteral("cover"), coverFor(slug, map.value(QStringLiteral("remoteCover")).toString()));
        map.insert(QStringLiteral("status"), status(slug));
        v = map;
    }
    return list;
}

QString LutrisSource::searchState(const QString &text) const
{
    const QString key = foldName(text);
    if (key.size() < 3)
        return QStringLiteral("none");
    return m_searches.value(key).state.isEmpty() ? QStringLiteral("none") : m_searches.value(key).state;
}

// ---- Covers ------------------------------------------------------------------------

QString LutrisSource::coverFor(const QString &slug, const QString &remote) const
{
    const QString path = m_cacheDir + QStringLiteral("/covers/") + slug + QStringLiteral(".img");
    if (QFileInfo::exists(path))
        return QUrl::fromLocalFile(path).toString();
    Q_UNUSED(remote)
    return {}; // still downloading (or none): the card shows a placeholder
}

void LutrisSource::queueCovers(const QList<Game> &games)
{
    int queued = 0;
    for (const Game &g : games) {
        if (g.cover.isEmpty() || !g.platforms.contains(QLatin1String("Windows")) || m_coverFailed.contains(g.slug))
            continue;
        const QString path = cachePath(QStringLiteral("covers"), g.slug + QStringLiteral(".img"));
        const QFileInfo info(path);
        if (info.exists() && info.lastModified().secsTo(QDateTime::currentDateTime()) < CoverMaxAge)
            continue;
        if (m_coverUrls.contains(g.slug))
            continue;
        m_coverUrls.insert(g.slug, g.cover);
        m_coverQueue << g.slug;
        // Only what a results page shows
        if (++queued >= 16)
            break;
    }
    fetchNextCover();
}

void LutrisSource::fetchNextCover()
{
    if (!m_networkEnabled || rateLimited())
        return;
    while (m_coversInFlight < MaxCoversInFlight && !m_coverQueue.isEmpty()) {
        const QString slug = m_coverQueue.takeFirst();
        const QString url = m_coverUrls.value(slug);
        ++m_coversInFlight;
        QNetworkReply *reply = get(QUrl(url));
        connect(reply, &QNetworkReply::downloadProgress, reply, [reply](qint64 received, qint64) {
            if (received > MaxCoverBytes)
                reply->abort();
        });
        connect(reply, &QNetworkReply::finished, this, [this, reply, slug] {
            reply->deleteLater();
            --m_coversInFlight;
            m_coverUrls.remove(slug);
            noteRateLimit(reply);
            const QByteArray data = reply->readAll();
            const QString type = reply->header(QNetworkRequest::ContentTypeHeader).toString();
            if (reply->error() == QNetworkReply::NoError && type.startsWith(QLatin1String("image/")) && !data.isEmpty()) {
                writeCache(cachePath(QStringLiteral("covers"), slug + QStringLiteral(".img")), data);
                bump();
            } else {
                m_coverFailed.insert(slug);
            }
            fetchNextCover();
        });
    }
}

// ---- A game's page -----------------------------------------------------------------

void LutrisSource::fetchGame(const QString &slug)
{
    if (!validSlug(slug))
        return;
    refreshLibrary();
    GameEntry &entry = m_games[slug];
    if (entry.state == QLatin1String("loading"))
        return;
    if (entry.state == QLatin1String("ready") && QDateTime::currentSecsSinceEpoch() - entry.fetched < GameMaxAge)
        return;
    const QString path = cachePath(QStringLiteral("games"), slug + QStringLiteral(".json"));
    QByteArray cached;
    qint64 modified = 0;
    if (readCache(path, GameMaxAge, &cached, &modified)) {
        entry.data = parseGame(cached);
        entry.fetched = modified;
        entry.state = entry.data.isEmpty() ? QStringLiteral("error") : QStringLiteral("ready");
        if (!entry.data.isEmpty()) {
            queueCovers({Game{0, slug, entry.data.value(QStringLiteral("name")).toString(),
                              entry.data.value(QStringLiteral("cover")).toString(), QString(), 0,
                              {QStringLiteral("Windows")}, {}}});
            bump();
            return;
        }
    }
    auto fallBack = [this, slug, path](const QString &state) {
        GameEntry &e = m_games[slug];
        QByteArray stale;
        qint64 modified = 0;
        if (readCache(path, -1, &stale, &modified) && !parseGame(stale).isEmpty()) {
            e.data = parseGame(stale);
            e.fetched = modified;
            e.state = QStringLiteral("ready");
        } else {
            e.state = state;
        }
        bump();
    };
    if (!m_networkEnabled) {
        fallBack(QStringLiteral("offline"));
        return;
    }
    if (rateLimited()) {
        fallBack(QStringLiteral("busy"));
        return;
    }
    entry.state = QStringLiteral("loading");
    bump();
    QNetworkReply *reply = get(QUrl(apiBase() + QStringLiteral("/api/games/") + slug));
    connect(reply, &QNetworkReply::finished, this, [this, reply, slug, path, fallBack] {
        reply->deleteLater();
        noteRateLimit(reply);
        const QByteArray body = reply->readAll();
        const QVariantMap data = reply->error() == QNetworkReply::NoError ? parseGame(body) : QVariantMap();
        if (data.isEmpty()) {
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            fallBack(status == 429 ? QStringLiteral("busy")
                     : status == 404 ? QStringLiteral("missing")
                     : isOffline(reply->error()) ? QStringLiteral("offline") : QStringLiteral("error"));
            return;
        }
        writeCache(path, body);
        GameEntry &e = m_games[slug];
        e.data = data;
        e.fetched = QDateTime::currentSecsSinceEpoch();
        e.state = QStringLiteral("ready");
        queueCovers({Game{0, slug, data.value(QStringLiteral("name")).toString(),
                          data.value(QStringLiteral("cover")).toString(), QString(), 0, {QStringLiteral("Windows")}, {}}});
        bump();
    });
}

QVariantMap LutrisSource::game(const QString &slug) const
{
    const auto it = m_games.constFind(slug);
    QVariantMap map = it != m_games.constEnd() ? it->data : QVariantMap();
    map.insert(QStringLiteral("slug"), slug);
    map.insert(QStringLiteral("state"), it != m_games.constEnd() ? it->state : QStringLiteral("none"));
    map.insert(QStringLiteral("status"), status(slug));
    map.insert(QStringLiteral("installed"), status(slug) == QLatin1String("installed"));
    map.insert(QStringLiteral("coverFile"), coverFor(slug, map.value(QStringLiteral("cover")).toString()));
    map.insert(QStringLiteral("lutrisUrl"), QStringLiteral("https://lutris.net/games/%1/").arg(slug));
    return map;
}

// ---- Installing and playing -------------------------------------------------------

QString LutrisSource::databasePath() const
{
    return m_appDir + QStringLiteral("/data/lutris/pga.db");
}

void LutrisSource::refreshLibrary()
{
    QSet<QString> library = readLibrary(databasePath());
    library.unite(m_fakeInstalled);
    if (library != m_library) {
        m_library = library;
        bump();
    }
}

QString LutrisSource::status(const QString &slug) const
{
    if (m_tracking.contains(slug))
        return QStringLiteral("installing");
    if (m_pendingSlug == slug && !slug.isEmpty())
        return QStringLiteral("lutris");
    if (m_library.contains(slug))
        return QStringLiteral("installed");
    return {};
}

void LutrisSource::run(const QStringList &args, std::function<void(int)> done)
{
    m_commandLog << (QStringList{QStringLiteral("flatpak")} + args);
    m_runner(QStringLiteral("flatpak"), args, std::move(done));
}

void LutrisSource::setStrictIsolation(bool strict)
{
    if (strict == m_strict)
        return;
    m_strict = strict;
    QSettings settings;
    settings.setValue(QStringLiteral("lutris/strictIsolation"), strict);
    bump();
    // Turning it off undoes only what the store did
    if (!strict && settings.value(QStringLiteral("lutris/isolationApplied")).toBool()) {
        run(isolationArguments(false), [](int) {});
        settings.setValue(QStringLiteral("lutris/isolationApplied"), false);
    } else if (strict && lutrisInstalled()) {
        applyIsolation({});
    }
}

void LutrisSource::applyIsolation(std::function<void()> then)
{
    if (!m_strict) {
        if (then)
            then();
        return;
    }
    QPointer<LutrisSource> self(this);
    run(isolationArguments(true), [self, then](int code) {
        if (!self)
            return;
        if (code == 0)
            QSettings().setValue(QStringLiteral("lutris/isolationApplied"), true);
        else
            qWarning() << "Could not restrict the Lutris flatpak (flatpak override exited with" << code << ")";
        if (then)
            then();
    });
}

// Games go to ~/Games/Lutris, away from a native Lutris' ~/Games/<game>
void LutrisSource::seedLutrisConfig() const
{
    if (m_fake)
        return; // a simulation changes nothing on disk
    QDir().mkpath(gamesFolder());
    const QString configDir = m_appDir + QStringLiteral("/config/lutris");
    const QString dataDir = m_appDir + QStringLiteral("/data/lutris");
    if (QFileInfo::exists(configDir + QStringLiteral("/system.yml")) || QFileInfo::exists(dataDir + QStringLiteral("/system.yml")))
        return; // the user's (or an earlier) Lutris configuration: left alone
    // Lutris reads ~/.config/lutris when it exists, else ~/.local/share/lutris
    const QString dir = QFileInfo::exists(configDir) ? configDir : dataDir;
    QDir().mkpath(dir);
    QString path = gamesFolder();
    path.replace(QLatin1Char('\\'), QLatin1String("\\\\")).replace(QLatin1Char('"'), QLatin1String("\\\""));
    QSaveFile file(dir + QStringLiteral("/system.yml"));
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QStringLiteral("system:\n  game_path: \"%1\"\n").arg(path).toUtf8());
        file.commit();
    }
}

bool LutrisSource::install(const QString &slug, const QString &installerSlug)
{
    if (!validSlug(slug) || !validSlug(installerSlug) || m_tracking.contains(slug))
        return false;
    if (!m_pendingSlug.isEmpty())
        return false; // Lutris itself is being installed for another game
    // Only installers this game's page offers
    const QVariantList installers = m_games.value(slug).data.value(QStringLiteral("installers")).toList();
    const bool known = std::any_of(installers.cbegin(), installers.cend(), [&](const QVariant &v) {
        return v.toMap().value(QStringLiteral("slug")).toString() == installerSlug;
    });
    if (!known)
        return false;
    m_tracking.remove(slug);
    if (!lutrisInstalled()) {
        m_pendingSlug = slug;
        m_pendingInstaller = installerSlug;
        bump();
        Q_EMIT lutrisInstallNeeded();
        return true;
    }
    m_pendingInstaller = installerSlug;
    startInstall(slug);
    return true;
}

void LutrisSource::lutrisInstallFinished(bool ok, const QString &error)
{
    const QString slug = m_pendingSlug;
    m_pendingSlug.clear();
    if (slug.isEmpty())
        return;
    if (!ok) {
        bump();
        Q_EMIT gameFinished(slug, m_games.value(slug).data.value(QStringLiteral("name")).toString(), false, error);
        return;
    }
    startInstall(slug);
}

void LutrisSource::cancelPending()
{
    if (m_pendingSlug.isEmpty())
        return;
    m_pendingSlug.clear();
    bump();
}

void LutrisSource::startInstall(const QString &slug)
{
    const QString installerSlug = m_pendingInstaller;
    m_pendingInstaller.clear();
    Tracking t;
    t.installerSlug = installerSlug;
    t.name = m_games.value(slug).data.value(QStringLiteral("name")).toString();
    t.started = QDateTime::currentSecsSinceEpoch();
    m_tracking.insert(slug, t);
    bump();
    seedLutrisConfig();
    QPointer<LutrisSource> self(this);
    applyIsolation([self, slug, installerSlug] {
        if (!self || !self->m_tracking.contains(slug))
            return;
        self->run(installArguments(installerSlug), [self, slug](int code) {
            if (!self || !self->m_tracking.contains(slug))
                return;
            if (code != 0) {
                const QString name = self->m_tracking.value(slug).name;
                self->m_tracking.remove(slug);
                self->bump();
                Q_EMIT self->gameFinished(slug, name, false, tr("Lutris could not be started."));
                return;
            }
            self->m_tracking[slug].processDone = true;
        });
        // Fake mode: Lutris "installs" the game after a few seconds
        if (self->m_fake) {
            QTimer::singleShot(qEnvironmentVariableIntValue("LINGMO_STORE_FAKE_DELAY") * 1000 + 4000, self,
                               [self, slug] { self->m_fakeInstalled.insert(slug); });
        }
    });
    m_poll.start();
}

void LutrisSource::stopTracking(const QString &slug)
{
    if (m_tracking.remove(slug) > 0)
        bump();
    if (m_tracking.isEmpty())
        m_poll.stop();
}

bool LutrisSource::defaultLutrisRunning() const
{
    // flatpak ps: one directory per running sandbox in $XDG_RUNTIME_DIR/.flatpak
    const QString runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + QStringLiteral("/.flatpak");
    const QDir dir(runtime);
    for (const QString &instance : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QFile info(dir.filePath(instance + QStringLiteral("/info")));
        if (!info.open(QIODevice::ReadOnly) || !info.readAll().contains(QByteArray("name=") + FlatpakId))
            continue;
        QFile bwrap(dir.filePath(instance + QStringLiteral("/bwrapinfo.json")));
        if (bwrap.open(QIODevice::ReadOnly)) {
            const int pid = QJsonDocument::fromJson(bwrap.readAll()).object().value(QStringLiteral("child-pid")).toInt();
            if (pid > 0 && QFileInfo::exists(QStringLiteral("/proc/%1").arg(pid)))
                return true;
        }
    }
    return false;
}

void LutrisSource::poll()
{
    refreshLibrary();
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const QStringList slugs = m_tracking.keys();
    bool running = false, checked = false;
    for (const QString &slug : slugs) {
        const Tracking t = m_tracking.value(slug);
        if (m_library.contains(slug) || m_library.contains(t.installerSlug)) {
            m_tracking.remove(slug);
            m_library.insert(slug);
            bump();
            Q_EMIT gameFinished(slug, t.name, true, QString());
            continue;
        }
        if (!t.processDone || now - t.started < m_startGrace)
            continue;
        if (!checked) {
            running = m_lutrisRunningCheck && m_lutrisRunningCheck();
            checked = true;
        }
        if (!running && !m_fake) {
            m_tracking.remove(slug);
            bump();
            Q_EMIT gameFinished(slug, t.name, false, tr("Lutris was closed before the installation finished."));
        }
    }
    if (m_tracking.isEmpty())
        m_poll.stop();
}

bool LutrisSource::play(const QString &slug)
{
    if (!validSlug(slug) || status(slug) != QLatin1String("installed"))
        return false;
    QPointer<LutrisSource> self(this);
    applyIsolation([self, slug] {
        if (self)
            self->run(playArguments(slug));
    });
    return true;
}
