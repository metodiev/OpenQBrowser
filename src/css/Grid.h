#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVector>

namespace oqb::css {

/// One track (`<track-size>`) from a `grid-template-columns` / `-rows` list.
///
/// A track is either intrinsically sized or flexible. The distinction is not
/// just arithmetic: flexible tracks absorb the free space the intrinsic ones
/// leave, so sizing is a two-step process — intrinsic first, then the fractional
/// share of whatever remains.
struct GridTrack
{
    enum class Kind {
        /// A `<length>` or `<percentage>`: a definite size once the grid's
        /// available space is known.
        Fixed,
        /// `auto`. Sized to its content, but may be stretched by `justify-content`
        /// or `align-content` when the container is larger than the tracks.
        Auto,
        /// `min-content`: the smallest size the content can take without
        /// overflowing — the widest unbreakable word.
        MinContent,
        /// `max-content`: the size the content would take with unlimited room.
        MaxContent,
        /// `<flex>`: a share of the space left after the other tracks are sized.
        Fraction,
        /// `fit-content(<length>)`: content-sized, clamped to the argument.
        FitContent,
    };

    Kind kind = Kind::Auto;
    /// Pixels for Fixed and FitContent; the flex factor for Fraction.
    double value = 0;
    /// True when the size was written as a percentage, which resolves against
    /// the grid's available space rather than being an absolute length.
    bool isPercentage = false;
    /// The name given by `[name]` before the track, for `grid-column: name`.
    QString name;

    /// The resolved size, filled in by the sizing pass.
    double size = 0;
    /// The size the track's content wants, filled in by the measurement pass.
    double contentMin = 0;
    double contentMax = 0;

    bool isFlexible() const { return kind == Kind::Fraction; }
    /// True when the track has a size that does not depend on its content.
    bool isDefinite() const
    {
        return kind == Kind::Fixed || kind == Kind::FitContent;
    }
};

/// A parsed `grid-template-columns` / `-rows` value.
struct GridTrackList
{
    QVector<GridTrack> tracks;
    /// The names that appear in `[name]` line name lists, in order, each
    /// associated with the line index it precedes. A name may repeat, which is
    /// what makes `grid-column: content` span several tracks.
    QHash<QString, QList<int>> lineNames;

    bool isEmpty() const { return tracks.isEmpty(); }
    int size() const { return tracks.size(); }

    /// The line name at `line`, if any. Line 1 is the first.
    void addLineName(int line, const QString &name);
};

/// Resolves every track's size against the space available.
///
/// The order matters and follows the specification's intent: definite tracks
/// take their size first, then content-sized tracks take what their contents
/// need, and flexible tracks divide whatever is left in proportion to their
/// factors. Doing it in the other order would let an `fr` track consume space a
/// fixed track had already claimed.
///
/// `gap` is the space between tracks, which is subtracted up front: it is not
/// available to any track. `contentSizes` supplies each track's measured
/// `min`/`max` content contribution, indexed by track. When the tracks do not
/// fit, they are scaled down proportionally rather than overflowing, which is
/// what keeps a grid inside its container.
void resolveTrackSizes(QVector<GridTrack> *tracks, double availableSpace, double gap);

/// The total of the tracks plus the gaps between them.
double totalTrackSize(const QVector<GridTrack> &tracks, double gap);


/// One `<track-size>` with its optional preceding line names.
struct GridTrackEntry
{
    GridTrack track;
    QStringList namesBefore;
};

/// Parses a track list such as
/// `[full-start] minmax(1em, 1fr) [full-end]`.
///
/// Supports `<length>`, `<percentage>`, `<flex>`, `auto`, `min-content`,
/// `max-content`, `fit-content(<length>)`, `minmax(<min>, <max>)` and
/// `repeat(<count>, <list>)`. A construct that is not understood is skipped
/// rather than aborting the list, because a browser that drops a whole
/// declaration over one unsupported function lays the page out as a single
/// column, which looks far more broken than ignoring the one part.
///
/// `rootFontSize` and `fontSize` resolve absolute units; `availableWidth` is the
/// base a percentage resolves against.
QVector<GridTrackEntry> parseTrackList(const QString &value, double fontSize,
                                       double rootFontSize, double availableWidth,
                                       GridTrackList *out);

/// Parsed `grid-column` / `grid-row` / `grid-area` placement.
///
/// A placement names a start and an end line: `2 / 4`, `span 2`, `-1`, or a name.
/// `auto` means the item is placed by the auto-placement algorithm.
struct GridPlacement
{
    /// 0 means auto. A positive value is a line counted from the start; a
    /// negative value is counted back from the end, so -1 is the last line.
    int startLine = 0;
    int endLine = 0;
    /// A span on each axis, used when the line is auto but a span is given.
    int startSpan = 0;
    int endSpan = 0;
    /// A named line, empty when the placement is numeric.
    QString startName;
    QString endName;
    /// True when the axis is `auto`.
    ///
    /// A bare `span 2` counts as auto for placement purposes: it says how many
    /// tracks to occupy, not where to begin, so the auto-placement algorithm
    /// still chooses the position. Only a named or numbered start line anchors
    /// the item.
    bool isAuto() const
    {
        return startLine == 0 && endLine == 0 && startName.isEmpty() && endName.isEmpty();
    }
};

/// Parses one axis of a placement: `grid-column`, `grid-row`, or one half of
/// `grid-area`.
GridPlacement parsePlacement(const QString &value);

/// Splits a `grid-area` shorthand into its four parts
/// (`row-start / column-start / row-end / column-end`), filling in the ones the
/// author omitted as the specification requires.
QStringList splitGridArea(const QString &value);

} // namespace oqb::css
