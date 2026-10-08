#pragma once

#include <QMainWindow>
#include <QString>

#include "browser/PageSettings.h"
#include "network/Url.h"
#include "storage/Bookmarks.h"
#include "storage/History.h"
#include "ui/Theme.h"

class QAction;
class QDockWidget;
class QLabel;
class QLineEdit;
class QProgressBar;
class QStackedWidget;
class QTabBar;
class QToolButton;
class QWebEnginePage;
class QWebEngineProfile;
class QWebEngineView;

namespace oqb::ui {
class TabBar;
class WebTab;
}

namespace oqb::ui {

/// The browser window.
///
/// The layout follows what users expect from a modern browser: a tab strip on
/// top, a toolbar with the address bar under it, and a hairline progress bar
/// between the chrome and the page. Each tab is a WebTab rendered by Qt
/// WebEngine (Chromium).
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(const browser::PageSettings &settings = {}, QWidget *parent = nullptr);
    ~MainWindow() override;

    /// Opens a URL in the current tab.
    void openUrl(const network::Url &url);
    /// Opens a URL in a new tab and switches to it.
    void openInNewTab(const network::Url &url);
    /// Opens a new tab on the built-in new tab page.
    void openNewTab();

    int tabCount() const;

private slots:
    void onTabChanged(int index);
    void onTabCloseRequested(int index);
    void onAddressEntered();
    void onBack();
    void onForward();
    void onReloadOrStop();
    void onToggleBookmark();
    void onToggleDevTools(bool show);
    void onNewViewRequested(QWebEngineView *view);
    void updateNavigationState();
    void updateTitle();
    /// Rebuilds the stylesheet and the painted icons when the desktop switches
    /// between light and dark appearance.
    void applyTheme();

private:
    /// Creates a tab with its own view and adds it to the strip and the stack.
    WebTab *addTab();
    WebTab *currentTab() const;
    /// The index of `tab` in the strip, or -1.
    int indexOfTab(const WebTab *tab) const;
    /// Removes a tab, its stack page and its widgets.
    void removeTab(int index);
    /// Applies the shared stores and the tab count to every tab.
    void syncTabs();
    /// Rewrites the address bar's security icon for the current URL.
    void updateSecurityIcon();

    browser::PageSettings m_settings;
    storage::HistoryStore m_history;
    storage::BookmarkStore m_bookmarks;

    Theme m_theme;
    QWebEngineProfile *m_profile = nullptr;

    TabBar *m_tabBar = nullptr;
    QStackedWidget *m_stack = nullptr;
    QToolButton *m_newTabButton = nullptr;
    QToolButton *m_backButton = nullptr;
    QToolButton *m_forwardButton = nullptr;
    QToolButton *m_reloadButton = nullptr;
    QToolButton *m_homeButton = nullptr;
    QToolButton *m_bookmarkButton = nullptr;
    QToolButton *m_menuButton = nullptr;
    QLineEdit *m_addressBar = nullptr;
    QLabel *m_statusLabel = nullptr;
    QProgressBar *m_progress = nullptr;
    /// True while the user is typing into the address bar, so a finishing load
    /// cannot overwrite what they are writing. Set by textEdited and cleared
    /// when editing finishes, which is what distinguishes typing from a click
    /// that merely put the caret in the field.
    bool m_addressBarEdited = false;

    QAction *m_newTabAction = nullptr;
    QAction *m_closeTabAction = nullptr;
    QAction *m_backAction = nullptr;
    QAction *m_forwardAction = nullptr;
    QAction *m_reloadAction = nullptr;
    QAction *m_devToolsAction = nullptr;

    QDockWidget *m_devToolsDock = nullptr;
    QWebEngineView *m_devToolsView = nullptr;
};

} // namespace oqb::ui
