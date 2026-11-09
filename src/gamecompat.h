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
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QVariantList>

class QNetworkAccessManager;
class QNetworkReply;

// "Does my game work?": finds the game on the Steam store, then combines ProtonDB's
// rating with Are We Anti-Cheat Yet into one friendly verdict. Answers are cached in
// memory and on disk, so a second look works offline.
class GameCompat : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QVariantList results READ results NOTIFY changed)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY changed)
    Q_PROPERTY(QString query READ query NOTIFY changed)
    // Popular games (featured.json "antiCheatWatch") whose anti-cheat keeps Linux out
    Q_PROPERTY(QVariantList blockedGames READ blockedGames NOTIFY changed)

public:
    explicit GameCompat(QObject *parent = nullptr);

    bool busy() const { return m_pending > 0; }
    QVariantList results() const { return m_results; }
    QString errorMessage() const { return m_error; }
    QString query() const { return m_query; }

    Q_INVOKABLE void search(const QString &text);
    Q_INVOKABLE void clear();
    // Downloads (or reads the cached) Are We Anti-Cheat Yet list for blockedGames
    Q_INVOKABLE void loadWatchlist();
    QVariantList blockedGames() const;
    // Are We Anti-Cheat Yet's entry for a game by name (Lutris games):
    // {status, anticheats, blocked, url, text}, empty when unknown. Call loadWatchlist() first.
    Q_INVOKABLE QVariantMap antiCheatStatus(const QString &name) const;
    // The same verdict from one Are We Anti-Cheat Yet entry (tests)
    static QVariantMap antiCheatVerdict(const QJsonObject &game);

Q_SIGNALS:
    void changed();

private:
    QNetworkReply *get(const QString &url);
    void loadAntiCheat();
    void onSteamResults(const QJsonArray &items, int generation);
    void fetchProtonDb(const QString &appId, int generation);
    void updateVerdict(int index);
    // Games that aren't on Steam (Fortnite, Valorant…) are still in Are We Anti-Cheat Yet
    void addAntiCheatOnly();
    QJsonObject antiCheatFor(const QString &appId, const QString &name) const;
    QString cachePath(const QString &name) const;
    void saveCache();

    QNetworkAccessManager *m_network;
    QVariantList m_results;
    QString m_error;
    QString m_query;
    int m_pending = 0;
    int m_generation = 0;
    bool m_steamPending = false;
    QJsonArray m_antiCheat;
    QStringList m_watch;
    bool m_antiCheatLoading = false;
    QHash<QString, QJsonObject> m_protonDb;    // appid -> summary (+ "fetched")
    QHash<QString, QJsonArray> m_steamSearch;  // folded query -> items
};
