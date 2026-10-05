#pragma once

#include <QString>

namespace oqb::css {
class StyleEngine;
}
namespace oqb::dom {
class Document;
class Element;
}
namespace oqb::renderer {
class Box;
struct LayoutResult;
}
namespace oqb::javascript {
class ScriptEngine;
}
namespace oqb::network {
struct Resource;
}

namespace oqb::devtools {

/// Builds the developer tools views from a loaded page.
///
/// The inspector is a pure function of the browser's state: it takes the
/// document, the computed styles, the box tree and the network log, and returns
/// text. That keeps the tool usable from the command line as well as from the
/// graphical window, and means it never has to reach into the browser to find
/// out what happened.
class Inspector
{
public:
    /// The document tree, one node per line, nested by indentation.
    static QString domTree(const dom::Document *document);

    /// The computed style of one element, as a declaration list.
    static QString computedStyles(const css::StyleEngine *engine, const dom::Element *element);

    /// The declarations that applied to an element, in cascade order, with the
    /// selector each came from. This is the view that makes the cascade legible.
    static QString appliedRules(const css::StyleEngine *engine, const dom::Element *element);

    /// The box tree with each box's geometry.
    static QString boxTree(const renderer::Box *root);
    /// The same tree as "x y width height" lines, for comparing two layouts.
    static QString geometry(const renderer::Box *root);

    /// A summary of the layout: document size, line count and any warnings.
    static QString layoutSummary(const renderer::LayoutResult &result);

    /// The scripts the page declared and what happened to each.
    static QString scriptSummary(const javascript::ScriptEngine *engine,
                                 const dom::Document *document);

    /// The external resources the page references, and whether each loaded.
    static QString resourceSummary(const dom::Document *document);

    /// Everything above, as one report. This is what the command line tool
    /// prints and what a future DevTools window would show in its tabs.
    static QString fullReport(const dom::Document *document, const css::StyleEngine *engine,
                              const renderer::Box *root,
                              const renderer::LayoutResult &layout,
                              const javascript::ScriptEngine *scripts);

    /// A one-line description of a page for the window title and history.
    static QString pageSummary(const dom::Document *document, const renderer::Box *root);
};

} // namespace oqb::devtools
