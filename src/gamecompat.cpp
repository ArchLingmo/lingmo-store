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

#include "gamecompat.h"
#include "errors.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrlQuery>

static const QString SteamSearch = QStringLiteral("https://store.steampowered.com/api/storesearch/");
static const QString ProtonDbSummary = QStringLiteral("https://www.protondb.com/api/v1/reports/summaries/%1.json");
static const QString ProtonDbPage = QStringLiteral("https://www.protondb.com/app/%1");
static const QString AntiCheatUrl = QStringLiteral(
    "https://raw.githubusercontent.com/AreWeAntiCheatYet/AreWeAntiCheatYet/master/games.json");
static constexpr qint64 AntiCheatMaxAge = 24 * 3600;
static constexpr qint64 ProtonDbMaxAge = 3 * 24 * 3600;
static constexpr int MaxResults = 6;

static QString foldName(const QString &name)
{
    QString out;
    for (const QChar c : name.normalized(QString::NormalizationForm_D)) {
        if (c.isLetterOrNumber())
            out.append(c.toLower());
    }
    return out;
}

static bool isOffline(QNetworkReply::NetworkError error)
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

GameCompat::GameCompat(QObject *parent)
    : QObject(parent)
    , m_network(new QNetworkAccessManager(this))
{
    m_network->setTransferTimeout(15000);
    QFile featured(QStringLiteral(":/data/featured.json"));
    if (featured.open(QIODevice::ReadOnly)) {
        const QJsonArray watch = QJsonDocument::fromJson(featured.readAll()).object().value(QStringLiteral("antiCheatWatch")).toArray();
        for (const QJsonValue &v : watch)
            m_watch << foldName(v.toString());
    }
    QFile cache(cachePath(QStringLiteral("protondb.json")));
    if (cache.open(QIODevice::ReadOnly)) {
        const QJsonObject all = QJsonDocument::fromJson(cache.readAll()).object();
        for (auto it = all.constBegin(); it != all.constEnd(); ++it)
            m_protonDb.insert(it.key(), it.value().toObject());
    }
}

QString GameCompat::cachePath(const QString &name) const
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QDir().mkpath(dir);
    return dir + QLatin1Char('/') + name;
}

void GameCompat::saveCache()
{
    QJsonObject all;
    for (auto it = m_protonDb.constBegin(); it != m_protonDb.constEnd(); ++it)
        all.insert(it.key(), it.value());
    QFile cache(cachePath(QStringLiteral("protondb.json")));
    if (cache.open(QIODevice::WriteOnly | QIODevice::Truncate))
        cache.write(QJsonDocument(all).toJson(QJsonDocument::Compact));
}

QNetworkReply *GameCompat::get(const QString &url)
{
    QNetworkRequest request{QUrl(url)};
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("lingmo-store/" LINGMO_STORE_VERSION));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    return m_network->get(request);
}

void GameCompat::clear()
{
    ++m_generation;
    m_results.clear();
    m_error.clear();
    m_query.clear();
    m_pending = 0;
    m_steamPending = false;
    Q_EMIT changed();
}

void GameCompat::loadAntiCheat()
{
    if (!m_antiCheat.isEmpty() || m_antiCheatLoading)
        return;
    const QString path = cachePath(QStringLiteral("anticheat.json"));
    const QFileInfo info(path);
    auto readCache = [this, path] {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly))
            m_antiCheat = QJsonDocument::fromJson(file.readAll()).array();
    };
    if (info.exists() && info.lastModified().secsTo(QDateTime::currentDateTime()) < AntiCheatMaxAge) {
        readCache();
        if (!m_antiCheat.isEmpty())
            return;
    }
    m_antiCheatLoading = true;
    ++m_pending;
    QNetworkReply *reply = get(AntiCheatUrl);
    connect(reply, &QNetworkReply::finished, this, [this, reply, path, readCache] {
        reply->deleteLater();
        m_antiCheatLoading = false;
        m_pending = qMax(0, m_pending - 1);
        const QByteArray body = reply->readAll();
        const QJsonArray games = QJsonDocument::fromJson(body).array();
        if (reply->error() == QNetworkReply::NoError && !games.isEmpty()) {
            m_antiCheat = games;
            QFile file(path);
            if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
                file.write(body);
        } else {
            readCache(); // stale is better than nothing
        }
        addAntiCheatOnly();
        for (int i = 0; i < m_results.size(); ++i)
            updateVerdict(i);
        Q_EMIT changed();
    });
}

void GameCompat::addAntiCheatOnly()
{
    if (m_query.isEmpty() || m_antiCheat.isEmpty() || m_steamPending)
        return;
    const QString query = foldName(m_query);
    if (query.size() < 3)
        return;
    QSet<QString> known;
    for (const QVariant &v : std::as_const(m_results)) {
        const QVariantMap r = v.toMap();
        known.insert(foldName(r.value(QStringLiteral("name")).toString()));
    }
    int added = 0;
    for (const QJsonValue &v : std::as_const(m_antiCheat)) {
        const QJsonObject game = v.toObject();
        const QString name = game.value(QStringLiteral("name")).toString();
        const QString folded = foldName(name);
        if (!folded.contains(query) || known.contains(folded))
            continue;
        // Only games Steam doesn't know about: the others come with a ProtonDB rating
        if (game.value(QStringLiteral("storeIds")).toObject().contains(QStringLiteral("steam")))
            continue;
        const QString slug = game.value(QStringLiteral("slug")).toString();
        m_results << QVariantMap{
            {QStringLiteral("appId"), QString()},
            {QStringLiteral("name"), name},
            {QStringLiteral("image"), QString()},
            {QStringLiteral("native"), game.value(QStringLiteral("native")).toBool()},
            {QStringLiteral("url"), QStringLiteral("https://areweanticheatyet.com/game/%1").arg(slug)},
            {QStringLiteral("verdict"), QStringLiteral("loading")},
            {QStringLiteral("antiCheatOnly"), true},
        };
        known.insert(folded);
        if (++added >= 3)
            break;
    }
    if (added > 0)
        m_error.clear();
}

void GameCompat::search(const QString &text)
{
    const QString query = text.simplified();
    if (query.isEmpty()) {
        clear();
        return;
    }
    const int generation = ++m_generation;
    m_query = query;
    m_error.clear();
    m_results.clear();
    m_pending = 0;
    m_steamPending = false;
    loadAntiCheat();

    const QString key = foldName(query);
    if (m_steamSearch.contains(key)) {
        onSteamResults(m_steamSearch.value(key), generation);
        return;
    }

    QUrl url(SteamSearch);
    QUrlQuery params;
    params.addQueryItem(QStringLiteral("term"), query);
    params.addQueryItem(QStringLiteral("l"), QStringLiteral("portuguese"));
    params.addQueryItem(QStringLiteral("cc"), QStringLiteral("BR"));
    url.setQuery(params);
    ++m_pending;
    m_steamPending = true;
    Q_EMIT changed();
    QNetworkReply *reply = get(url.toString());
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation, key] {
        reply->deleteLater();
        if (generation != m_generation)
            return;
        m_pending = qMax(0, m_pending - 1);
        m_steamPending = false;
        if (reply->error() != QNetworkReply::NoError) {
            m_error = isOffline(reply->error()) ? Errors::offline()
                                                : tr("The Steam store did not answer. Try again in a moment.");
            Q_EMIT changed();
            return;
        }
        const QJsonArray items = QJsonDocument::fromJson(reply->readAll()).object().value(QStringLiteral("items")).toArray();
        m_steamSearch.insert(key, items);
        onSteamResults(items, generation);
    });
}

void GameCompat::onSteamResults(const QJsonArray &items, int generation)
{
    for (const QJsonValue &v : items) {
        const QJsonObject item = v.toObject();
        if (item.value(QStringLiteral("type")).toString() != QLatin1String("app"))
            continue;
        // Soundtracks, artbooks and other extras aren't games
        static const QRegularExpression extras(
            QStringLiteral("\\b(soundtrack|ost|artbook|art book|season pass|dlc|wallpapers?|demo)\\b"),
            QRegularExpression::CaseInsensitiveOption);
        const QString name = item.value(QStringLiteral("name")).toString();
        if (extras.match(name).hasMatch() && !extras.match(m_query).hasMatch())
            continue;
        const QString appId = QString::number(item.value(QStringLiteral("id")).toInteger());
        m_results << QVariantMap{
            {QStringLiteral("appId"), appId},
            {QStringLiteral("name"), item.value(QStringLiteral("name")).toString()},
            {QStringLiteral("image"), item.value(QStringLiteral("tiny_image")).toString()},
            {QStringLiteral("native"), item.value(QStringLiteral("platforms")).toObject().value(QStringLiteral("linux")).toBool()},
            {QStringLiteral("url"), ProtonDbPage.arg(appId)},
            {QStringLiteral("verdict"), QStringLiteral("loading")},
        };
        if (m_results.size() >= MaxResults)
            break;
    }
    addAntiCheatOnly();
    if (m_results.isEmpty())
        m_error = tr("No game called “%1” was found.").arg(m_query);
    for (int i = 0; i < m_results.size(); ++i) {
        updateVerdict(i);
        const QString appId = m_results.at(i).toMap().value(QStringLiteral("appId")).toString();
        if (!appId.isEmpty())
            fetchProtonDb(appId, generation);
    }
    Q_EMIT changed();
}

void GameCompat::fetchProtonDb(const QString &appId, int generation)
{
    const QJsonObject cached = m_protonDb.value(appId);
    if (!cached.isEmpty()
        && QDateTime::currentSecsSinceEpoch() - cached.value(QStringLiteral("fetched")).toInteger() < ProtonDbMaxAge)
        return;
    ++m_pending;
    QNetworkReply *reply = get(ProtonDbSummary.arg(appId));
    connect(reply, &QNetworkReply::finished, this, [this, reply, appId, generation] {
        reply->deleteLater();
        if (generation != m_generation)
            return;
        m_pending = qMax(0, m_pending - 1);
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QJsonObject summary;
        if (reply->error() == QNetworkReply::NoError) {
            summary = QJsonDocument::fromJson(reply->readAll()).object();
        } else if (status == 404) {
            summary.insert(QStringLiteral("tier"), QStringLiteral("none")); // nobody reported it yet
        } else {
            if (isOffline(reply->error()) && m_protonDb.value(appId).isEmpty())
                m_error = Errors::offline();
        }
        if (!summary.isEmpty()) {
            summary.insert(QStringLiteral("fetched"), QDateTime::currentSecsSinceEpoch());
            m_protonDb.insert(appId, summary);
            saveCache();
        } else if (!m_protonDb.contains(appId)) {
            m_protonDb.insert(appId, QJsonObject{{QStringLiteral("tier"), QStringLiteral("error")}});
        }
        for (int i = 0; i < m_results.size(); ++i) {
            if (m_results.at(i).toMap().value(QStringLiteral("appId")).toString() == appId)
                updateVerdict(i);
        }
        Q_EMIT changed();
    });
}

QJsonObject GameCompat::antiCheatFor(const QString &appId, const QString &name) const
{
    const QString folded = foldName(name);
    for (const QJsonValue &v : m_antiCheat) {
        const QJsonObject game = v.toObject();
        const QJsonValue steam = game.value(QStringLiteral("storeIds")).toObject().value(QStringLiteral("steam"));
        if (steam.toString() == appId || (steam.isDouble() && QString::number(steam.toInteger()) == appId))
            return game;
    }
    for (const QJsonValue &v : m_antiCheat) {
        const QJsonObject game = v.toObject();
        if (foldName(game.value(QStringLiteral("name")).toString()) == folded)
            return game;
    }
    return {};
}

void GameCompat::updateVerdict(int index)
{
    QVariantMap result = m_results.at(index).toMap();
    const QString appId = result.value(QStringLiteral("appId")).toString();
    const QJsonObject proton = m_protonDb.value(appId);
    const QJsonObject antiCheat = antiCheatFor(appId, result.value(QStringLiteral("name")).toString());
    const QString status = antiCheat.value(QStringLiteral("status")).toString();
    QStringList engines;
    for (const QJsonValue &v : antiCheat.value(QStringLiteral("anticheats")).toArray())
        engines << v.toString();
    const bool native = result.value(QStringLiteral("native")).toBool();
    QString tier = proton.value(QStringLiteral("tier")).toString();
    if (tier.isEmpty() && proton.contains(QStringLiteral("trendingTier")))
        tier = proton.value(QStringLiteral("trendingTier")).toString();
    const int reports = proton.value(QStringLiteral("total")).toInt();

    static const QHash<QString, const char *> tierNames = {
        {QStringLiteral("platinum"), QT_TR_NOOP("Platinum")}, {QStringLiteral("gold"), QT_TR_NOOP("Gold")},
        {QStringLiteral("silver"), QT_TR_NOOP("Silver")},     {QStringLiteral("bronze"), QT_TR_NOOP("Bronze")},
        {QStringLiteral("borked"), QT_TR_NOOP("Borked")},
    };

    QString verdict, detail;
    QStringList notes;
    if (status == QLatin1String("Denied") || status == QLatin1String("Broken")) {
        verdict = QStringLiteral("anticheat");
        notes << (status == QLatin1String("Denied")
                      ? tr("The developer blocks Linux players in its anti-cheat (%1).").arg(engines.join(QStringLiteral(", ")))
                      : tr("Its anti-cheat (%1) does not work on Linux, so online play is blocked.").arg(engines.join(QStringLiteral(", "))));
    } else if (result.value(QStringLiteral("antiCheatOnly")).toBool()) {
        if (status == QLatin1String("Supported") || status == QLatin1String("Running"))
            verdict = QStringLiteral("great");
        else
            verdict = QStringLiteral("unknown");
        notes << tr("Not sold on Steam; its anti-cheat status comes from Are We Anti-Cheat Yet.");
    } else if (native) {
        verdict = QStringLiteral("great");
        notes << tr("Has a native Linux version.");
    } else if (tier == QLatin1String("platinum") || tier == QLatin1String("gold") || tier == QLatin1String("native")) {
        verdict = QStringLiteral("great");
    } else if (tier == QLatin1String("silver") || tier == QLatin1String("bronze")) {
        verdict = QStringLiteral("tweaks");
    } else if (tier == QLatin1String("borked")) {
        verdict = QStringLiteral("broken");
    } else if (proton.isEmpty() && !m_error.isEmpty()) {
        verdict = QStringLiteral("unknown");
    } else if (proton.isEmpty()) {
        verdict = QStringLiteral("loading");
    } else {
        verdict = QStringLiteral("unknown");
    }
    if (tierNames.contains(tier)) {
        notes << tr("ProtonDB rating: %1 (%n report(s)).", nullptr, reports).arg(tr(tierNames.value(tier)));
    }
    if (!engines.isEmpty() && (status == QLatin1String("Supported") || status == QLatin1String("Running")))
        notes << tr("Its anti-cheat works on Linux.");
    if (verdict == QStringLiteral("tweaks"))
        notes << tr("Some players needed to change settings or pick another Proton version.");

    if (verdict == QLatin1String("great"))
        detail = tr("Works great");
    else if (verdict == QLatin1String("tweaks"))
        detail = tr("Works with tweaks");
    else if (verdict == QLatin1String("anticheat"))
        detail = tr("Doesn't work (anti-cheat)");
    else if (verdict == QLatin1String("broken"))
        detail = tr("Doesn't work");
    else if (verdict == QLatin1String("loading"))
        detail = tr("Checking…");
    else
        detail = tr("Not enough reports yet");

    result.insert(QStringLiteral("verdict"), verdict);
    result.insert(QStringLiteral("verdictText"), detail);
    result.insert(QStringLiteral("notes"), notes.join(QLatin1Char(' ')));
    m_results[index] = result;
}

void GameCompat::loadWatchlist()
{
    loadAntiCheat();
    Q_EMIT changed();
}

QVariantList GameCompat::blockedGames() const
{
    QVariantList list;
    if (m_antiCheat.isEmpty())
        return list;
    for (const QString &wanted : m_watch) {
        for (const QJsonValue &v : m_antiCheat) {
            const QJsonObject game = v.toObject();
            if (foldName(game.value(QStringLiteral("name")).toString()) != wanted)
                continue;
            const QString status = game.value(QStringLiteral("status")).toString();
            if (status != QLatin1String("Denied") && status != QLatin1String("Broken"))
                break;
            QStringList engines;
            for (const QJsonValue &e : game.value(QStringLiteral("anticheats")).toArray())
                engines << e.toString();
            list << QVariantMap{
                {QStringLiteral("name"), game.value(QStringLiteral("name")).toString()},
                {QStringLiteral("status"), status},
                {QStringLiteral("reason"), status == QLatin1String("Denied")
                                               ? tr("The developer blocks Linux")
                                               : tr("The anti-cheat doesn't run on Linux")},
                {QStringLiteral("anticheats"), engines.join(QStringLiteral(", "))},
                {QStringLiteral("url"), QStringLiteral("https://areweanticheatyet.com/game/%1")
                                            .arg(game.value(QStringLiteral("slug")).toString())},
            };
            break;
        }
    }
    return list;
}

QVariantMap GameCompat::antiCheatVerdict(const QJsonObject &game)
{
    if (game.isEmpty())
        return {};
    const QString status = game.value(QStringLiteral("status")).toString();
    QStringList engines;
    for (const QJsonValue &e : game.value(QStringLiteral("anticheats")).toArray())
        engines << e.toString();
    const bool blocked = status == QLatin1String("Denied") || status == QLatin1String("Broken");
    QString text;
    if (status == QLatin1String("Denied"))
        text = tr("The developer blocks Linux players in its anti-cheat (%1).").arg(engines.join(QStringLiteral(", ")));
    else if (status == QLatin1String("Broken"))
        text = tr("Its anti-cheat (%1) does not work on Linux, so online play is blocked.").arg(engines.join(QStringLiteral(", ")));
    else if (status == QLatin1String("Supported") || status == QLatin1String("Running"))
        text = engines.isEmpty() ? QString() : tr("Its anti-cheat works on Linux.");
    else
        text = tr("Its anti-cheat (%1) has not been confirmed to work on Linux.").arg(engines.join(QStringLiteral(", ")));
    return {
        {QStringLiteral("status"), status},
        {QStringLiteral("anticheats"), engines.join(QStringLiteral(", "))},
        {QStringLiteral("blocked"), blocked},
        {QStringLiteral("text"), text},
        {QStringLiteral("url"), QStringLiteral("https://areweanticheatyet.com/game/%1")
                                    .arg(game.value(QStringLiteral("slug")).toString())},
    };
}

QVariantMap GameCompat::antiCheatStatus(const QString &name) const
{
    const QString wanted = foldName(name);
    if (wanted.isEmpty())
        return {};
    for (const QJsonValue &v : m_antiCheat) {
        const QJsonObject game = v.toObject();
        if (foldName(game.value(QStringLiteral("name")).toString()) == wanted)
            return antiCheatVerdict(game);
    }
    return {};
}
