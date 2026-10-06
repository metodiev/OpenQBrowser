#pragma once

#include <QString>
#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTabWidget;
class QTreeWidget;
class QTreeWidgetItem;

namespace oqb::browser {
class Page;
class Tab;
}
namespace oqb::dom {
class Element;
class Document;
}
namespace oqb::ui {
class PageView;
}

namespace oqb::ui {

/// The developer tools: a console, an element inspector, a layout view and a
/// resource list.
///
/// The panel holds no state of its own beyond the selection. Every view is built
/// by asking the page what it currently is, through `devtools::Inspector`, so a
/// refresh after a navigation or a script's change shows what is there now
/// rather than what was there when the panel opened. That also keeps the panel
/// usable while a page is still loading.
///
/// The console is the one part that acts rather than reports: an expression
/// typed into it is evaluated in the page's own engine, so it sees the same
/// globals, the same DOM and the same timers the page does.
class DevToolsPanel : public QWidget
{
    Q_OBJECT

public:
    explicit DevToolsPanel(QWidget *parent = nullptr);
    ~DevToolsPanel() override;

    /// Points the panel at a tab. A null tab clears every view.
    void setTab(browser::Tab *tab);

    /// Rebuilds every view from the page's current state. Called when the page
    /// loads, when a script changes it, and when the user asks for a refresh.
    void refresh();

    /// Shows `element` in the elements tab, expanding its ancestors so it is
    /// visible. Used by the element picker and by a click in the page.
    void revealElement(const dom::Element *element);

    /// Tells the panel the page's document may have changed. A script that
    /// inserts or removes elements does not change the document object, so the
    /// panel cannot detect it by comparing pointers and is told instead.
    void noteDocumentChanged();

    /// Selects one of the tabs by name: "console", "elements", "layout" or
    /// "network".
    void showTab(const QString &name);

    /// Arms the element picker on the page the panel is attached to, if the
    /// panel knows the view.
    void setPageView(PageView *view);

signals:
    /// A message for the status bar.
    void statusMessage(const QString &message);

private slots:
    void onEvaluate();
    void onTreeSelectionChanged();
    void onRefresh();
    void onPickElement();

private:
    /// The page the panel is showing, or nullptr.
    browser::Page *page() const;

    /// Adds a line to the console, coloured by level.
    void appendConsole(const QString &text, const QString &kind);


    /// Rebuilds the elements tree from the document.
    void buildTree();
    /// Fills a tree item from a DOM node and recurses into its children.
    void fillTreeItem(QTreeWidgetItem *item, const dom::Element *element);
    /// Finds the tree item for an element, or nullptr when it is not shown.
    QTreeWidgetItem *findTreeItem(const dom::Element *element) const;

    browser::Tab *m_tab = nullptr;
    PageView *m_view = nullptr;

    QTabWidget *m_tabs = nullptr;

    // Console.
    QPlainTextEdit *m_console = nullptr;
    QLineEdit *m_input = nullptr;

    // Elements.
    QTreeWidget *m_tree = nullptr;
    QPlainTextEdit *m_styleView = nullptr;
    QPlainTextEdit *m_rulesView = nullptr;
    QLabel *m_elementLabel = nullptr;
    /// The element currently shown, so a refresh can keep the selection.
    const dom::Element *m_selected = nullptr;
    /// Which navigation the tree was built for. A document pointer cannot serve
    /// here: the allocator reuses the address of the document a navigation just
    /// freed, so a rebuilt tree would look as though it described the old page.
    quint64 m_treeNavigation = 0;
    /// The document the tree currently describes, for the selected-element
    /// lookup within one navigation.
    const dom::Document *m_treeDocument = nullptr;

    // Layout and network.
    QPlainTextEdit *m_layoutView = nullptr;
    QPlainTextEdit *m_networkView = nullptr;
    QPlainTextEdit *m_cookieView = nullptr;

    QPushButton *m_pickButton = nullptr;
    /// How many console entries the view currently shows, and which document
    /// they came from. Together these decide whether the view has to be redrawn.
    int m_shownMessages = 0;
    /// Which navigation the console was last drawn for, and how many lines it
    /// showed then.
    quint64 m_consoleNavigation = 0;
    /// True when the element tree has to be rebuilt before it is shown.
    bool m_treeNeedsRebuild = false;
    /// Set by a caller that is asking for a rebuild, and cleared when one
    /// happens.
    bool m_refreshRequestedRebuild = false;
};

} // namespace oqb::ui
