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
#ifndef WEBHISTORYWRAPPER_H
#define WEBHISTORYWRAPPER_H

#include <QList>

#include "qzcommon.h"

class QWebEngineHistory;
class QWebEngineHistoryItem;

class WebHistoryWrapper
{
public:
    static QList<QWebEngineHistoryItem> forwardItems(int maxItems, QWebEngineHistory* history);
    static QList<QWebEngineHistoryItem> backItems(int maxItems, QWebEngineHistory* history);

    static bool canGoForward(QWebEngineHistory* history);
    static bool canGoBack(QWebEngineHistory* history);

    static void goBack(QWebEngineHistory* history);
    static void goForward(QWebEngineHistory* history);

    static int indexOfItem(const QList<QWebEngineHistoryItem> &list, const QWebEngineHistoryItem &item);
};

#endif // WEBHISTORYWRAPPER_H
