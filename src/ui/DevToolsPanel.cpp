#include "ui/DevToolsPanel.h"

#include <QComboBox>
#include <QDir>
#include <QHash>
#include <QVariant>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <functional>

#include "browser/Page.h"
#include "browser/Tab.h"
#include "css/Style.h"
#include "devtools/Inspector.h"
#include "dom/Document.h"
#include "javascript/Engine.h"
#include "javascript/ScriptEngine.h"
#include "renderer/BoxTree.h"
#include "renderer/Layout.h"
#include "renderer/Layout.h"
#include "ui/PageView.h"

namespace oqb::ui {

namespace {

/// A monospaced font, which every devtools view wants: the trees, the
/// declarations and the console all line up in columns that a proportional font
/// would ruin.
QFont monospaceFont()
{
    return QFontDatabase::systemFont(QFontDatabase::FixedFont);
}

/// How an element is named in the tree: the tag, plus the id and classes that a
/// reader uses to tell one div from another.
QString elementLabel(const dom::Element *element)
{
    if (!element)
        return {};

    QString label = element->tagName().toLower();
    if (const QString id = element->id(); !id.isEmpty())
        label += QStringLiteral("#") + id;
    for (const QString &name : element->classList())
        label += QStringLiteral(".") + name;
    return label;
}

/// The kind of a console line, used for its colour. The DOM messages the browser
/// itself produces are distinguished from the page's own output.
QString kindForLevel(javascript::ConsoleMessage::Level level)
{
    switch (level) {
    case javascript::ConsoleMessage::Level::Error: return QStringLiteral("error");
    case javascript::ConsoleMessage::Level::Warning: return QStringLiteral("warning");
    case javascript::ConsoleMessage::Level::Info: return QStringLiteral("info");
    case javascript::ConsoleMessage::Level::Log: break;
    }
    return QStringLiteral("log");
}

} // namespace

DevToolsPanel::DevToolsPanel(QWidget *parent)
    : QWidget(parent)
{
    m_tabs = new QTabWidget(this);

    // ------------------------------------------------------------- console
    auto *consolePage = new QWidget(this);
    auto *consoleLayout = new QVBoxLayout(consolePage);
    consoleLayout->setContentsMargins(4, 4, 4, 4);
    consoleLayout->setSpacing(4);

    m_console = new QPlainTextEdit(consolePage);
    m_console->setObjectName(QStringLiteral("consoleOutput"));
    m_console->setReadOnly(true);
    m_console->setFont(monospaceFont());
    m_console->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_console->setPlaceholderText(
        QStringLiteral("Console output from the page appears here."));
    consoleLayout->addWidget(m_console, 1);

    auto *inputRow = new QHBoxLayout;
    auto *prompt = new QLabel(QStringLiteral("&gt;"), consolePage);
    prompt->setFont(monospaceFont());
    m_input = new QLineEdit(consolePage);
    m_input->setObjectName(QStringLiteral("consoleInput"));
    m_input->setFont(monospaceFont());
    m_input->setPlaceholderText(
        QStringLiteral("Run JavaScript in the page, then press Enter"));
    inputRow->addWidget(prompt);
    inputRow->addWidget(m_input, 1);
    consoleLayout->addLayout(inputRow);

    connect(m_input, &QLineEdit::returnPressed, this, &DevToolsPanel::onEvaluate);

    m_tabs->addTab(consolePage, QStringLiteral("Console"));

    // ------------------------------------------------------------ elements
    auto *elementsPage = new QWidget(this);
    auto *elementsLayout = new QVBoxLayout(elementsPage);
    elementsLayout->setContentsMargins(4, 4, 4, 4);
    elementsLayout->setSpacing(4);

    auto *pickRow = new QHBoxLayout;
    m_pickButton = new QPushButton(QStringLiteral("Pick element"), elementsPage);
    m_pickButton->setCheckable(false);
    m_pickButton->setToolTip(
        QStringLiteral("Click an element in the page to inspect it"));
    m_elementLabel = new QLabel(QStringLiteral("No element selected"), elementsPage);
    m_elementLabel->setObjectName(QStringLiteral("selectedElement"));
    m_elementLabel->setFont(monospaceFont());
    pickRow->addWidget(m_pickButton);
    pickRow->addWidget(m_elementLabel, 1);
    elementsLayout->addLayout(pickRow);

    m_tree = new QTreeWidget(elementsPage);
    m_tree->setObjectName(QStringLiteral("elementTree"));
    m_tree->setFont(monospaceFont());
    m_tree->setHeaderHidden(true);
    m_tree->setUniformRowHeights(true);

    m_styleView = new QPlainTextEdit(elementsPage);
    m_styleView->setObjectName(QStringLiteral("computedStyle"));
    m_styleView->setReadOnly(true);
    m_styleView->setFont(monospaceFont());
    m_styleView->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_styleView->setPlaceholderText(QStringLiteral("Computed style"));

    m_rulesView = new QPlainTextEdit(elementsPage);
    m_rulesView->setObjectName(QStringLiteral("appliedRules"));
    m_rulesView->setReadOnly(true);
    m_rulesView->setFont(monospaceFont());
    m_rulesView->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_rulesView->setPlaceholderText(QStringLiteral("Rules that applied"));

    // The tree on the left, the two style views stacked on the right: the shape
    // every browser's inspector uses, because it keeps the tree visible while a
    // rule is read.
    auto *rightSplitter = new QSplitter(Qt::Vertical, elementsPage);
    rightSplitter->addWidget(m_styleView);
    rightSplitter->addWidget(m_rulesView);

    auto *elementsSplitter = new QSplitter(Qt::Horizontal, elementsPage);
    elementsSplitter->addWidget(m_tree);
    elementsSplitter->addWidget(rightSplitter);
    elementsSplitter->setStretchFactor(0, 1);
    elementsSplitter->setStretchFactor(1, 2);
    elementsLayout->addWidget(elementsSplitter, 1);

    connect(m_tree, &QTreeWidget::itemSelectionChanged, this,
            &DevToolsPanel::onTreeSelectionChanged);
    connect(m_pickButton, &QPushButton::clicked, this, &DevToolsPanel::onPickElement);

    m_tabs->addTab(elementsPage, QStringLiteral("Elements"));

    // -------------------------------------------------------------- layout
    m_layoutView = new QPlainTextEdit(this);
    m_layoutView->setObjectName(QStringLiteral("layoutReport"));
    m_layoutView->setReadOnly(true);
    m_layoutView->setFont(monospaceFont());
    m_layoutView->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_tabs->addTab(m_layoutView, QStringLiteral("Layout"));

    // ------------------------------------------------------------- network
    m_networkView = new QPlainTextEdit(this);
    m_networkView->setObjectName(QStringLiteral("resourceReport"));
    m_networkView->setReadOnly(true);
    m_networkView->setFont(monospaceFont());
    m_networkView->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_tabs->addTab(m_networkView, QStringLiteral("Network"));

    // --------------------------------------------------------------- shell
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *tools = new QHBoxLayout;
    tools->setContentsMargins(4, 4, 4, 0);
    auto *refresh = new QPushButton(QStringLiteral("Refresh"), this);
    connect(refresh, &QPushButton::clicked, this, &DevToolsPanel::onRefresh);
    tools->addWidget(refresh);
    tools->addStretch(1);
    layout->addLayout(tools);
    layout->addWidget(m_tabs, 1);
}

DevToolsPanel::~DevToolsPanel() = default;

void DevToolsPanel::setPageView(PageView *view)
{
    if (m_view == view)
        return;

    if (m_view)
        m_view->disconnect(this);

    m_view = view;

    if (m_view) {
        // The picker reports back through the view, so the signal is connected
        // once here rather than each time the button is pressed.
        connect(m_view, &PageView::elementPicked, this, [this](const dom::Element *element) {
            m_pickButton->setEnabled(true);
            if (element) {
                revealElement(element);
                emit statusMessage(QStringLiteral("Selected %1").arg(elementLabel(element)));
            } else {
                emit statusMessage(QStringLiteral("No element under the pointer"));
            }
        });
    }
}

void DevToolsPanel::setTab(browser::Tab *tab)
{
    if (m_tab == tab)
        return;

    m_tab = tab;

    // Detaching from a page invalidates every view. Both markers are reset, not
    // just the document: the new tab may sit at the same navigation number, in
    // which case comparing ids would leave the previous page's tree in place.
    m_treeDocument = nullptr;
    m_treeNavigation = 0;

    refresh();
}

browser::Page *DevToolsPanel::page() const
{
    return m_tab ? m_tab->page() : nullptr;
}

void DevToolsPanel::onRefresh()
{
    // An explicit refresh always rebuilds the tree, because the user asking for
    // one usually means "show me what is there now".
    m_refreshRequestedRebuild = true;
    refresh();
}

void DevToolsPanel::noteDocumentChanged()
{
    // Called when a script has run and may have altered the tree. The rebuild
    // itself is deferred to refresh(), so several changes cost one rebuild.
    m_treeNeedsRebuild = true;
    refresh();
}

void DevToolsPanel::refresh()
{
    browser::Page *current = page();
    if (!current || !current->document()) {
        m_layoutView->setPlainText(
            QStringLiteral("No page loaded.\n\nOpen a page to inspect its layout."));
        m_networkView->setPlainText(QStringLiteral("No page loaded."));
        return;
    }

    // ---------------------------------------------------------- new document
    //
    // A different document means a navigation, so everything the panel showed
    // belonged to a page that is gone. This runs before any view is filled, or
    // the console would re-append the previous page's output and the clearing
    // would look as though it had not happened.
    //
    // The check belongs here rather than in setTab() because a navigation does
    // not replace the tab, and callers reach refresh() without passing through
    // setTab() at all - the window's own load handler among them.
    // A refresh is also how a caller says "the page changed". The panel cannot
    // detect a completed navigation from a document pointer alone when the same
    // tab is reused, so the request itself is taken as the signal.
    if (m_treeNavigation != current->navigationId() || m_refreshRequestedRebuild) {
        m_refreshRequestedRebuild = false;
        // Only the views are emptied. The page's own message log is not touched:
        // it belongs to the page, the browser already clears it when a load
        // starts, and clearing it here would discard what the new page has
        // already logged before the panel was told about it.
        for (QPlainTextEdit *view : {m_console, m_styleView, m_rulesView, m_layoutView,
                                     m_networkView}) {
            view->clear();
        }
        m_tree->clear();
        m_selected = nullptr;
        m_shownMessages = 0;
        m_elementLabel->setText(QStringLiteral("No element selected"));

        m_treeNavigation = current->navigationId();
        m_treeDocument = current->document();
        m_shownMessages = 0;
        m_consoleNavigation = 0;
        buildTree();
        m_treeNeedsRebuild = false;

    }

    // ------------------------------------------------------------- console
    //
    // The view is redrawn from the log rather than appended to. A running count
    // cannot be kept in step: the browser clears the log when a load starts, and
    // a clear arriving between two refreshes would leave the count ahead of the
    // list and hide the new page's first messages. Redrawing is cheap - a page
    // logs a handful of lines - and cannot drift.
    const QList<javascript::ConsoleMessage> &messages = current->scripts()->messages();
    if (messages.size() != m_shownMessages
        || m_consoleNavigation != current->navigationId()) {
        m_console->clear();
        for (const javascript::ConsoleMessage &message : messages) {
            QString text = message.text;
            if (!message.source.isEmpty())
                text += QStringLiteral("    (%1)").arg(message.source);
            appendConsole(text, kindForLevel(message.level));
        }
        m_shownMessages = messages.size();
        m_consoleNavigation = current->navigationId();
    }

    // ------------------------------------------------------------ elements
    //
    // A script can change the tree without changing the document object, so the
    // view is rebuilt whenever the document has been touched since it was built.
    // Rebuilding is cheap compared with the layout that produced the page, and
    // it is the only way the tree cannot show a stale element.
    if (m_treeNeedsRebuild || m_refreshRequestedRebuild) {
        m_refreshRequestedRebuild = false;
        const dom::Element *previous = m_selected;
        buildTree();
        m_treeNeedsRebuild = false;
        if (previous) {
            if (QTreeWidgetItem *item = findTreeItem(previous))
                m_tree->setCurrentItem(item);
            else
                m_selected = nullptr;
        }
    }

    // Keep the selection meaningful: a script may have removed the element that
    // was being inspected, in which case there is nothing to show. The element
    // having no parent is what "removed" means, and it is the cheapest reliable
    // test - an element detached but still referenced is no longer on the page.
    if (m_selected && !findTreeItem(m_selected)) {
        m_selected = nullptr;
        m_elementLabel->setText(QStringLiteral("No element selected"));
        m_styleView->clear();
        m_rulesView->clear();
    }

    // -------------------------------------------------------------- layout
    const QString summary = devtools::Inspector::layoutSummary(current->layout());
    const QString boxes = devtools::Inspector::boxTree(current->boxTree());
    m_layoutView->setPlainText(summary + QStringLiteral("\n") + boxes);

    // ------------------------------------------------------------- network
    QStringList resources;
    const QStringList requested = current->requestedResources();
    if (requested.isEmpty()) {
        resources << QStringLiteral("No external resources.");
    } else {
        resources << QStringLiteral("Requested (%1):").arg(requested.size());
        for (const QString &url : requested)
            resources << QStringLiteral("  %1").arg(url);
    }
    const QStringList failed = current->failedResources();
    if (!failed.isEmpty()) {
        resources << QString() << QStringLiteral("Failed (%1):").arg(failed.size());
        for (const QString &entry : failed)
            resources << QStringLiteral("  %1").arg(entry);
    }
    m_networkView->setPlainText(resources.join(u'\n'));

}

void DevToolsPanel::appendConsole(const QString &text, const QString &kind)
{
    // A colour per level, which is the one thing that makes a long console
    // readable: an error has to be findable at a glance.
    QString colour = QStringLiteral("#d0d0d0");
    if (kind == QLatin1String("error"))
        colour = QStringLiteral("#ff8080");
    else if (kind == QLatin1String("warning"))
        colour = QStringLiteral("#e0c060");
    else if (kind == QLatin1String("info"))
        colour = QStringLiteral("#80b0ff");

    // The text is escaped and wrapped in a span rather than inserted as markup,
    // because a page's console output can contain anything at all.
    const QString escaped = text.toHtmlEscaped();
    m_console->appendHtml(QStringLiteral("<span style=\"color:%1; white-space:pre;\">%2</span>")
                              .arg(colour, escaped));
}

void DevToolsPanel::onEvaluate()
{
    browser::Page *current = page();
    if (!current || !current->document()) {
        appendConsole(QStringLiteral("No page loaded."), QStringLiteral("error"));
        return;
    }

    const QString source = m_input->text();
    if (source.trimmed().isEmpty())
        return;

    m_input->clear();

    // The expression is echoed so the console reads like a transcript rather
    // than a list of answers.
    appendConsole(QStringLiteral("&gt; ") + source, QStringLiteral("info"));

    if (!current->scripts()->isAvailable()) {
        appendConsole(QStringLiteral("No JavaScript engine in this build."),
                      QStringLiteral("error"));
        return;
    }

    const javascript::ExecutionResult result
        = current->scripts()->execute(source, current->document(), QStringLiteral("console"));

    // A console echoes the line's completion value, which is what makes typing
    // `document.title` useful.
    if (!result.value.isEmpty())
        appendConsole(result.value, QStringLiteral("log"));

    if (!result.success)
        appendConsole(result.error, QStringLiteral("error"));

    // A script run from the console may have changed the page. serviceScripts()
    // re-styles and re-lays out when that happened, which is what makes a change
    // typed into the console show up in the window and in the views below.
    current->serviceScripts(0);

    // The tree is rebuilt, because a console expression that inserts or removes
    // elements leaves the view describing a document that no longer exists while
    // the document object itself is unchanged.
    m_treeNeedsRebuild = true;

    // Redrawing the views picks up the console output this call produced and any
    // change the expression made to the document.
    refresh();

    emit statusMessage(QStringLiteral("Evaluated in the page"));
}

void DevToolsPanel::buildTree()
{
    m_tree->clear();

    browser::Page *current = page();
    if (!current || !current->document() || !current->document()->documentElement())
        return;

    auto *root = new QTreeWidgetItem(m_tree);
    fillTreeItem(root, current->document()->documentElement());
    // The root is expanded so the page is visible without a click, which is what
    // a reader wants to see first.
    root->setExpanded(true);
}

void DevToolsPanel::fillTreeItem(QTreeWidgetItem *item, const dom::Element *element)
{
    if (!element)
        return;

    item->setText(0, elementLabel(element));
    // The element pointer is carried on the item so a selection can be turned
    // back into a DOM node without a second search.
    item->setData(0, Qt::UserRole, QVariant::fromValue(static_cast<const void *>(element)));

    for (const dom::Element *child : element->childElements()) {
        auto *childItem = new QTreeWidgetItem(item);
        fillTreeItem(childItem, child);
    }

    // A text-only element is shown with its text, which is what makes a tree of
    // divs readable.
    const QString text = element->textContent().trimmed();
    if (text.isEmpty() || !element->childElements().isEmpty())
        return;

    const QString collapsed = text.simplified();
    const QString shown = collapsed.size() > 60 ? collapsed.left(57) + QStringLiteral("...")
                                                : collapsed;
    auto *textItem = new QTreeWidgetItem(item);
    textItem->setText(0, QStringLiteral("\"%1\"").arg(shown));
}

QTreeWidgetItem *DevToolsPanel::findTreeItem(const dom::Element *element) const
{
    if (!element)
        return nullptr;

    // A linear walk is enough: the tree is small enough to fit on screen, and a
    // map would have to be rebuilt whenever the document changed.
    std::function<QTreeWidgetItem *(QTreeWidgetItem *)> search = [&](QTreeWidgetItem *item)
        -> QTreeWidgetItem * {
        if (!item)
            return nullptr;
        if (item->data(0, Qt::UserRole).value<const void *>() == element)
            return item;
        for (int i = 0; i < item->childCount(); ++i) {
            if (QTreeWidgetItem *found = search(item->child(i)))
                return found;
        }
        return nullptr;
    };

    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        if (QTreeWidgetItem *found = search(m_tree->topLevelItem(i)))
            return found;
    }
    return nullptr;
}

void DevToolsPanel::revealElement(const dom::Element *element)
{
    if (!element)
        return;

    QTreeWidgetItem *item = findTreeItem(element);
    if (!item) {
        // The panel may be showing an older document, so it is rebuilt before
        // giving up.
        buildTree();
        item = findTreeItem(element);
    }
    if (!item)
        return;

    // Every ancestor is expanded, or the item would be selected but not visible.
    for (QTreeWidgetItem *parent = item->parent(); parent; parent = parent->parent())
        parent->setExpanded(true);

    m_tree->setCurrentItem(item);
    m_tree->scrollToItem(item);
    showTab(QStringLiteral("elements"));
}

void DevToolsPanel::showTab(const QString &name)
{
    static const QHash<QString, int> kTabs = {
        {QStringLiteral("console"), 0},
        {QStringLiteral("elements"), 1},
        {QStringLiteral("layout"), 2},
        {QStringLiteral("network"), 3},
    };

    if (const auto it = kTabs.constFind(name); it != kTabs.constEnd())
        m_tabs->setCurrentIndex(it.value());
}

void DevToolsPanel::onTreeSelectionChanged()
{
    QTreeWidgetItem *item = m_tree->currentItem();
    if (!item) {
        m_selected = nullptr;
        m_styleView->clear();
        m_rulesView->clear();
        return;
    }

    const auto *element = static_cast<const dom::Element *>(
        item->data(0, Qt::UserRole).value<const void *>());
    m_selected = element;

    browser::Page *current = page();
    if (!element || !current || !current->styles()) {
        m_styleView->clear();
        m_rulesView->clear();
        return;
    }

    m_elementLabel->setText(elementLabel(element));
    m_styleView->setPlainText(
        devtools::Inspector::computedStyles(current->styles(), element));
    m_rulesView->setPlainText(
        devtools::Inspector::appliedRules(current->styles(), element));
}

void DevToolsPanel::onPickElement()
{
    if (!m_view) {
        emit statusMessage(QStringLiteral("No page view to pick from"));
        return;
    }

    // The picker is armed on the view and the button is disabled until a click
    // arrives, so a second press cannot arm it twice.
    m_view->setPickingElement(true);
    showTab(QStringLiteral("elements"));
    emit statusMessage(QStringLiteral("Click an element in the page to inspect it"));
}

} // namespace oqb::ui
