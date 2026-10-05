#pragma once

#include <QMainWindow>
#include <QString>

#include "browser/PageSettings.h"
#include "storage/Bookmarks.h"
#include "storage/History.h"

class QAction;
class QLabel;
class QLineEdit;
class QProgressBar;
class QTabWidget;
class QToolBar;

namespace oqb::browser {
class Tab;
}
namespace oqb::ui {
class PageView;
}

namespace oqb::ui {

/// The browser window: tabs, an address bar, and the navigation controls.
///
/// The window owns the shared history and bookmarks and hands them to each tab,
/// so that back and forward are consistent across tabs and the history page
/// shows everything the user has done.
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
    void onShowBoxModel(bool show);
    void updateNavigationState();
    void updateTitle();

private:
    /// Creates a tab, wires its signals and returns it.
    browser::Tab *addTab();
    browser::Tab *currentTab() const;
    PageView *currentView() const;
    /// Applies the shared stores and the tab count to every tab.
    void syncTabs();

    browser::PageSettings m_settings;
    storage::HistoryStore m_history;
    storage::BookmarkStore m_bookmarks;

    QTabWidget *m_tabs = nullptr;
    QLineEdit *m_addressBar = nullptr;
    QLabel *m_statusLabel = nullptr;
    QProgressBar *m_progress = nullptr;

    QAction *m_backAction = nullptr;
    QAction *m_forwardAction = nullptr;
    QAction *m_reloadAction = nullptr;
    QAction *m_stopAction = nullptr;
    QAction *m_bookmarkAction = nullptr;
    QAction *m_boxModelAction = nullptr;
};

} // namespace oqb::ui
