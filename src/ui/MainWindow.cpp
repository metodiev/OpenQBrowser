#include "ui/MainWindow.h"

#include "browser/BuiltinPages.h"

#include "ui/BuiltinScheme.h"
#include "ui/TabBar.h"
#include "ui/Theme.h"
#include "ui/WebTab.h"

#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QProgressBar>
#include <QShortcut>
#include <QStackedWidget>
#include <QStatusBar>
#include <QToolButton>
#include <QStyleHints>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWebEngineHistory>
#include <QWebEngineProfile>
#include <QWebEngineView>

namespace oqb::ui {

namespace {

/// The size every toolbar glyph is drawn at, so the icons stay in step with each
/// other and with the buttons that hold them. A mature browser's toolbar glyphs
/// sit around 16px in a 28px target, which is the proportion used here: larger
/// than this and the chrome starts to look like a toolbar of stickers.
constexpr int kIconSize = 16;

/// The square the toolbar buttons occupy. Large enough to be a comfortable
/// target at 16px of artwork, without making the toolbar feel heavy.
constexpr int kButtonSize = 28;

/// A flat icon button for the toolbar and the tab strip. The theme styles it;
/// this only sets what the stylesheet cannot express.
QToolButton *makeIconButton(NavIcon icon, const Theme &theme, const QString &toolTip,
                            int size = kButtonSize)
{
    auto *button = new QToolButton;
    button->setIcon(theme.icon(icon, theme.iconColor, theme.iconColorDisabled, kIconSize));
    button->setIconSize(QSize(kIconSize, kIconSize));
    button->setFixedSize(size, size);
    button->setToolTip(toolTip);
    button->setCursor(Qt::ArrowCursor);
    button->setFocusPolicy(Qt::NoFocus);
    button->setAutoRaise(true);
    return button;
}

} // namespace

MainWindow::MainWindow(const browser::PageSettings &settings, QWidget *parent)
    : QMainWindow(parent)
    , m_settings(settings)
    , m_theme(Theme::forCurrentColorScheme())
{
    setWindowTitle(QStringLiteral("OpenQBrowser"));
    setWindowIcon(QIcon(QStringLiteral(":/assets/icon.svg")));
    resize(static_cast<int>(settings.viewportWidth + 40),
           static_cast<int>(settings.viewportHeight + 130));

    // Every tab shares one profile, so cookies, cache and local storage are
    // common to the window and survive restarts.
    m_profile = QWebEngineProfile::defaultProfile();
    if (!settings.userAgent.isEmpty())
        m_profile->setHttpUserAgent(settings.userAgent);

    // The built-in pages are served over the browser's own scheme, so that
    // navigating to one is a real navigation with a history entry: that is what
    // makes Back work after leaving the start page.
    installBuiltinSchemeHandler(m_profile, [this](const QString &pageName) {
        return browser::builtin::documentFor(network::Url::parse(QStringLiteral("about:")
                                                                 + pageName),
                                             QStringLiteral(OPENQBROWSER_VERSION),
                                             m_settings.viewportWidth, &m_history, &m_bookmarks,
                                             tabCount());
    });

    // ------------------------------------------------------------ structure
    //
    // The chrome is assembled by hand rather than with QMainWindow's toolbar
    // area, because a browser puts the tabs above the toolbar and the progress
    // line directly under it - a sibling order QMainWindow cannot express.
    auto *central = new QWidget;
    auto *centralLayout = new QVBoxLayout(central);
    centralLayout->setContentsMargins(0, 0, 0, 0);
    centralLayout->setSpacing(0);

    auto *chrome = new QWidget;
    chrome->setObjectName(QStringLiteral("BrowserChrome"));
    chrome->setAutoFillBackground(false);
    auto *chromeLayout = new QVBoxLayout(chrome);
    chromeLayout->setContentsMargins(0, 0, 0, 0);
    chromeLayout->setSpacing(0);

    // ------------------------------------------------------------- tab strip
    auto *tabRow = new QWidget;
    tabRow->setObjectName(QStringLiteral("TabRow"));
    auto *tabLayout = new QHBoxLayout(tabRow);
    tabLayout->setContentsMargins(8, 7, 8, 0);
    tabLayout->setSpacing(4);

    m_tabBar = new TabBar;
    m_tabBar->setTheme(m_theme);
    tabLayout->addWidget(m_tabBar, 1);

    m_newTabButton = makeIconButton(NavIcon::NewTab, m_theme, tr("New tab (Ctrl+T)"), 28);
    m_newTabButton->setObjectName(QStringLiteral("NewTabButton"));
    tabLayout->addWidget(m_newTabButton, 0, Qt::AlignVCenter);

    chromeLayout->addWidget(tabRow);

    // --------------------------------------------------------------- toolbar
    auto *toolRow = new QWidget;
    toolRow->setObjectName(QStringLiteral("ToolRow"));
    auto *toolLayout = new QHBoxLayout(toolRow);
    toolLayout->setContentsMargins(8, 6, 8, 8);
    toolLayout->setSpacing(4);

    m_backButton = makeIconButton(NavIcon::Back, m_theme, tr("Back (Alt+Left)"));
    m_backButton->setObjectName(QStringLiteral("BackButton"));
    m_forwardButton = makeIconButton(NavIcon::Forward, m_theme, tr("Forward (Alt+Right)"));
    m_forwardButton->setObjectName(QStringLiteral("ForwardButton"));
    m_reloadButton = makeIconButton(NavIcon::Reload, m_theme, tr("Reload (F5)"));
    m_reloadButton->setObjectName(QStringLiteral("ReloadButton"));
    m_homeButton = makeIconButton(NavIcon::Home, m_theme, tr("Home"));
    m_homeButton->setObjectName(QStringLiteral("HomeButton"));

    toolLayout->addWidget(m_backButton);
    toolLayout->addWidget(m_forwardButton);
    toolLayout->addWidget(m_reloadButton);
    toolLayout->addWidget(m_homeButton);
    toolLayout->addSpacing(4);

    m_addressBar = new QLineEdit;
    m_addressBar->setObjectName(QStringLiteral("AddressBar"));
    m_addressBar->setPlaceholderText(tr("Search or enter an address"));
    m_addressBar->setClearButtonEnabled(true);
    m_addressBar->setMinimumHeight(32);
    toolLayout->addWidget(m_addressBar, 1);

    m_bookmarkButton = makeIconButton(NavIcon::Star, m_theme, tr("Bookmark this page"));
    m_bookmarkButton->setObjectName(QStringLiteral("BookmarkButton"));
    m_menuButton = makeIconButton(NavIcon::Menu, m_theme, tr("Menu"));
    m_menuButton->setObjectName(QStringLiteral("MenuButton"));
    toolLayout->addWidget(m_bookmarkButton);
    toolLayout->addWidget(m_menuButton);

    chromeLayout->addWidget(toolRow);

    centralLayout->addWidget(chrome);

    // The progress line sits between the chrome and the page, where Chrome puts
    // it: the page below never moves, and the bar is visible without looking at
    // the window's edge.
    m_progress = new QProgressBar;
    m_progress->setObjectName(QStringLiteral("LoadProgress"));
    m_progress->setRange(0, 100);
    m_progress->setTextVisible(false);
    m_progress->setFixedHeight(3);
    centralLayout->addWidget(m_progress);

    m_stack = new QStackedWidget;
    centralLayout->addWidget(m_stack, 1);

    setCentralWidget(central);

    // ------------------------------------------------------------- status bar
    m_statusLabel = new QLabel;
    statusBar()->addWidget(m_statusLabel, 1);

    // ----------------------------------------------------------- devtools
    m_devToolsView = new QWebEngineView(this);
    m_devToolsDock = new QDockWidget(tr("Developer Tools"), this);
    m_devToolsDock->setObjectName(QStringLiteral("devToolsDock"));
    m_devToolsDock->setWidget(m_devToolsView);
    m_devToolsDock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::RightDockWidgetArea);
    addDockWidget(Qt::BottomDockWidgetArea, m_devToolsDock);
    m_devToolsDock->hide();
    connect(m_devToolsDock, &QDockWidget::visibilityChanged, this, [this](bool visible) {
        if (m_devToolsAction && m_devToolsAction->isChecked() != visible)
            m_devToolsAction->setChecked(visible);
    });

    // ------------------------------------------------------------- behaviour
    connect(m_tabBar, &QTabBar::currentChanged, this, &MainWindow::onTabChanged);
    connect(m_tabBar, &TabBar::closeRequested, this, &MainWindow::onTabCloseRequested);
    connect(m_tabBar, &TabBar::newTabRequested, this, &MainWindow::openNewTab);
    connect(m_newTabButton, &QToolButton::clicked, this, &MainWindow::openNewTab);

    connect(m_addressBar, &QLineEdit::returnPressed, this, &MainWindow::onAddressEntered);
    // textEdited fires only for changes the user makes, so it is exactly the
    // "the user is typing" signal the address-bar update needs. Editing is over
    // on Enter or when the field loses focus, after which the address follows
    // the page again.
    connect(m_addressBar, &QLineEdit::textEdited, this, [this] {
        m_addressBarEdited = true;
    });
    connect(m_addressBar, &QLineEdit::editingFinished, this, [this] {
        m_addressBarEdited = false;
    });
    connect(m_backButton, &QToolButton::clicked, this, &MainWindow::onBack);
    connect(m_forwardButton, &QToolButton::clicked, this, &MainWindow::onForward);
    connect(m_reloadButton, &QToolButton::clicked, this, &MainWindow::onReloadOrStop);
    connect(m_homeButton, &QToolButton::clicked, this, &MainWindow::onGoHome);
    connect(m_bookmarkButton, &QToolButton::clicked, this, &MainWindow::onToggleBookmark);
    connect(m_menuButton, &QToolButton::clicked, this, [this] {
        QMenu menu(this);
        menu.addAction(m_newTabAction);
        menu.addAction(m_closeTabAction);
        menu.addSeparator();
        menu.addAction(tr("History"), this,
                       [this] { openUrl(network::Url::parse(QStringLiteral("about:history"))); });
        menu.addAction(tr("Bookmarks"), this,
                       [this] { openUrl(network::Url::parse(QStringLiteral("about:bookmarks"))); });
        menu.addSeparator();
        menu.addAction(m_devToolsAction);
        menu.addSeparator();
        menu.addAction(tr("Quit"), qApp, &QApplication::quit);
        menu.exec(m_menuButton->mapToGlobal(QPoint(0, m_menuButton->height() + 4)));
    });

    // ------------------------------------------------------------- shortcuts
    //
    // The shortcuts live on actions as well as on the menu bar, so they work
    // when the menu bar is hidden (macOS puts it in the system bar) and keep
    // working while the page has focus.
    m_newTabAction = new QAction(tr("New Tab"), this);
    m_newTabAction->setShortcut(QKeySequence::AddTab);
    connect(m_newTabAction, &QAction::triggered, this, &MainWindow::openNewTab);
    addAction(m_newTabAction);

    m_closeTabAction = new QAction(tr("Close Tab"), this);
    m_closeTabAction->setShortcut(QKeySequence::Close);
    connect(m_closeTabAction, &QAction::triggered, this,
            [this] { onTabCloseRequested(m_tabBar->currentIndex()); });
    addAction(m_closeTabAction);

    m_backAction = new QAction(tr("Back"), this);
    m_backAction->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Left));
    connect(m_backAction, &QAction::triggered, this, &MainWindow::onBack);
    addAction(m_backAction);

    m_forwardAction = new QAction(tr("Forward"), this);
    m_forwardAction->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Right));
    connect(m_forwardAction, &QAction::triggered, this, &MainWindow::onForward);
    addAction(m_forwardAction);

    m_reloadAction = new QAction(tr("Reload"), this);
    m_reloadAction->setShortcut(QKeySequence::Refresh);
    connect(m_reloadAction, &QAction::triggered, this, &MainWindow::onReloadOrStop);
    addAction(m_reloadAction);

    auto *focusShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+L")), this);
    connect(focusShortcut, &QShortcut::activated, this, [this] {
        m_addressBar->setFocus();
        m_addressBar->selectAll();
    });

    m_devToolsAction = new QAction(tr("Developer Tools"), this);
    m_devToolsAction->setCheckable(true);
    m_devToolsAction->setShortcut(QKeySequence(Qt::Key_F12));
    connect(m_devToolsAction, &QAction::toggled, this, &MainWindow::onToggleDevTools);
    addAction(m_devToolsAction);

    // ---------------------------------------------------------------- menus
    QMenu *fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(m_newTabAction);
    fileMenu->addAction(m_closeTabAction);
    fileMenu->addSeparator();
    fileMenu->addAction(tr("&Quit"), QKeySequence::Quit, qApp, &QApplication::quit);

    QMenu *viewMenu = menuBar()->addMenu(tr("&View"));
    viewMenu->addAction(m_backAction);
    viewMenu->addAction(m_forwardAction);
    viewMenu->addAction(m_reloadAction);
    viewMenu->addSeparator();
    viewMenu->addAction(m_devToolsAction);

    QMenu *pagesMenu = menuBar()->addMenu(tr("&Pages"));
    pagesMenu->addAction(tr("New Tab Page"), this, &MainWindow::openNewTab);
    pagesMenu->addAction(tr("History"), this,
                         [this] { openUrl(network::Url::parse(QStringLiteral("about:history"))); });
    pagesMenu->addAction(tr("Bookmarks"), this,
                         [this] { openUrl(network::Url::parse(QStringLiteral("about:bookmarks"))); });
    pagesMenu->addAction(tr("Version"), this,
                         [this] { openUrl(network::Url::parse(QStringLiteral("about:version"))); });

    // --------------------------------------------------------------- theme
    applyTheme();
    connect(qApp->styleHints(), &QStyleHints::colorSchemeChanged, this,
            [this](Qt::ColorScheme) { applyTheme(); });

    // The first tab shows the new tab page, which is what a browser does.
    openNewTab();
}

MainWindow::~MainWindow() = default;

void MainWindow::applyTheme()
{
    m_theme = Theme::forCurrentColorScheme();
    setStyleSheet(m_theme.styleSheet());

    // The painted icons are not styled by the stylesheet, so they are rebuilt
    // for the new colours here.
    const auto refresh = [this](QToolButton *button, NavIcon icon, const QString &tip) {
        if (!button)
            return;
        button->setIcon(m_theme.icon(icon, m_theme.iconColor, m_theme.iconColorDisabled,
                                     kIconSize));
        button->setToolTip(tip);
    };

    refresh(m_newTabButton, NavIcon::NewTab, tr("New tab (Ctrl+T)"));
    refresh(m_backButton, NavIcon::Back, tr("Back (Alt+Left)"));
    refresh(m_forwardButton, NavIcon::Forward, tr("Forward (Alt+Right)"));
    refresh(m_reloadButton, NavIcon::Reload, tr("Reload (F5)"));
    refresh(m_homeButton, NavIcon::Home, tr("Home"));
    refresh(m_menuButton, NavIcon::Menu, tr("Menu"));

    updateSecurityIcon();

    if (m_tabBar)
        m_tabBar->setTheme(m_theme);

    updateNavigationState();
}

int MainWindow::tabCount() const
{
    return m_tabBar ? m_tabBar->count() : 0;
}

int MainWindow::indexOfTab(const WebTab *tab) const
{
    return m_stack ? m_stack->indexOf(const_cast<WebTab *>(tab)) : -1;
}

WebTab *MainWindow::addTab()
{
    auto *tab = new WebTab(m_profile, this);
    tab->setHistory(&m_history);
    tab->setBookmarks(&m_bookmarks);
    tab->setSearchTemplate(m_settings.searchTemplate);

    m_stack->addWidget(tab);
    const int index = m_tabBar->addTab(tr("New Tab"));
    m_tabBar->setTabData(index, QVariant());
    m_tabBar->setCurrentIndex(index);

    connect(tab, &WebTab::titleChanged, this, [this, tab](const QString &title) {
        const int i = m_stack->indexOf(tab);
        if (i >= 0)
            m_tabBar->setTabText(i, title.isEmpty() ? tr("New Tab") : title);
        updateTitle();
    });

    connect(tab, &WebTab::iconChanged, this, [this, tab](const QIcon &icon) {
        const int i = m_stack->indexOf(tab);
        if (i >= 0)
            m_tabBar->setTabIcon(i, icon);
    });

    connect(tab, &WebTab::urlChanged, this, [this](const network::Url &) {
        updateNavigationState();
    });
    connect(tab, &WebTab::loadStarted, this, [this] { updateNavigationState(); });
    connect(tab, &WebTab::loadFinished, this, [this](bool) { updateNavigationState(); });
    connect(tab, &WebTab::loadProgress, this, [this, tab](int progress) {
        if (tab != currentTab())
            return;
        m_progress->setValue(progress);
        m_progress->setVisible(progress > 0 && progress < 100);
    });
    connect(tab, &WebTab::linkHovered, this, [this](const QString &url) {
        m_statusLabel->setText(url);
    });
    connect(tab, &WebTab::newViewRequested, this, &MainWindow::onNewViewRequested);

    syncTabs();
    return tab;
}

WebTab *MainWindow::currentTab() const
{
    return qobject_cast<WebTab *>(m_stack->currentWidget());
}

void MainWindow::removeTab(int index)
{
    if (index < 0 || index >= m_tabBar->count())
        return;

    QWidget *page = m_stack->widget(index);
    m_tabBar->removeTab(index);
    m_stack->removeWidget(page);
    delete page;

    // Closing the last tab opens a fresh one, so the window never sits empty.
    if (m_tabBar->count() == 0) {
        openNewTab();
        return;
    }

    syncTabs();
    updateNavigationState();
}

void MainWindow::syncTabs()
{
    const int count = m_tabBar->count();
    for (int i = 0; i < count; ++i) {
        if (auto *tab = qobject_cast<WebTab *>(m_stack->widget(i)))
            tab->setTabCount(count);
    }
}

void MainWindow::openUrl(const network::Url &url)
{
    WebTab *tab = currentTab();
    if (!tab) {
        openNewTab();
        tab = currentTab();
        if (!tab)
            return;
    }
    tab->navigate(url);
}

void MainWindow::openNewTab()
{
    WebTab *tab = addTab();
    if (!tab)
        return;
    tab->navigate(network::Url::parse(QStringLiteral("about:home")));
}

void MainWindow::openInNewTab(const network::Url &url)
{
    WebTab *tab = addTab();
    if (!tab)
        return;
    tab->navigate(url);
}

void MainWindow::onTabChanged(int index)
{
    m_stack->setCurrentIndex(index);

    // A tab that has never loaded anything shows the new tab page, so a fresh
    // tab is never a blank pane.
    updateTitle();
    updateNavigationState();
    updateSecurityIcon();

    if (m_devToolsDock && m_devToolsDock->isVisible()) {
        if (WebTab *tab = currentTab())
            tab->attachDevTools(m_devToolsView->page());
    }
}

void MainWindow::onTabCloseRequested(int index)
{
    removeTab(index);
}

void MainWindow::onNewViewRequested(QWebEngineView *view)
{
    // A target=_blank link or window.open() arrives here: the page asked for a
    // new view, which becomes a new tab rather than a separate window.
    auto *tab = new WebTab(view, m_profile, this);
    tab->setHistory(&m_history);
    tab->setBookmarks(&m_bookmarks);
    tab->setSearchTemplate(m_settings.searchTemplate);

    m_stack->addWidget(tab);
    m_stack->setCurrentWidget(tab);
    const int index = m_tabBar->addTab(view->title().isEmpty() ? tr("New Tab") : view->title());
    m_tabBar->setCurrentIndex(index);

    connect(tab, &WebTab::titleChanged, this, [this, tab](const QString &title) {
        const int i = m_stack->indexOf(tab);
        if (i >= 0)
            m_tabBar->setTabText(i, title.isEmpty() ? tr("New Tab") : title);
        updateTitle();
    });
    connect(tab, &WebTab::iconChanged, this, [this, tab](const QIcon &icon) {
        const int i = m_stack->indexOf(tab);
        if (i >= 0)
            m_tabBar->setTabIcon(i, icon);
    });
    connect(tab, &WebTab::urlChanged, this,
            [this](const network::Url &) { updateNavigationState(); });
    connect(tab, &WebTab::loadStarted, this, [this] { updateNavigationState(); });
    connect(tab, &WebTab::loadFinished, this, [this](bool) { updateNavigationState(); });
    connect(tab, &WebTab::linkHovered, this,
            [this](const QString &url) { m_statusLabel->setText(url); });
    connect(tab, &WebTab::newViewRequested, this, &MainWindow::onNewViewRequested);

    syncTabs();
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

void MainWindow::onReloadOrStop()
{
    WebTab *tab = currentTab();
    if (!tab)
        return;

    // One button does both, as in every browser: while a page is loading it
    // stops it, and otherwise it reloads.
    if (tab->isLoading())
        tab->stop();
    else
        tab->reload();
}

void MainWindow::onGoHome()
{
    // Home goes to the start page in the tab that is already open, the way every
    // other browser does it: it is a navigation, so Back returns to the page the
    // user came from rather than the button being a second "new tab".
    WebTab *tab = currentTab();
    if (!tab)
        return;

    tab->navigate(network::Url::parse(QStringLiteral("about:home")));
}

void MainWindow::onToggleBookmark()
{
    WebTab *tab = currentTab();
    if (!tab)
        return;

    const bool added = m_bookmarks.toggle(tab->url(), tab->title());
    m_bookmarkButton->setIcon(
        m_theme.icon(added ? NavIcon::StarFilled : NavIcon::Star,
                     added ? m_theme.accent : m_theme.iconColor, m_theme.iconColorDisabled,
                     kIconSize));
    m_statusLabel->setText(added ? tr("Bookmarked") : tr("Bookmark removed"));
}

void MainWindow::onToggleDevTools(bool show)
{
    if (!m_devToolsDock || !m_devToolsView)
        return;

    m_devToolsDock->setVisible(show);

    if (WebTab *tab = currentTab()) {
        if (show)
            tab->attachDevTools(m_devToolsView->page());
        else
            tab->detachDevTools();
    }
}

void MainWindow::updateSecurityIcon()
{
    WebTab *tab = currentTab();
    const network::Url url = tab ? tab->url() : network::Url();

    NavIcon which = NavIcon::Globe;
    QString tip = tr("Not secure");
    if (url.isHttps()) {
        which = NavIcon::Lock;
        tip = tr("Connection is encrypted");
    } else if (url.isAbout()) {
        which = NavIcon::Info;
        tip = tr("Built-in page");
    }

    // The icon is shown inside the field, at its leading edge, which is where a
    // browser tells the user what they are connected to.
    const QList<QAction *> actions = m_addressBar->actions();
    for (QAction *action : actions) {
        if (action->property("security").toBool()) {
            m_addressBar->removeAction(action);
            action->deleteLater();
        }
    }

    auto *action = m_addressBar->addAction(m_theme.icon(which, m_theme.iconColor, 15),
                                           QLineEdit::LeadingPosition);
    action->setProperty("security", true);
    action->setToolTip(tip);
    action->setEnabled(false);
}

void MainWindow::updateNavigationState()
{
    WebTab *tab = currentTab();

    if (!tab) {
        m_backButton->setEnabled(false);
        m_forwardButton->setEnabled(false);
        m_addressBar->clear();
        m_progress->setVisible(false);
        return;
    }

    const bool canGoBack = tab->view()->history()->canGoBack();
    const bool canGoForward = tab->view()->history()->canGoForward();
    m_backButton->setEnabled(canGoBack);
    m_forwardButton->setEnabled(canGoForward);
    m_backAction->setEnabled(canGoBack);
    m_forwardAction->setEnabled(canGoForward);

    // The toolbar button shows Stop while loading and Reload when idle.
    m_reloadButton->setIcon(m_theme.icon(tab->isLoading() ? NavIcon::Stop : NavIcon::Reload,
                                         m_theme.iconColor, kIconSize));
    m_reloadButton->setToolTip(tab->isLoading() ? tr("Stop loading") : tr("Reload (F5)"));

    // The address bar follows the page, but only while the user is not typing
    // into it. Focus alone is the wrong test: clicking the field to read the URL
    // leaves it focused, and the address would then stop tracking navigation.
    // The edit flag is what distinguishes reading from typing.
    if (!m_addressBar->hasFocus() || !m_addressBarEdited) {
        m_addressBar->setText(tab->url().toString());
        m_addressBarEdited = false;
    }

    const bool bookmarked = m_bookmarks.contains(tab->url());
    m_bookmarkButton->setIcon(m_theme.icon(bookmarked ? NavIcon::StarFilled : NavIcon::Star,
                                           bookmarked ? m_theme.accent : m_theme.iconColor,
                                           m_theme.iconColorDisabled, kIconSize));

    if (tab->isLoading()) {
        m_statusLabel->setText(tr("Loading %1…").arg(tab->url().displayHost()));
    } else if (m_statusLabel->text().startsWith(tr("Loading"))) {
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
    m_tabBar->setTabText(m_tabBar->currentIndex(),
                         title.isEmpty() ? tr("New Tab") : title);
}

} // namespace oqb::ui
