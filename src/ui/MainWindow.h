#pragma once

#include <QMainWindow>
#include <QString>

#include "browser/PageSettings.h"
#include "network/Url.h"
#include "storage/Bookmarks.h"
#include "storage/History.h"

class QAction;
class QDockWidget;
class QLabel;
class QLineEdit;
class QProgressBar;
class QTabWidget;
class QToolBar;
class QWebEnginePage;
class QWebEngineProfile;
class QWebEngineView;

namespace oqb::ui {
class WebTab;
}

namespace oqb::ui {

/// The browser window: tabs, an address bar, and the navigation controls.
///
/// Each tab is a WebTab backed by Qt WebEngine (Chromium). The window owns the
/// shared history and bookmarks and hands them to each tab, so the built-in
/// about: pages show everything the user has done.
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

    int tabCount() const;

private slots:
    void onTabChanged(int index);
    void onTabClosed(int index);
    void onAddressEntered();
    void onBack();
    void onForward();
    void onReload();
    void onStop();
    void onToggleBookmark();
    void onToggleDevTools(bool show);
    void onNewViewRequested(QWebEngineView *view);
    void updateNavigationState();
    void updateTitle();

private:
    /// Creates a tab with its own view and wires its signals.
    WebTab *addTab();
    WebTab *currentTab() const;
    /// Applies the shared stores and the tab count to every tab.
    void syncTabs();

    browser::PageSettings m_settings;
    storage::HistoryStore m_history;
    storage::BookmarkStore m_bookmarks;

    QWebEngineProfile *m_profile = nullptr;
    QTabWidget *m_tabs = nullptr;
    QLineEdit *m_addressBar = nullptr;
    QLabel *m_statusLabel = nullptr;
    QProgressBar *m_progress = nullptr;

    QAction *m_backAction = nullptr;
    QAction *m_forwardAction = nullptr;
    QAction *m_reloadAction = nullptr;
    QAction *m_stopAction = nullptr;
    QAction *m_bookmarkAction = nullptr;
    QAction *m_devToolsAction = nullptr;

    QDockWidget *m_devToolsDock = nullptr;
    QWebEngineView *m_devToolsView = nullptr;
};

} // namespace oqb::ui
