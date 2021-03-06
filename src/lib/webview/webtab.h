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
#ifndef WEBTAB_H
#define WEBTAB_H

#include <QWidget>
#include <QIcon>
#include <QUrl>

#include "qzcommon.h"

class QVBoxLayout;
class QWebHistory;

class BrowserWindow;
class LocationBar;
class TabbedWebView;
class TabIcon;
class TabBar;

class QUPZILLA_EXPORT WebTab : public QWidget
{
    Q_OBJECT
public:
    struct SavedTab {
        QString title;
        QUrl url;
        QIcon icon;
        QByteArray history;

        SavedTab() { }
        SavedTab(WebTab* webTab);

        bool isEmpty() const;
        void clear();

        friend QUPZILLA_EXPORT QDataStream &operator<<(QDataStream &stream, const SavedTab &tab);
        friend QUPZILLA_EXPORT QDataStream &operator>>(QDataStream &stream, SavedTab &tab);
    };

    explicit WebTab(BrowserWindow* window);

    TabbedWebView* webView() const;
    LocationBar* locationBar() const;
    TabIcon* tabIcon() const;

    QUrl url() const;
    QString title() const;
    QIcon icon() const;
    QWebHistory* history() const;

    void moveToWindow(BrowserWindow* window);
    void setTabbed(int index);
    void setTabTitle(const QString &title);

    void setHistoryData(const QByteArray &data);
    QByteArray historyData() const;

    void stop();
    void reload();
    bool isLoading() const;

    bool isPinned() const;
    void setPinned(bool state);

    int tabIndex() const;
    void pinTab(int index);

    bool isCurrentTab() const;
    void setCurrentTab();

    bool inspectorVisible() const;
    void setInspectorVisible(bool v);

    bool isRestored() const;
    void restoreTab(const SavedTab &tab);
    void p_restoreTab(const SavedTab &tab);
    void p_restoreTab(const QUrl &url, const QByteArray &history);

    QPixmap renderTabPreview();

private slots:
    void showNotification(QWidget* notif);
    void slotRestore();

private:
    BrowserWindow* m_window;
    TabbedWebView* m_webView;
    LocationBar* m_locationBar;
    TabIcon* m_tabIcon;
    TabBar* m_tabBar;
    QVBoxLayout* m_layout;

    SavedTab m_savedTab;
    bool m_isPinned;
    bool m_inspectorVisible;
};

#endif // WEBTAB_H
