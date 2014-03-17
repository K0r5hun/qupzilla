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
#include "webpage.h"
#include "tabbedwebview.h"
#include "tabwidget.h"
#include "browserwindow.h"
#include "pluginproxy.h"
#include "downloadmanager.h"
#include "webpluginfactory.h"
#include "mainapplication.h"
#include "checkboxdialog.h"
#include "widget.h"
#include "qztools.h"
#include "speeddial.h"
#include "autofill.h"
#include "popupwebpage.h"
#include "popupwebview.h"
#include "networkmanagerproxy.h"
#include "adblockicon.h"
#include "adblockmanager.h"
#include "iconprovider.h"
#include "qzsettings.h"
#include "useragentmanager.h"
#include "delayedfilewatcher.h"
#include "recoverywidget.h"
#include "html5permissions/html5permissionsmanager.h"
#include "schemehandlers/fileschemehandler.h"
#include "javascript/externaljsobject.h"

#ifdef NONBLOCK_JS_DIALOGS
#include "ui_jsconfirm.h"
#include "ui_jsalert.h"
#include "ui_jsprompt.h"

#include <QPushButton>
#endif

#include <QAuthenticator>
#include <QDir>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QWebEngineHistory>
#include <QTimer>
#include <QNetworkReply>
#include <QDesktopServices>
#include <QMessageBox>
#include <QFileDialog>
#include <QCheckBox>

QString WebPage::s_lastUploadLocation = QDir::homePath();
QUrl WebPage::s_lastUnsupportedUrl;
QTime WebPage::s_lastUnsupportedUrlTime;
QList<WebPage*> WebPage::s_livingPages;

WebPage::WebPage(QObject* parent)
    : QWebEnginePage(parent)
    , m_view(0)
    , m_fileWatcher(0)
    , m_runningLoop(0)
    , m_loadProgress(-1)
    , m_blockAlerts(false)
    , m_secureStatus(false)
    , m_adjustingScheduled(false)
{
#if QTWEBENGINE_DISABLED
    m_javaScriptEnabled = QWebEngineSettings::globalSettings()->testAttribute(QWebEngineSettings::JavascriptEnabled);

    m_networkProxy = new NetworkManagerProxy(this);
    m_networkProxy->setPrimaryNetworkAccessManager(mApp->networkManager());
    m_networkProxy->setPage(this);
    setNetworkAccessManager(m_networkProxy);

    setForwardUnsupportedContent(true);
    setPluginFactory(new WebPluginFactory(this));
#endif
    history()->setMaximumItemCount(20);

    connect(this, SIGNAL(unsupportedContent(QNetworkReply*)), this, SLOT(handleUnsupportedContent(QNetworkReply*)));
    connect(this, SIGNAL(loadProgress(int)), this, SLOT(progress(int)));
    connect(this, SIGNAL(loadFinished(bool)), this, SLOT(finished()));
    connect(this, SIGNAL(printRequested(QWebEngineFrame*)), this, SLOT(printFrame(QWebEngineFrame*)));
    connect(this, SIGNAL(downloadRequested(QNetworkRequest)), this, SLOT(downloadRequested(QNetworkRequest)));
    connect(this, SIGNAL(windowCloseRequested()), this, SLOT(windowCloseRequested()));
    connect(this, SIGNAL(authenticationRequired(QUrl,QAuthenticator*)), this, SLOT(authentication(QUrl,QAuthenticator*)));
    connect(this, SIGNAL(proxyAuthenticationRequired(QUrl,QAuthenticator*,QString)), this, SLOT(proxyAuthentication(QUrl,QAuthenticator*,QString)));

    connect(this, SIGNAL(databaseQuotaExceeded(QWebEngineFrame*,QString)),
            this, SLOT(dbQuotaExceeded(QWebEngineFrame*)));

#if QTWEBENGINE_DISABLED
    connect(mainFrame(), SIGNAL(javaScriptWindowObjectCleared()), this, SLOT(addJavaScriptObject()));

#if QTWEBKIT_FROM_2_2
    connect(this, SIGNAL(featurePermissionRequested(QWebEngineFrame*,QWebEnginePage::Feature)),
            this, SLOT(featurePermissionRequested(QWebEngineFrame*,QWebEnginePage::Feature)));
#endif

#if QTWEBKIT_FROM_2_3
    connect(this, SIGNAL(applicationCacheQuotaExceeded(QWebSecurityOrigin*,quint64,quint64)),
            this, SLOT(appCacheQuotaExceeded(QWebSecurityOrigin*,quint64)));
#elif QTWEBKIT_FROM_2_2
    connect(this, SIGNAL(applicationCacheQuotaExceeded(QWebSecurityOrigin*,quint64)),
            this, SLOT(appCacheQuotaExceeded(QWebSecurityOrigin*,quint64)));
#endif

#endif

    s_livingPages.append(this);
}

WebPage::~WebPage()
{
    mApp->plugins()->emitWebPageDeleted(this);

    if (m_runningLoop) {
        m_runningLoop->exit(1);
        m_runningLoop = 0;
    }

    s_livingPages.removeOne(this);

#if QTWEBENGINE_DISABLED
    // Page's network manager will be deleted and then set to null
    // Fixes issue with network manager being used after deleted in destructor
    setNetworkAccessManager(0);
#endif
}

void WebPage::setWebView(TabbedWebView* view)
{
    if (m_view == view) {
        return;
    }

    if (m_view) {
        delete m_view;
        m_view = 0;
    }

    m_view = view;
    m_view->setWebPage(this);

    connect(m_view, SIGNAL(urlChanged(QUrl)), this, SLOT(urlChanged(QUrl)));
}

void WebPage::scheduleAdjustPage()
{
    WebView* webView = qobject_cast<WebView*>(view());
    if (!webView) {
        return;
    }

    if (webView->isLoading()) {
        m_adjustingScheduled = true;
    }
    else {
        const QSize originalSize = webView->size();
        QSize newSize(originalSize.width() - 1, originalSize.height() - 1);

        webView->resize(newSize);
        webView->resize(originalSize);
    }
}

bool WebPage::loadingError() const
{
#if QTWEBENGINE_DISABLED
    return !mainFrame()->findFirstElement("span[id=\"qupzilla-error-page\"]").isNull();
#else
    return false;
#endif
}

void WebPage::addRejectedCerts(const QList<QSslCertificate> &certs)
{
    foreach (const QSslCertificate &cert, certs) {
        if (!m_rejectedSslCerts.contains(cert)) {
            m_rejectedSslCerts.append(cert);
        }
    }
}

bool WebPage::containsRejectedCerts(const QList<QSslCertificate> &certs)
{
    int matches = 0;

    foreach (const QSslCertificate &cert, certs) {
        if (m_rejectedSslCerts.contains(cert)) {
            ++matches;
        }

        if (m_sslCert == cert) {
            m_sslCert.clear();
        }
    }

    return matches == certs.count();
}

#if QTWEBENGINE_DISABLED
QWebElement WebPage::activeElement() const
{
    QRect activeRect = inputMethodQuery(Qt::ImMicroFocus).toRect();
    return mainFrame()->hitTestContent(activeRect.center()).element();
}
#endif

bool WebPage::isRunningLoop()
{
    return m_runningLoop;
}

bool WebPage::isLoading() const
{
    return m_loadProgress < 100;
}

void WebPage::urlChanged(const QUrl &url)
{
#if QTWEBENGINE_DISABLED
    // Make sure JavaScript is enabled for qupzilla pages regardless of user settings
    if (url.scheme() == QLatin1String("qupzilla")) {
        settings()->setAttribute(QWebEngineSettings::JavascriptEnabled, true);
    }
#endif

    if (isLoading()) {
        m_adBlockedEntries.clear();
        m_blockAlerts = false;
    }
}

void WebPage::progress(int prog)
{
    m_loadProgress = prog;

    bool secStatus = QzTools::isCertificateValid(sslCertificate());

    if (secStatus != m_secureStatus) {
        m_secureStatus = secStatus;
        emit privacyChanged(QzTools::isCertificateValid(sslCertificate()));
    }
}

void WebPage::finished()
{
    progress(100);

    if (m_adjustingScheduled) {
        m_adjustingScheduled = false;
        setZoomFactor(zoomFactor() + 1);
        setZoomFactor(zoomFactor() - 1);
    }

    // File scheme watcher
    if (url().scheme() == QLatin1String("file")) {
        QFileInfo info(url().toLocalFile());
        if (info.isFile()) {
            if (!m_fileWatcher) {
                m_fileWatcher = new DelayedFileWatcher(this);
                connect(m_fileWatcher, SIGNAL(delayedFileChanged(QString)), this, SLOT(watchedFileChanged(QString)));
            }

            const QString filePath = url().toLocalFile();

            if (QFile::exists(filePath) && !m_fileWatcher->files().contains(filePath)) {
                m_fileWatcher->addPath(filePath);
            }
        }
    }
    else if (m_fileWatcher && !m_fileWatcher->files().isEmpty()) {
        m_fileWatcher->removePaths(m_fileWatcher->files());
    }

    // Autofill
    m_passwordEntries = mApp->autoFill()->completePage(this);

    // AdBlock
    cleanBlockedObjects();
}

void WebPage::watchedFileChanged(const QString &file)
{
    if (url().toLocalFile() == file) {
        triggerAction(QWebEnginePage::Reload);
    }
}

void WebPage::printFrame(QWebEngineFrame* frame)
{
    WebView* webView = qobject_cast<WebView*>(view());
    if (!webView) {
        return;
    }

    webView->printPage(frame);
}

void WebPage::addJavaScriptObject()
{
#if QTWEBENGINE_DISABLED
    // Make sure all other sites have JavaScript set by user preferences
    // (JavaScript is enabled in WebPage::urlChanged)
    if (url().scheme() != QLatin1String("qupzilla")) {
        settings()->setAttribute(QWebEngineSettings::JavascriptEnabled, m_javaScriptEnabled);
    }

    ExternalJsObject* jsObject = new ExternalJsObject(this);
    addToJavaScriptWindowObject("external", jsObject);

    if (url().toString() == QLatin1String("qupzilla:speeddial")) {
        jsObject->setOnSpeedDial(true);
        mApp->plugins()->speedDial()->addWebFrame(mainFrame());
    }
#endif
}

void WebPage::handleUnsupportedContent(QNetworkReply* reply)
{
    if (!reply) {
        return;
    }

    const QUrl url = reply->url();

    switch (reply->error()) {
    case QNetworkReply::NoError:
        if (reply->header(QNetworkRequest::ContentTypeHeader).isValid()) {
            QString requestUrl = reply->request().url().toString(QUrl::RemoveFragment | QUrl::RemoveQuery);
#if QTWEBENGINE_DISABLED
            if (requestUrl.endsWith(QLatin1String(".swf"))) {
                const QWebElement docElement = mainFrame()->documentElement();
                const QWebElement object = docElement.findFirst(QString("object[src=\"%1\"]").arg(requestUrl));
                const QWebElement embed = docElement.findFirst(QString("embed[src=\"%1\"]").arg(requestUrl));

                if (!object.isNull() || !embed.isNull()) {
                    qDebug() << "WebPage::UnsupportedContent" << url << "Attempt to download flash object on site!";
                    reply->deleteLater();
                    return;
                }
            }
            DownloadManager* dManager = mApp->downloadManager();
            dManager->handleUnsupportedContent(reply, this);
#endif
            return;
        }
        // Falling unsupported content with invalid ContentTypeHeader to be handled as UnknownProtocol

    case QNetworkReply::ProtocolUnknownError: {
        if (url.scheme() == QLatin1String("file")) {
            FileSchemeHandler::handleUrl(url);
            return;
        }

        qDebug() << "WebPage::UnsupportedContent" << url << "ProtocolUnknowError";
        desktopServicesOpen(url);

        reply->deleteLater();
        return;
    }
    default:
        break;
    }

    qDebug() << "WebPage::UnsupportedContent error" << url << reply->errorString();
    reply->deleteLater();
}

void WebPage::handleUnknownProtocol(const QUrl &url)
{
    const QString protocol = url.scheme();

    if (protocol == QLatin1String("mailto")) {
        desktopServicesOpen(url);
        return;
    }

    if (qzSettings->blockedProtocols.contains(protocol)) {
        qDebug() << "WebPage::handleUnknownProtocol Protocol" << protocol << "is blocked!";
        return;
    }

    if (qzSettings->autoOpenProtocols.contains(protocol)) {
        desktopServicesOpen(url);
        return;
    }

    CheckBoxDialog dialog(QDialogButtonBox::Yes | QDialogButtonBox::No, view());

    const QString wrappedUrl = QzTools::alignTextToWidth(url.toString(), "<br/>", dialog.fontMetrics(), 450);
    const QString text = tr("QupZilla cannot handle <b>%1:</b> links. The requested link "
                            "is <ul><li>%2</li></ul>Do you want QupZilla to try "
                            "open this link in system application?").arg(protocol, wrappedUrl);

    dialog.setText(text);
    dialog.setCheckBoxText(tr("Remember my choice for this protocol"));
    dialog.setWindowTitle(tr("External Protocol Request"));
    dialog.setIcon(IconProvider::standardIcon(QStyle::SP_MessageBoxQuestion));

    switch (dialog.exec()) {
    case QDialog::Accepted:
        if (dialog.isChecked()) {
            qzSettings->autoOpenProtocols.append(protocol);
            qzSettings->saveSettings();
        }


        QDesktopServices::openUrl(url);
        break;

    case QDialog::Rejected:
        if (dialog.isChecked()) {
            qzSettings->blockedProtocols.append(protocol);
            qzSettings->saveSettings();
        }

        break;

    default:
        break;
    }
}

void WebPage::desktopServicesOpen(const QUrl &url)
{
    // Open same url only once in 2 secs
    const int sameUrlTimeout = 2 * 1000;

    if (s_lastUnsupportedUrl != url || s_lastUnsupportedUrlTime.isNull() || s_lastUnsupportedUrlTime.elapsed() > sameUrlTimeout) {
        s_lastUnsupportedUrl = url;
        s_lastUnsupportedUrlTime.restart();
        QDesktopServices::openUrl(url);
    }
    else {
        qWarning() << "WebPage::desktopServicesOpen Url" << url << "has already been opened!\n"
                   "Ignoring it to prevent infinite loop!";
    }
}

void WebPage::downloadRequested(const QNetworkRequest &request)
{
    DownloadManager* dManager = mApp->downloadManager();
    dManager->download(request, this);
}

void WebPage::windowCloseRequested()
{
    WebView* webView = qobject_cast<WebView*>(view());
    if (!webView) {
        return;
    }

    webView->closeView();
}

void WebPage::authentication(const QUrl &requestUrl, QAuthenticator* auth)
{
    QDialog* dialog = new QDialog();
    dialog->setWindowTitle(tr("Authorisation required"));

    QFormLayout* formLa = new QFormLayout(dialog);

    QLabel* label = new QLabel(dialog);
    QLabel* userLab = new QLabel(dialog);
    QLabel* passLab = new QLabel(dialog);
    userLab->setText(tr("Username: "));
    passLab->setText(tr("Password: "));

    QLineEdit* user = new QLineEdit(dialog);
    QLineEdit* pass = new QLineEdit(dialog);
    pass->setEchoMode(QLineEdit::Password);
    QCheckBox* save = new QCheckBox(dialog);
    save->setText(tr("Save username and password on this site"));

    QDialogButtonBox* box = new QDialogButtonBox(dialog);
    box->addButton(QDialogButtonBox::Ok);
    box->addButton(QDialogButtonBox::Cancel);
    connect(box, SIGNAL(rejected()), dialog, SLOT(reject()));
    connect(box, SIGNAL(accepted()), dialog, SLOT(accept()));

    label->setText(tr("A username and password are being requested by %1. "
                      "The site says: \"%2\"").arg(requestUrl.host(), QzTools::escape(auth->realm())));

    formLa->addRow(label);
    formLa->addRow(userLab, user);
    formLa->addRow(passLab, pass);
    formLa->addRow(save);
    formLa->addWidget(box);

    AutoFill* fill = mApp->autoFill();
    QString storedUser;
    QString storedPassword;
    bool shouldUpdateEntry = false;

    if (fill->isStored(requestUrl)) {
        const QVector<PasswordEntry> &data = fill->getFormData(requestUrl);
        if (!data.isEmpty()) {
            save->setChecked(true);
            shouldUpdateEntry = true;
            storedUser = data.first().username;
            storedPassword = data.first().password;
            user->setText(storedUser);
            pass->setText(storedPassword);
        }
    }

    // Try to set the originating WebTab as a current tab
    TabbedWebView* tabView = qobject_cast<TabbedWebView*>(view());
    if (tabView) {
        tabView->setAsCurrentTab();
    }

    // Do not save when private browsing is enabled
    if (mApp->isPrivate()) {
        save->setVisible(false);
    }

    if (dialog->exec() != QDialog::Accepted) {
        return;
    }

    auth->setUser(user->text());
    auth->setPassword(pass->text());

    if (save->isChecked()) {
        if (shouldUpdateEntry) {
            if (storedUser != user->text() || storedPassword != pass->text()) {
                fill->updateEntry(requestUrl, user->text(), pass->text());
            }
        }
        else {
            fill->addEntry(requestUrl, user->text(), pass->text());
        }
    }
}

void WebPage::proxyAuthentication(const QUrl &requestUrl, QAuthenticator* auth, const QString &proxyHost)
{
    Q_UNUSED(requestUrl)

    QVector<PasswordEntry> passwords = mApp->autoFill()->getFormData(QUrl(proxyHost));
    if (!passwords.isEmpty()) {
        auth->setUser(passwords.at(0).username);
        auth->setPassword(passwords.at(0).password);
        return;
    }

    QDialog* dialog = new QDialog();
    dialog->setWindowTitle(tr("Proxy authorisation required"));

    QFormLayout* formLa = new QFormLayout(dialog);

    QLabel* label = new QLabel(dialog);
    QLabel* userLab = new QLabel(dialog);
    QLabel* passLab = new QLabel(dialog);
    userLab->setText(tr("Username: "));
    passLab->setText(tr("Password: "));

    QLineEdit* user = new QLineEdit(dialog);
    QLineEdit* pass = new QLineEdit(dialog);
    pass->setEchoMode(QLineEdit::Password);
    QCheckBox* save = new QCheckBox(dialog);
    save->setText(tr("Remember username and password for this proxy."));

    QDialogButtonBox* box = new QDialogButtonBox(dialog);
    box->addButton(QDialogButtonBox::Ok);
    box->addButton(QDialogButtonBox::Cancel);
    connect(box, SIGNAL(rejected()), dialog, SLOT(reject()));
    connect(box, SIGNAL(accepted()), dialog, SLOT(accept()));

    label->setText(tr("A username and password are being requested by proxy %1. ").arg(proxyHost));
    formLa->addRow(label);
    formLa->addRow(userLab, user);
    formLa->addRow(passLab, pass);
    formLa->addRow(save);
    formLa->addWidget(box);

    if (dialog->exec() != QDialog::Accepted) {
        return;
    }

    if (save->isChecked()) {
        mApp->autoFill()->addEntry(QUrl(proxyHost), user->text(), pass->text());
    }

    auth->setUser(user->text());
    auth->setPassword(pass->text());
}

void WebPage::dbQuotaExceeded(QWebEngineFrame* frame)
{
#if QTWEBENGINE_DISABLED
    if (!frame) {
        return;
    }

    const QWebSecurityOrigin origin = frame->securityOrigin();
    const qint64 oldQuota = origin.databaseQuota();

    frame->securityOrigin().setDatabaseQuota(oldQuota * 2);
#endif
}

#ifdef USE_QTWEBKIT_2_2
void WebPage::appCacheQuotaExceeded(QWebSecurityOrigin* origin, quint64 originalQuota)
{
    if (!origin) {
        return;
    }

    origin->setApplicationCacheQuota(originalQuota * 2);
}

void WebPage::featurePermissionRequested(QWebEngineFrame* frame, const QWebEnginePage::Feature &feature)
{
    mApp->html5PermissionsManager()->requestPermissions(this, frame, feature);
}
#endif // USE_QTWEBKIT_2_2

bool WebPage::event(QEvent* event)
{
    if (event->type() == QEvent::Leave) {
        // QWebEnginePagePrivate::leaveEvent():
        // Fake a mouse move event just outside of the widget, since all
        // the interesting mouse-out behavior like invalidating scrollbars
        // is handled by the WebKit event handler's mouseMoved function.

        // However, its implementation fake mouse move event on QCursor::pos()
        // position that is in global screen coordinates. So instead of
        // really faking it, it just creates mouse move event somewhere in
        // page. It can for example focus a link, and then link url gets
        // stuck in status bar message.

        // So we are faking mouse move event with proper coordinates for
        // so called "just outside of the widget" position

        const QPoint cursorPos = view()->mapFromGlobal(QCursor::pos());
        QPoint mousePos;

        if (cursorPos.y() < 0) {
            // Left on top
            mousePos = QPoint(cursorPos.x(), -1);
        }
        else if (cursorPos.x() < 0) {
            // Left on left
            mousePos = QPoint(-1, cursorPos.y());
        }
        else if (cursorPos.y() > view()->height()) {
            // Left on bottom
            mousePos = QPoint(cursorPos.x(), view()->height() + 1);
        }
        else {
            // Left on right
            mousePos = QPoint(view()->width() + 1, cursorPos.y());
        }

        QMouseEvent fakeEvent(QEvent::MouseMove, mousePos, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        return QWebEnginePage::event(&fakeEvent);
    }

    return QWebEnginePage::event(event);
}

void WebPage::setSSLCertificate(const QSslCertificate &cert)
{
    //    if (cert != m_SslCert)
    m_sslCert = cert;
}

QSslCertificate WebPage::sslCertificate()
{
    if (url().scheme() == QLatin1String("https") && QzTools::isCertificateValid(m_sslCert)) {
        return m_sslCert;
    }

    return QSslCertificate();
}

bool WebPage::acceptNavigationRequest(QWebEngineFrame* frame, const QNetworkRequest &request, NavigationType type)
{
    m_lastRequestType = type;
    m_lastRequestUrl = request.url();

    if (type == QWebEnginePage::NavigationTypeFormResubmitted) {
        // Don't show this dialog if app is still starting
        if (!view() || !view()->isVisible()) {
            return false;
        }
        QString message = tr("To show this page, QupZilla must resend request which do it again \n"
                             "(like searching on making an shopping, which has been already done.)");
        bool result = (QMessageBox::question(view(), tr("Confirm form resubmission"),
                                             message, QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes);
        if (!result) {
            return false;
        }
    }

    bool accept = QWebEnginePage::acceptNavigationRequest(frame, request, type);
    return accept;
}

void WebPage::populateNetworkRequest(QNetworkRequest &request)
{
    WebPage* pagePointer = this;

    QVariant variant = QVariant::fromValue((void*) pagePointer);
    request.setAttribute((QNetworkRequest::Attribute)(QNetworkRequest::User + 100), variant);

    if (m_lastRequestUrl == request.url()) {
        request.setAttribute((QNetworkRequest::Attribute)(QNetworkRequest::User + 101), m_lastRequestType);
        if (m_lastRequestType == NavigationTypeLinkClicked) {
            request.setRawHeader("X-QupZilla-UserLoadAction", QByteArray("1"));
        }
    }
}

QWebEnginePage* WebPage::createWindow(QWebEnginePage::WebWindowType type)
{
    if (m_view) {
        return new PopupWebPage(type, m_view->mainWindow());
    }

    if (PopupWebPage* popupPage = qobject_cast<PopupWebPage*>(this)) {
        return new PopupWebPage(type, popupPage->mainWindow());
    }

    return 0;
}

QObject* WebPage::createPlugin(const QString &classid, const QUrl &url,
                               const QStringList &paramNames, const QStringList &paramValues)
{
    Q_UNUSED(url)
    Q_UNUSED(paramNames)
    Q_UNUSED(paramValues)

    if (classid == QLatin1String("RecoveryWidget") && mApp->restoreManager() && m_view) {
        return new RecoveryWidget(m_view, m_view->mainWindow());
    }
    else {
        load(QUrl("qupzilla:start"));
    }

    return 0;
}

void WebPage::addAdBlockRule(const AdBlockRule* rule, const QUrl &url)
{
    AdBlockedEntry entry;
    entry.rule = rule;
    entry.url = url;

    if (!m_adBlockedEntries.contains(entry)) {
        m_adBlockedEntries.append(entry);
    }
}

QVector<WebPage::AdBlockedEntry> WebPage::adBlockedEntries() const
{
    return m_adBlockedEntries;
}

bool WebPage::hasMultipleUsernames() const
{
    return m_passwordEntries.count() > 1;
}

QVector<PasswordEntry> WebPage::autoFillData() const
{
    return m_passwordEntries;
}

void WebPage::cleanBlockedObjects()
{
#if QTWEBENGINE_DISABLED
    AdBlockManager* manager = AdBlockManager::instance();
    if (!manager->isEnabled()) {
        return;
    }

    const QWebElement docElement = mainFrame()->documentElement();

    foreach (const AdBlockedEntry &entry, m_adBlockedEntries) {
        const QString urlString = entry.url.toString();
        if (urlString.endsWith(QLatin1String(".js")) || urlString.endsWith(QLatin1String(".css"))) {
            continue;
        }

        QString urlEnd;

        int pos = urlString.lastIndexOf(QLatin1Char('/'));
        if (pos > 8) {
            urlEnd = urlString.mid(pos + 1);
        }

        if (urlString.endsWith(QLatin1Char('/'))) {
            urlEnd = urlString.left(urlString.size() - 1);
        }

        QString selector("img[src$=\"%1\"], iframe[src$=\"%1\"],embed[src$=\"%1\"]");
        QWebElementCollection elements = docElement.findAll(selector.arg(urlEnd));

        foreach (QWebElement element, elements) {
            QString src = element.attribute("src");
            src.remove(QLatin1String("../"));

            if (urlString.contains(src)) {
                element.setStyleProperty("display", "none");
            }
        }
    }

    // Apply domain-specific element hiding rules
    QString elementHiding = manager->elementHidingRulesForDomain(url());
    if (elementHiding.isEmpty()) {
        return;
    }

    elementHiding.append(QLatin1String("\n</style>"));

    QWebElement bodyElement = docElement.findFirst("body");
    bodyElement.appendInside("<style type=\"text/css\">\n/* AdBlock for QupZilla */\n" + elementHiding);

    // When hiding some elements, scroll position of page will change
    // If user loaded anchor link in background tab (and didn't show it yet), fix the scroll position
    if (view() && !view()->isVisible() && !url().fragment().isEmpty()) {
        mainFrame()->scrollToAnchor(url().fragment());
    }
#endif
}

QString WebPage::userAgentForUrl(const QUrl &url) const
{
    QString userAgent = mApp->userAgentManager()->userAgentForUrl(url);
#if QTWEBENGINE_DISABLED

    if (userAgent.isEmpty()) {
        userAgent = QWebEnginePage::userAgentForUrl(url);
#ifdef Q_OS_MAC
#ifdef __i386__ || __x86_64__
        userAgent.replace(QLatin1String("PPC Mac OS X"), QLatin1String("Intel Mac OS X"));
#endif
#endif
    }

#endif
    return userAgent;
}

bool WebPage::supportsExtension(Extension extension) const
{
    Q_UNUSED(extension)

    return true;
}

bool WebPage::extension(Extension extension, const ExtensionOption* option, ExtensionReturn* output)
{
#if QTWEBENGINE_DISABLED
    if (extension == ChooseMultipleFilesExtension) {
        const QWebEnginePage::ChooseMultipleFilesExtensionOption* exOption = static_cast<const QWebEnginePage::ChooseMultipleFilesExtensionOption*>(option);
        QWebEnginePage::ChooseMultipleFilesExtensionReturn* exReturn = static_cast<QWebEnginePage::ChooseMultipleFilesExtensionReturn*>(output);

        if (!exOption || !exReturn) {
            return QWebEnginePage::extension(extension, option, output);
        }

        QString suggestedFileName;
        if (!exOption->suggestedFileNames.isEmpty()) {
            suggestedFileName = exOption->suggestedFileNames.at(0);
        }

        exReturn->fileNames = QzTools::getOpenFileNames("WebPage-UploadFiles", 0, tr("Select files to upload..."), suggestedFileName);
        return true;
    }

    const ErrorPageExtensionOption* exOption = static_cast<const QWebEnginePage::ErrorPageExtensionOption*>(option);
    ErrorPageExtensionReturn* exReturn = static_cast<QWebEnginePage::ErrorPageExtensionReturn*>(output);

    if (!exOption || !exReturn) {
        return QWebEnginePage::extension(extension, option, output);
    }

    WebPage* erPage = qobject_cast<WebPage*>(exOption->frame->page());

    if (!erPage) {
        return QWebEnginePage::extension(extension, option, output);
    }

    QString errorString;
    if (exOption->domain == QWebEnginePage::QtNetwork) {
        switch (exOption->error) {
        case QNetworkReply::ConnectionRefusedError:
            errorString = tr("Server refused the connection");
            break;
        case QNetworkReply::RemoteHostClosedError:
            errorString = tr("Server closed the connection");
            break;
        case QNetworkReply::HostNotFoundError:
            errorString = tr("Server not found");
            break;
        case QNetworkReply::TimeoutError:
            errorString = tr("Connection timed out");
            break;
        case QNetworkReply::SslHandshakeFailedError:
            errorString = tr("Untrusted connection");
            break;
        case QNetworkReply::TemporaryNetworkFailureError:
            errorString = tr("Temporary network failure");
            break;
        case QNetworkReply::ProxyConnectionRefusedError:
            errorString = tr("Proxy connection refused");
            break;
        case QNetworkReply::ProxyNotFoundError:
            errorString = tr("Proxy server not found");
            break;
        case QNetworkReply::ProxyTimeoutError:
            errorString = tr("Proxy connection timed out");
            break;
        case QNetworkReply::ProxyAuthenticationRequiredError:
            errorString = tr("Proxy authentication required");
            break;
        case QNetworkReply::ContentNotFoundError:
            errorString = tr("Content not found");
            break;
        case QNetworkReply::UnknownNetworkError:
            errorString = exOption->errorString.isEmpty() ? tr("Unknown network error") : exOption->errorString;
            break;
        case QNetworkReply::ProtocolUnknownError: {
            // Sometimes exOption->url returns just "?" instead of actual url
            const QUrl unknownProtocolUrl = (exOption->url.toString() == QLatin1String("?")) ? erPage->mainFrame()->requestedUrl() : exOption->url;
            handleUnknownProtocol(unknownProtocolUrl);
            return false;
        }
        case QNetworkReply::ContentAccessDenied:
            if (exOption->errorString.startsWith(QLatin1String("AdBlock"))) {
                if (exOption->frame != erPage->mainFrame()) { //Content in <iframe>
                    QWebElement docElement = erPage->mainFrame()->documentElement();

                    QWebElementCollection elements;
                    elements.append(docElement.findAll("iframe"));

                    foreach (QWebElement element, elements) {
                        QString src = element.attribute("src");
                        if (exOption->url.toString().contains(src)) {
                            element.setStyleProperty("visibility", "hidden");
                        }
                    }

                    return false;
                }
                else {   //The whole page is blocked
                    QString rule = exOption->errorString;
                    rule.remove(QLatin1String("AdBlock: "));

                    QString errString = QzTools::readAllFileContents(":/html/adblockPage.html");
                    errString.replace(QLatin1String("%TITLE%"), tr("AdBlocked Content"));
                    errString.replace(QLatin1String("%IMAGE%"), QLatin1String("qrc:html/adblock_big.png"));
                    errString.replace(QLatin1String("%FAVICON%"), QLatin1String("qrc:html/adblock_big.png"));

                    errString.replace(QLatin1String("%RULE%"), tr("Blocked by <i>%1</i>").arg(rule));
                    errString = QzTools::applyDirectionToPage(errString);

                    exReturn->baseUrl = exOption->url;
                    exReturn->content = QString(errString + "<span id=\"qupzilla-error-page\"></span>").toUtf8();

                    if (PopupWebPage* popupPage = qobject_cast<PopupWebPage*>(exOption->frame->page())) {
                        WebView* view = qobject_cast<WebView*>(popupPage->view());
                        if (view) {
                            // Closing blocked popup
                            popupPage->mainWindow()->adBlockIcon()->popupBlocked(rule, exOption->url);
                            view->closeView();
                        }
                    }
                    return true;
                }
            }
            errorString = tr("Content Access Denied");
            break;
        default:
            if (exOption->errorString != QLatin1String("QupZilla:No Error")) {
                qDebug() << "Content error: " << exOption->errorString << exOption->error;
            }
            return false;
        }
    }
    else if (exOption->domain == QWebEnginePage::Http) {
        // 200 status code = OK
        // It shouldn't be reported as an error, but sometimes it is ...
        if (exOption->error == 200) {
            return false;
        }
        errorString = tr("Error code %1").arg(exOption->error);
    }
    else if (exOption->domain == QWebEnginePage::WebKit) {
        return false;    // Downloads
    }

    const QUrl loadedUrl = exOption->url;
    exReturn->baseUrl = loadedUrl;

    QFile file(":/html/errorPage.html");
    file.open(QFile::ReadOnly);
    QString errString = file.readAll();
    errString.replace(QLatin1String("%TITLE%"), tr("Failed loading page"));

    errString.replace(QLatin1String("%IMAGE%"), QzTools::pixmapToByteArray(IconProvider::standardIcon(QStyle::SP_MessageBoxWarning).pixmap(45, 45)));
    errString.replace(QLatin1String("%FAVICON%"), QzTools::pixmapToByteArray(IconProvider::standardIcon(QStyle::SP_MessageBoxWarning).pixmap(16, 16)));
    errString.replace(QLatin1String("%BOX-BORDER%"), QLatin1String("qrc:html/box-border.png"));

    QString heading2 = loadedUrl.host().isEmpty() ? tr("QupZilla can't load page.") : tr("QupZilla can't load page from %1.").arg(loadedUrl.host());

    errString.replace(QLatin1String("%HEADING%"), errorString);
    errString.replace(QLatin1String("%HEADING2%"), heading2);
    errString.replace(QLatin1String("%LI-1%"), tr("Check the address for typing errors such as <b>ww.</b>example.com instead of <b>www.</b>example.com"));
    errString.replace(QLatin1String("%LI-2%"), tr("If you are unable to load any pages, check your computer's network connection."));
    errString.replace(QLatin1String("%LI-3%"), tr("If your computer or network is protected by a firewall or proxy, make sure that QupZilla is permitted to access the Web."));
    errString.replace(QLatin1String("%TRY-AGAIN%"), tr("Try Again"));
    errString = QzTools::applyDirectionToPage(errString);

    exReturn->content = QString(errString + "<span id=\"qupzilla-error-page\"></span>").toUtf8();
#endif
    return true;
}

bool WebPage::javaScriptPrompt(QWebEngineFrame* originatingFrame, const QString &msg, const QString &defaultValue, QString* result)
{
#ifndef NONBLOCK_JS_DIALOGS
    return QWebEnginePage::javaScriptPrompt(originatingFrame, msg, defaultValue, result);
#else
    if (m_runningLoop) {
        return false;
    }

    WebView* webView = qobject_cast<WebView*>(originatingFrame->page()->view());
    ResizableFrame* widget = new ResizableFrame(webView->overlayForJsAlert());

    widget->setObjectName("jsFrame");
    Ui_jsPrompt* ui = new Ui_jsPrompt();
    ui->setupUi(widget);
    ui->message->setText(msg);
    ui->lineEdit->setText(defaultValue);
    ui->lineEdit->setFocus();
    widget->resize(originatingFrame->page()->viewportSize());
    widget->show();

    connect(webView, SIGNAL(viewportResized(QSize)), widget, SLOT(slotResize(QSize)));
    connect(ui->lineEdit, SIGNAL(returnPressed()), ui->buttonBox->button(QDialogButtonBox::Ok), SLOT(animateClick()));

    QEventLoop eLoop;
    m_runningLoop = &eLoop;
    connect(ui->buttonBox, SIGNAL(clicked(QAbstractButton*)), &eLoop, SLOT(quit()));

    if (eLoop.exec() == 1) {
        return result;
    }
    m_runningLoop = 0;

    QString x = ui->lineEdit->text();
    bool _result = ui->buttonBox->clickedButtonRole() == QDialogButtonBox::AcceptRole;
    *result = x;

    delete widget;
    webView->setFocus();

    return _result;
#endif
}

bool WebPage::javaScriptConfirm(QWebEngineFrame* originatingFrame, const QString &msg)
{
#ifndef NONBLOCK_JS_DIALOGS
    return QWebEnginePage::javaScriptConfirm(originatingFrame, msg);
#else
    if (m_runningLoop) {
        return false;
    }

    WebView* webView = qobject_cast<WebView*>(originatingFrame->page()->view());
    ResizableFrame* widget = new ResizableFrame(webView->overlayForJsAlert());

    widget->setObjectName("jsFrame");
    Ui_jsConfirm* ui = new Ui_jsConfirm();
    ui->setupUi(widget);
    ui->message->setText(msg);
    ui->buttonBox->button(QDialogButtonBox::Ok)->setFocus();
    widget->resize(originatingFrame->page()->viewportSize());
    widget->show();

    connect(webView, SIGNAL(viewportResized(QSize)), widget, SLOT(slotResize(QSize)));

    QEventLoop eLoop;
    m_runningLoop = &eLoop;
    connect(ui->buttonBox, SIGNAL(clicked(QAbstractButton*)), &eLoop, SLOT(quit()));

    if (eLoop.exec() == 1) {
        return false;
    }
    m_runningLoop = 0;

    bool result = ui->buttonBox->clickedButtonRole() == QDialogButtonBox::AcceptRole;

    delete widget;
    webView->setFocus();

    return result;
#endif
}

void WebPage::javaScriptAlert(QWebEngineFrame* originatingFrame, const QString &msg)
{
    Q_UNUSED(originatingFrame)

    if (m_blockAlerts || m_runningLoop) {
        return;
    }

#ifndef NONBLOCK_JS_DIALOGS
    QString title = tr("JavaScript alert");
    if (!url().host().isEmpty()) {
        title.append(QString(" - %1").arg(url().host()));
    }

    CheckBoxDialog dialog(QDialogButtonBox::Ok, view());
    dialog.setWindowTitle(title);
    dialog.setText(msg);
    dialog.setCheckBoxText(tr("Prevent this page from creating additional dialogs"));
    dialog.setIcon(IconProvider::standardIcon(QStyle::SP_MessageBoxInformation));
    dialog.exec();

    m_blockAlerts = dialog.isChecked();

#else
    WebView* webView = qobject_cast<WebView*>(originatingFrame->page()->view());
    ResizableFrame* widget = new ResizableFrame(webView->overlayForJsAlert());

    widget->setObjectName("jsFrame");
    Ui_jsAlert* ui = new Ui_jsAlert();
    ui->setupUi(widget);
    ui->message->setText(msg);
    ui->buttonBox->button(QDialogButtonBox::Ok)->setFocus();
    widget->resize(originatingFrame->page()->viewportSize());
    widget->show();

    connect(webView, SIGNAL(viewportResized(QSize)), widget, SLOT(slotResize(QSize)));

    QEventLoop eLoop;
    m_runningLoop = &eLoop;
    connect(ui->buttonBox, SIGNAL(clicked(QAbstractButton*)), &eLoop, SLOT(quit()));

    if (eLoop.exec() == 1) {
        return;
    }
    m_runningLoop = 0;

    m_blockAlerts = ui->preventAlerts->isChecked();

    delete widget;

    webView->setFocus();
#endif
}

void WebPage::setJavaScriptEnabled(bool enabled)
{
#if QTWEBENGINE_DISABLED
    settings()->setAttribute(QWebEngineSettings::JavascriptEnabled, enabled);
    m_javaScriptEnabled = enabled;
#endif
}

QString WebPage::chooseFile(QWebEngineFrame* originatingFrame, const QString &oldFile)
{
    QString suggFileName;

    if (oldFile.isEmpty()) {
        suggFileName = s_lastUploadLocation;
    }
    else {
        suggFileName = oldFile;
    }

    const QString fileName = QzTools::getOpenFileName("WebPage-ChooseFile", view(), tr("Choose file..."), suggFileName);

    if (!fileName.isEmpty()) {
        s_lastUploadLocation = fileName;

        // Check if we can read from file
        QFile file(fileName);
        if (!file.open(QFile::ReadOnly)) {
            const QString msg = tr("Cannot read data from <b>%1</b>. Upload was cancelled!").arg(fileName);
            QMessageBox::critical(view(), tr("Cannot read file!"), msg);
            return QString();
        }
    }

    return fileName;
}

bool WebPage::isPointerSafeToUse(WebPage* page)
{
    // Pointer to WebPage is passed with every QNetworkRequest casted to void*
    // So there is no way to test whether pointer is still valid or not, except
    // this hack.

    return page == 0 ? false : s_livingPages.contains(page);
}
