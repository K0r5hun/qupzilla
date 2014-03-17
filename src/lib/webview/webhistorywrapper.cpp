/* ============================================================
* QupZilla - WebKit based browser
* Copyright (C) 2010-2014  David Rosca <nowrep@gmail.com>
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
* ============================================================ */
#include "webhistorywrapper.h"

#include <QUrl>
#include <QVariant>
#include <QWebEngineHistory>

QList<QWebEngineHistoryItem> WebHistoryWrapper::forwardItems(int maxItems, QWebEngineHistory* history)
{
    QList<QWebEngineHistoryItem> list;
    QUrl lastUrl = history->currentItem().url();

    int count = 0;
    foreach (const QWebEngineHistoryItem &item, history->forwardItems(maxItems + 5)) {
        if (item.url() == lastUrl || count == maxItems) {
            continue;
        }

        lastUrl = item.url();
        list.append(item);
        count++;
    }

    return list;
}

QList<QWebEngineHistoryItem> WebHistoryWrapper::backItems(int maxItems, QWebEngineHistory* history)
{
    QList<QWebEngineHistoryItem> list;
    QUrl lastUrl = history->currentItem().url();

    int count = 0;
    QList<QWebEngineHistoryItem> bItems = history->backItems(maxItems + 5);
    for (int i = bItems.count() - 1; i >= 0; i--) {
        QWebEngineHistoryItem item = bItems.at(i);
        if (item.url() == lastUrl || count == maxItems) {
            continue;
        }

        lastUrl = item.url();
        list.append(item);
        count++;
    }

    return list;
}

bool WebHistoryWrapper::canGoForward(QWebEngineHistory* history)
{
    return !forwardItems(1, history).isEmpty();
}

bool WebHistoryWrapper::canGoBack(QWebEngineHistory* history)
{
    return !backItems(1, history).isEmpty();
}

void WebHistoryWrapper::goBack(QWebEngineHistory* history)
{
    QList<QWebEngineHistoryItem> items = backItems(1, history);

    if (items.isEmpty()) {
        return;
    }

    history->goToItem(items.at(0));
}

void WebHistoryWrapper::goForward(QWebEngineHistory* history)
{
    QList<QWebEngineHistoryItem> items = forwardItems(1, history);

    if (items.isEmpty()) {
        return;
    }

    history->goToItem(items.at(0));
}

int WebHistoryWrapper::indexOfItem(const QList<QWebEngineHistoryItem> &list, const QWebEngineHistoryItem &item)
{
    for (int i = 0; i < list.count(); i++) {
        QWebEngineHistoryItem it = list.at(i);

        if (it.lastVisited() == item.lastVisited() &&
                it.originalUrl() == item.originalUrl() &&
                it.title() == item.title() &&
                it.url() == item.url()) {
            return i;
        }
    }

    return -1;
}
