#include "ui/MainWindow.h"

#include "browser/Tab.h"
#include "ui/PageView.h"

#include <QAction>
#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QProgressBar>
#include <QShortcut>
#include <QStatusBar>
#include <QTabWidget>
#include <QToolBar>
#include <QVBoxLayout>
#include <QIcon>

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

    m_boxModelAction = toolbar->addAction(QStringLiteral("Boxes"));
    m_boxModelAction->setToolTip(QStringLiteral("Show the box model overlay"));
    m_boxModelAction->setCheckable(true);
    connect(m_boxModelAction, &QAction::toggled, this, &MainWindow::onShowBoxModel);

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
    viewMenu->addAction(QStringLiteral("Box model overlay"), this,
                        [this] { m_boxModelAction->toggle(); });

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
    m_progress->setRange(0, 0); // busy indicator
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

browser::Tab *MainWindow::addTab()
{
    const int index = m_tabs->count();

    auto *tab = new browser::Tab(m_settings, this);
    tab->setHistory(&m_history);
    tab->setBookmarks(&m_bookmarks);
    tab->setTabCount(index + 1);

    auto *view = new PageView;
    view->setTab(tab);

    connect(tab, &browser::Tab::titleChanged, this, [this, tab](const QString &title) {
        updateTitle();
        // Keep the tab's own label in step with its document title.
        for (int i = 0; i < m_tabs->count(); ++i) {
            const auto *pageView = qobject_cast<PageView *>(m_tabs->widget(i));
            if (pageView && pageView->tab() == tab)
                m_tabs->setTabText(i, title.left(24));
        }
    });

    connect(tab, &browser::Tab::stateChanged, this, &MainWindow::updateNavigationState);
    connect(tab, &browser::Tab::loadFailed, this, [this](const QString &message) {
        m_statusLabel->setText(message);
    });
    connect(tab, &browser::Tab::navigationRequested, this,
            [this](const network::Url &url, bool newTab) {
                if (newTab)
                    openInNewTab(url);
                else
                    openUrl(url);
            });

    connect(view, &PageView::linkActivated, this,
            [this](const network::Url &url, bool newTab) {
                if (newTab)
                    openInNewTab(url);
                else
                    openUrl(url);
            });
    connect(view, &PageView::linkHovered, this, [this](const QString &url) {
        m_statusLabel->setText(url);
    });
    connect(view, &PageView::documentSizeChanged, this, [this] { updateNavigationState(); });
    connect(view, &PageView::reloadRequested, this, &MainWindow::onReload);

    m_tabs->addTab(view, QStringLiteral("New Tab"));
    syncTabs();
    return tab;
}

browser::Tab *MainWindow::currentTab() const
{
    PageView *view = currentView();
    return view ? view->tab() : nullptr;
}

PageView *MainWindow::currentView() const
{
    return qobject_cast<PageView *>(m_tabs->currentWidget());
}

void MainWindow::syncTabs()
{
    // The tab count appears on the new tab page, so every tab is told when it
    // changes.
    const int count = m_tabs->count();
    for (int i = 0; i < count; ++i) {
        if (auto *view = qobject_cast<PageView *>(m_tabs->widget(i))) {
            if (view->tab())
                view->tab()->setTabCount(count);
        }
    }
}

void MainWindow::openUrl(const network::Url &url)
{
    browser::Tab *tab = currentTab();
    if (!tab) {
        tab = addTab();
        if (!tab)
            return;
    }
    tab->navigate(url);
}

void MainWindow::openInNewTab(const network::Url &url)
{
    browser::Tab *tab = addTab();
    if (!tab)
        return;
    m_tabs->setCurrentIndex(m_tabs->count() - 1);
    tab->navigate(url);
}

void MainWindow::onTabChanged(int index)
{
    Q_UNUSED(index);

    updateTitle();
    updateNavigationState();

    if (PageView *view = currentView())
        view->refresh();
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
    browser::Tab *tab = currentTab();
    if (!tab) {
        openUrl(network::Url::fromUserInput(m_addressBar->text()));
        return;
    }
    tab->navigateToUserInput(m_addressBar->text());
}

void MainWindow::onBack()
{
    if (browser::Tab *tab = currentTab())
        tab->goBack();
}

void MainWindow::onForward()
{
    if (browser::Tab *tab = currentTab())
        tab->goForward();
}

void MainWindow::onReload()
{
    if (browser::Tab *tab = currentTab())
        tab->reload();
}

void MainWindow::onStop()
{
    if (browser::Tab *tab = currentTab())
        tab->stop();
}

void MainWindow::onToggleBookmark()
{
    browser::Tab *tab = currentTab();
    if (!tab)
        return;

    const bool added = m_bookmarks.toggle(tab->url(), tab->title());
    m_bookmarkAction->setText(added ? QStringLiteral("★") : QStringLiteral("☆"));
    m_statusLabel->setText(added ? QStringLiteral("Bookmarked") : QStringLiteral("Bookmark removed"));
}

void MainWindow::onShowBoxModel(bool show)
{
    if (PageView *view = currentView())
        view->setShowBoxModel(show);
}

void MainWindow::updateNavigationState()
{
    browser::Tab *tab = currentTab();

    if (!tab) {
        m_backAction->setEnabled(false);
        m_forwardAction->setEnabled(false);
        m_stopAction->setEnabled(false);
        m_addressBar->clear();
        return;
    }

    m_backAction->setEnabled(tab->canGoBack());
    m_forwardAction->setEnabled(tab->canGoForward());
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
    browser::Tab *tab = currentTab();
    const QString title = tab ? tab->title() : QString();

    setWindowTitle(title.isEmpty() ? QStringLiteral("OpenQBrowser")
                                   : title + QStringLiteral(" — OpenQBrowser"));
}

} // namespace oqb::ui
