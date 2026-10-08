#include "ui/MainWindow.h"

#include "ui/WebTab.h"

#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QProgressBar>
#include <QShortcut>
#include <QStatusBar>
#include <QTabWidget>
#include <QToolBar>
#include <QWebEngineHistory>
#include <QWebEngineProfile>
#include <QWebEngineView>

namespace oqb::ui {

MainWindow::MainWindow(const browser::PageSettings &settings, QWidget *parent)
    : QMainWindow(parent)
    , m_settings(settings)
{
    setWindowTitle(QStringLiteral("OpenQBrowser"));
    // The icon is compiled into the binary as a Qt resource, so it does not have
    // to be found on disk at run time.
    setWindowIcon(QIcon(QStringLiteral(":/assets/icon.svg")));
    resize(static_cast<int>(settings.viewportWidth + 20),
           static_cast<int>(settings.viewportHeight + 120));

    // Every tab shares one profile, so cookies, cache and local storage are
    // common to the window and survive restarts. The default profile is
    // persistent and owned by Qt.
    m_profile = QWebEngineProfile::defaultProfile();
    if (!settings.userAgent.isEmpty())
        m_profile->setHttpUserAgent(settings.userAgent);

    // ------------------------------------------------------------- toolbar
    auto *toolbar = addToolBar(QStringLiteral("Navigation"));
    toolbar->setMovable(false);

    m_backAction = toolbar->addAction(QStringLiteral("◀"));
    m_backAction->setToolTip(QStringLiteral("Back (Alt+Left)"));
    connect(m_backAction, &QAction::triggered, this, &MainWindow::onBack);

    m_forwardAction = toolbar->addAction(QStringLiteral("▶"));
    m_forwardAction->setToolTip(QStringLiteral("Forward (Alt+Right)"));
    connect(m_forwardAction, &QAction::triggered, this, &MainWindow::onForward);

    m_reloadAction = toolbar->addAction(QStringLiteral("⟳"));
    m_reloadAction->setToolTip(QStringLiteral("Reload (F5)"));
    connect(m_reloadAction, &QAction::triggered, this, &MainWindow::onReload);

    m_stopAction = toolbar->addAction(QStringLiteral("✕"));
    m_stopAction->setToolTip(QStringLiteral("Stop"));
    connect(m_stopAction, &QAction::triggered, this, &MainWindow::onStop);
    m_stopAction->setEnabled(false);

    m_addressBar = new QLineEdit;
    m_addressBar->setPlaceholderText(QStringLiteral("Search or enter an address"));
    m_addressBar->setClearButtonEnabled(true);
    connect(m_addressBar, &QLineEdit::returnPressed, this, &MainWindow::onAddressEntered);
    toolbar->addWidget(m_addressBar);

    m_bookmarkAction = toolbar->addAction(QStringLiteral("☆"));
    m_bookmarkAction->setToolTip(QStringLiteral("Bookmark this page"));
    connect(m_bookmarkAction, &QAction::triggered, this, &MainWindow::onToggleBookmark);

    m_devToolsAction = toolbar->addAction(QStringLiteral("DevTools"));
    m_devToolsAction->setToolTip(QStringLiteral("Show the developer tools (F12)"));
    m_devToolsAction->setCheckable(true);
    m_devToolsAction->setShortcut(QKeySequence(Qt::Key_F12));
    connect(m_devToolsAction, &QAction::toggled, this, &MainWindow::onToggleDevTools);

    // ------------------------------------------------------------- devtools
    //
    // The inspector is the Chromium DevTools page, docked so it can be resized
    // or floated like a real browser's. It starts hidden.
    m_devToolsView = new QWebEngineView(this);
    m_devToolsDock = new QDockWidget(QStringLiteral("Developer Tools"), this);
    m_devToolsDock->setObjectName(QStringLiteral("devToolsDock"));
    m_devToolsDock->setWidget(m_devToolsView);
    m_devToolsDock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::RightDockWidgetArea);
    addDockWidget(Qt::BottomDockWidgetArea, m_devToolsDock);
    m_devToolsDock->hide();

    // Closing the dock with its own close box unchecks the toolbar button, so the
    // two ways of controlling it cannot disagree.
    connect(m_devToolsDock, &QDockWidget::visibilityChanged, this, [this](bool visible) {
        if (m_devToolsAction->isChecked() != visible)
            m_devToolsAction->setChecked(visible);
    });

    // ---------------------------------------------------------------- menus
    QMenu *fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
    fileMenu->addAction(QStringLiteral("&New Tab"), QKeySequence::AddTab, this,
                        [this] { openInNewTab(network::Url::parse(QStringLiteral("about:home"))); });
    fileMenu->addAction(QStringLiteral("&Close Tab"), QKeySequence::Close, this,
                        [this] { onTabClosed(m_tabs->currentIndex()); });
    fileMenu->addSeparator();
    fileMenu->addAction(QStringLiteral("&Quit"), QKeySequence::Quit, qApp, &QApplication::quit);

    QMenu *viewMenu = menuBar()->addMenu(QStringLiteral("&View"));
    viewMenu->addAction(m_backAction);
    viewMenu->addAction(m_forwardAction);
    viewMenu->addAction(m_reloadAction);
    viewMenu->addSeparator();
    viewMenu->addAction(m_devToolsAction);

    QMenu *pagesMenu = menuBar()->addMenu(QStringLiteral("&Pages"));
    pagesMenu->addAction(QStringLiteral("New Tab Page"), this,
                         [this] { openUrl(network::Url::parse(QStringLiteral("about:home"))); });
    pagesMenu->addAction(QStringLiteral("History"), this,
                         [this] { openUrl(network::Url::parse(QStringLiteral("about:history"))); });
    pagesMenu->addAction(QStringLiteral("Bookmarks"), this,
                         [this] { openUrl(network::Url::parse(QStringLiteral("about:bookmarks"))); });
    pagesMenu->addAction(QStringLiteral("Version"), this,
                         [this] { openUrl(network::Url::parse(QStringLiteral("about:version"))); });

    // ------------------------------------------------------------ status bar
    m_statusLabel = new QLabel;
    statusBar()->addWidget(m_statusLabel, 1);

    m_progress = new QProgressBar;
    m_progress->setMaximumWidth(140);
    m_progress->setRange(0, 100);
    m_progress->setVisible(false);
    statusBar()->addPermanentWidget(m_progress);

    // ------------------------------------------------------------------ tabs
    m_tabs = new QTabWidget;
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);
    connect(m_tabs, &QTabWidget::currentChanged, this, &MainWindow::onTabChanged);
    connect(m_tabs, &QTabWidget::tabCloseRequested, this, &MainWindow::onTabClosed);

    setCentralWidget(m_tabs);

    // ------------------------------------------------------------- shortcuts
    auto *backShortcut = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_Left), this);
    connect(backShortcut, &QShortcut::activated, this, &MainWindow::onBack);

    auto *forwardShortcut = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_Right), this);
    connect(forwardShortcut, &QShortcut::activated, this, &MainWindow::onForward);

    auto *reloadShortcut = new QShortcut(QKeySequence::Refresh, this);
    connect(reloadShortcut, &QShortcut::activated, this, &MainWindow::onReload);

    auto *focusShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+L")), this);
    connect(focusShortcut, &QShortcut::activated, this, [this] {
        m_addressBar->setFocus();
        m_addressBar->selectAll();
    });

    // The first tab shows the new tab page, which is what a browser does.
    openInNewTab(network::Url::parse(QStringLiteral("about:home")));
}

MainWindow::~MainWindow() = default;

int MainWindow::tabCount() const
{
    return m_tabs ? m_tabs->count() : 0;
}

WebTab *MainWindow::addTab()
{
    auto *tab = new WebTab(m_profile, this);
    tab->setHistory(&m_history);
    tab->setBookmarks(&m_bookmarks);
    tab->setSearchTemplate(m_settings.searchTemplate);

    connect(tab, &WebTab::titleChanged, this, [this, tab](const QString &title) {
        updateTitle();
        // Keep the tab's own label in step with its document title.
        for (int i = 0; i < m_tabs->count(); ++i) {
            if (qobject_cast<WebTab *>(m_tabs->widget(i)) == tab)
                m_tabs->setTabText(i, title.left(24));
        }
    });

    connect(tab, &WebTab::urlChanged, this, [this](const network::Url &) {
        updateNavigationState();
    });
    connect(tab, &WebTab::loadStarted, this, [this] { updateNavigationState(); });
    connect(tab, &WebTab::loadFinished, this, [this](bool) { updateNavigationState(); });
    connect(tab, &WebTab::loadProgress, this, [this](int progress) {
        if (qobject_cast<WebTab *>(m_tabs->currentWidget()) == currentTab()) {
            m_progress->setValue(progress);
            m_progress->setVisible(progress > 0 && progress < 100);
        }
    });
    connect(tab, &WebTab::linkHovered, this, [this](const QString &url) {
        m_statusLabel->setText(url);
    });
    connect(tab, &WebTab::newViewRequested, this, &MainWindow::onNewViewRequested);

    m_tabs->addTab(tab, QStringLiteral("New Tab"));
    syncTabs();
    return tab;
}

WebTab *MainWindow::currentTab() const
{
    return qobject_cast<WebTab *>(m_tabs->currentWidget());
}

void MainWindow::syncTabs()
{
    const int count = m_tabs->count();
    for (int i = 0; i < count; ++i) {
        if (auto *tab = qobject_cast<WebTab *>(m_tabs->widget(i)))
            tab->setTabCount(count);
    }
}

void MainWindow::openUrl(const network::Url &url)
{
    WebTab *tab = currentTab();
    if (!tab) {
        tab = addTab();
        if (!tab)
            return;
    }
    tab->navigate(url);
}

void MainWindow::openInNewTab(const network::Url &url)
{
    WebTab *tab = addTab();
    if (!tab)
        return;
    m_tabs->setCurrentIndex(m_tabs->count() - 1);
    tab->navigate(url);
}

void MainWindow::onToggleDevTools(bool show)
{
    if (!m_devToolsDock || !m_devToolsView)
        return;

    m_devToolsDock->setVisible(show);

    if (show) {
        if (WebTab *tab = currentTab())
            tab->attachDevTools(m_devToolsView->page());
    } else if (WebTab *tab = currentTab()) {
        tab->detachDevTools();
    }
}

void MainWindow::onNewViewRequested(QWebEngineView *view)
{
    // A target=_blank link or window.open() arrives here: the page asked for a
    // new view, which becomes a new tab rather than a separate window.
    auto *tab = new WebTab(view, m_profile, this);
    tab->setHistory(&m_history);
    tab->setBookmarks(&m_bookmarks);
    tab->setSearchTemplate(m_settings.searchTemplate);

    connect(tab, &WebTab::titleChanged, this, [this, tab](const QString &title) {
        updateTitle();
        for (int i = 0; i < m_tabs->count(); ++i) {
            if (qobject_cast<WebTab *>(m_tabs->widget(i)) == tab)
                m_tabs->setTabText(i, title.left(24));
        }
    });
    connect(tab, &WebTab::urlChanged, this, [this](const network::Url &) { updateNavigationState(); });
    connect(tab, &WebTab::loadStarted, this, [this] { updateNavigationState(); });
    connect(tab, &WebTab::loadFinished, this, [this](bool) { updateNavigationState(); });
    connect(tab, &WebTab::linkHovered, this, [this](const QString &url) {
        m_statusLabel->setText(url);
    });
    connect(tab, &WebTab::newViewRequested, this, &MainWindow::onNewViewRequested);

    m_tabs->addTab(tab, QStringLiteral("New Tab"));
    m_tabs->setCurrentIndex(m_tabs->count() - 1);
    syncTabs();
}

void MainWindow::onTabChanged(int index)
{
    Q_UNUSED(index);

    updateTitle();
    updateNavigationState();

    // The inspector follows the tab, so switching tabs shows the new page's
    // state rather than the previous one's.
    if (m_devToolsDock && m_devToolsDock->isVisible()) {
        if (WebTab *tab = currentTab())
            tab->attachDevTools(m_devToolsView->page());
    }
}

void MainWindow::onTabClosed(int index)
{
    if (index < 0 || index >= m_tabs->count())
        return;

    QWidget *widget = m_tabs->widget(index);
    m_tabs->removeTab(index);
    delete widget;

    // Closing the last tab opens a fresh one, so the window never sits empty.
    if (m_tabs->count() == 0) {
        openInNewTab(network::Url::parse(QStringLiteral("about:home")));
        return;
    }

    syncTabs();
    updateNavigationState();
}

void MainWindow::onAddressEntered()
{
    WebTab *tab = currentTab();
    if (!tab) {
        openUrl(network::Url::fromUserInput(m_addressBar->text()));
        return;
    }
    tab->navigateToUserInput(m_addressBar->text());
}

void MainWindow::onBack()
{
    if (WebTab *tab = currentTab())
        tab->back();
}

void MainWindow::onForward()
{
    if (WebTab *tab = currentTab())
        tab->forward();
}

void MainWindow::onReload()
{
    if (WebTab *tab = currentTab())
        tab->reload();
}

void MainWindow::onStop()
{
    if (WebTab *tab = currentTab())
        tab->stop();
}

void MainWindow::onToggleBookmark()
{
    WebTab *tab = currentTab();
    if (!tab)
        return;

    const bool added = m_bookmarks.toggle(tab->url(), tab->title());
    m_bookmarkAction->setText(added ? QStringLiteral("★") : QStringLiteral("☆"));
    m_statusLabel->setText(added ? QStringLiteral("Bookmarked") : QStringLiteral("Bookmark removed"));
}

void MainWindow::updateNavigationState()
{
    WebTab *tab = currentTab();

    if (!tab) {
        m_backAction->setEnabled(false);
        m_forwardAction->setEnabled(false);
        m_stopAction->setEnabled(false);
        m_addressBar->clear();
        return;
    }

    m_backAction->setEnabled(tab->view()->history()->canGoBack());
    m_forwardAction->setEnabled(tab->view()->history()->canGoForward());
    m_stopAction->setEnabled(tab->isLoading());
    m_reloadAction->setEnabled(!tab->isLoading());

    // The address bar is only rewritten when it does not have focus, so typing
    // is never interrupted by a load finishing.
    if (!m_addressBar->hasFocus())
        m_addressBar->setText(tab->url().toString());

    m_bookmarkAction->setText(m_bookmarks.contains(tab->url()) ? QStringLiteral("★")
                                                               : QStringLiteral("☆"));

    if (tab->isLoading()) {
        m_progress->setVisible(true);
        m_statusLabel->setText(QStringLiteral("Loading %1…").arg(tab->url().displayHost()));
    } else {
        m_progress->setVisible(false);
        if (m_statusLabel->text().startsWith(QLatin1String("Loading")))
            m_statusLabel->clear();
    }

    updateTitle();
}

void MainWindow::updateTitle()
{
    WebTab *tab = currentTab();
    const QString title = tab ? tab->title() : QString();

    setWindowTitle(title.isEmpty() ? QStringLiteral("OpenQBrowser")
                                   : title + QStringLiteral(" — OpenQBrowser"));
}

} // namespace oqb::ui
