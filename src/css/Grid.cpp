#include "css/Grid.h"

#include <QRegularExpression>

#include "css/Value.h"

namespace oqb::css {

void GridTrackList::addLineName(int line, const QString &name)
{
    if (name.isEmpty() || line < 1)
        return;
    QList<int> &lines = lineNames[name];
    if (!lines.contains(line))
        lines.append(line);
}

namespace {

/// Splits on top-level whitespace, keeping bracketed groups and function
/// arguments together. `repeat(2, 1fr)` and `[a b]` must survive as single
/// tokens: splitting on every space would break the first into three pieces and
/// the second into two.
QStringList splitTopLevel(const QString &value)
{
    QStringList out;
    QString current;
    int depth = 0;
    bool inBracket = false;

    for (const QChar &c : value) {
        if (c == u'(') {
            ++depth;
        } else if (c == u')') {
            depth = qMax(0, depth - 1);
        } else if (c == u'[') {
            inBracket = true;
        } else if (c == u']') {
            inBracket = false;
        }

        if (depth == 0 && !inBracket && c.isSpace()) {
            if (!current.isEmpty()) {
                out.append(current);
                current.clear();
            }
            continue;
        }
        current.append(c);
    }
    if (!current.isEmpty())
        out.append(current);
    return out;
}

/// The text inside the outermost parentheses of `name(...)`, or empty when the
/// token is not a call to `name`.
QString functionBody(const QString &token, const QString &name)
{
    const QString prefix = name + u'(';
    if (!token.startsWith(prefix, Qt::CaseInsensitive) || !token.endsWith(u')'))
        return {};
    return token.mid(prefix.size(), token.size() - prefix.size() - 1);
}

/// Splits a function's arguments on top-level commas, so `minmax(1em, 1fr)`
/// yields two parts and `minmax(1em, min-content)` still does.
QStringList splitArguments(const QString &body)
{
    QStringList out;
    QString current;
    int depth = 0;
    for (const QChar &c : body) {
        if (c == u'(')
            ++depth;
        else if (c == u')')
            depth = qMax(0, depth - 1);

        if (c == u',' && depth == 0) {
            out.append(current.trimmed());
            current.clear();
            continue;
        }
        current.append(c);
    }
    out.append(current.trimmed());
    return out;
}

/// Resolves a `<length>` token to pixels, or returns false when it is not one.
/// `isPercentage` is set for a percentage, whose base is not known yet.
bool resolveToken(const QString &token, double fontSize, double rootFontSize,
                  double availableWidth, double *pixels, bool *isPercentage)
{
    *isPercentage = false;
    const Value value = values::parseComponentValue(token);
    if (!value.isValid())
        return false;

    if (value.isPercentage()) {
        *isPercentage = true;
        *pixels = availableWidth * value.number / 100.0;
        return true;
    }

    if (value.isNumber()) {
        // A bare zero is a valid length; any other bare number is not.
        if (qFuzzyIsNull(value.number)) {
            *pixels = 0;
            return true;
        }
        return false;
    }

    if (!value.isLength())
        return false;

    double resolved = 0;
    if (!values::lengthToPixels(value.number, value.unit, fontSize, rootFontSize, availableWidth,
                                0, &resolved)) {
        return false;
    }
    *pixels = resolved;
    return true;
}

/// True when `token` is a `<flex>` such as `1fr` or `2.5fr`.
bool parseFlex(const QString &token, double *factor)
{
    if (!token.endsWith(QLatin1String("fr"), Qt::CaseInsensitive))
        return false;
    bool ok = false;
    const double value = token.left(token.size() - 2).trimmed().toDouble(&ok);
    if (!ok || value < 0)
        return false;
    *factor = value;
    return true;
}

/// Turns one `<track-size>` token into a track, or returns false when it is not
/// one this engine understands.
bool parseTrackSize(const QString &token, double fontSize, double rootFontSize,
                    double availableWidth, GridTrack *out)
{
    const QString trimmed = token.trimmed();
    if (trimmed.isEmpty())
        return false;

    if (trimmed.compare(QLatin1String("auto"), Qt::CaseInsensitive) == 0) {
        out->kind = GridTrack::Kind::Auto;
        return true;
    }
    if (trimmed.compare(QLatin1String("min-content"), Qt::CaseInsensitive) == 0) {
        out->kind = GridTrack::Kind::MinContent;
        return true;
    }
    if (trimmed.compare(QLatin1String("max-content"), Qt::CaseInsensitive) == 0) {
        out->kind = GridTrack::Kind::MaxContent;
        return true;
    }

    if (double factor = 0; parseFlex(trimmed, &factor)) {
        out->kind = GridTrack::Kind::Fraction;
        out->value = factor;
        return true;
    }

    // fit-content(<length>) is content-sized but never larger than its argument.
    if (const QString body = functionBody(trimmed, QStringLiteral("fit-content"));
        !body.isEmpty()) {
        double pixels = 0;
        bool percentage = false;
        if (!resolveToken(body, fontSize, rootFontSize, availableWidth, &pixels, &percentage))
            return false;
        out->kind = GridTrack::Kind::FitContent;
        out->value = pixels;
        out->isPercentage = percentage;
        return true;
    }

    // minmax(<min>, <max>) takes the larger of the two. Only the forms that
    // matter in practice are honoured: a flexible maximum with an intrinsic
    // minimum (`minmax(0, 1fr)`), and a fixed range. A min that is `auto` is
    // treated as min-content, which is what it means for a track holding text.
    if (const QString body = functionBody(trimmed, QStringLiteral("minmax")); !body.isEmpty()) {
        const QStringList parts = splitArguments(body);
        if (parts.size() != 2)
            return false;

        const QString &minText = parts.at(0);
        const QString &maxText = parts.at(1);

        // A flexible maximum wins: the track's floor is the minimum, and it
        // takes a share of the free space above that.
        if (double factor = 0; parseFlex(maxText, &factor)) {
            out->kind = GridTrack::Kind::Fraction;
            out->value = factor;
            if (minText.compare(QLatin1String("auto"), Qt::CaseInsensitive) == 0) {
                // `minmax(auto, 1fr)` floors at the content's minimum size.
                out->contentMin = -1; // sentinel: use the content minimum
            } else {
                double pixels = 0;
                bool percentage = false;
                if (resolveToken(minText, fontSize, rootFontSize, availableWidth, &pixels,
                                 &percentage)) {
                    out->contentMin = pixels;
                } else if (minText.compare(QLatin1String("min-content"), Qt::CaseInsensitive) == 0
                           || minText.compare(QLatin1String("max-content"),
                                              Qt::CaseInsensitive) == 0) {
                    out->contentMin = -1;
                }
            }
            return true;
        }

        // A fixed range: the maximum decides, and the minimum is clamped to it.
        double maxPixels = 0;
        bool maxPercentage = false;
        if (!resolveToken(maxText, fontSize, rootFontSize, availableWidth, &maxPixels,
                          &maxPercentage)) {
            return false;
        }
        double minPixels = 0;
        bool minPercentage = false;
        if (!resolveToken(minText, fontSize, rootFontSize, availableWidth, &minPixels,
                          &minPercentage)
            && minText.compare(QLatin1String("auto"), Qt::CaseInsensitive) != 0
            && minText.compare(QLatin1String("min-content"), Qt::CaseInsensitive) != 0) {
            return false;
        }

        out->kind = GridTrack::Kind::Fixed;
        out->value = qMax(minPixels, maxPixels);
        out->isPercentage = maxPercentage || minPercentage;
        return true;
    }

    double pixels = 0;
    bool percentage = false;
    if (!resolveToken(trimmed, fontSize, rootFontSize, availableWidth, &pixels, &percentage))
        return false;

    out->kind = GridTrack::Kind::Fixed;
    out->value = pixels;
    out->isPercentage = percentage;
    return true;
}

/// Appends `entry` to `out`, recording any `[name]` list it carried.
void appendEntry(QVector<GridTrackEntry> *out, const GridTrackEntry &entry, GridTrackList *list)
{
    // A name on the entry applies to the line *before* the track, which is the
    // line index equal to the number of tracks already present.
    for (const QString &name : entry.namesBefore)
        list->addLineName(list->tracks.size() + 1, name);
    list->tracks.append(entry.track);
    out->append(entry);
}

} // namespace

QVector<GridTrackEntry> parseTrackList(const QString &value, double fontSize,
                                       double rootFontSize, double availableWidth,
                                       GridTrackList *out)
{
    QVector<GridTrackEntry> entries;
    if (out)
        out->tracks.clear();

    // A `[name]` list names the line that follows it. Names accumulate here
    // until the next track is added, which is the line they belong to; anything
    // still pending at the end names the final line.
    QStringList pendingNames;

    const auto flushNames = [&](int line) {
        if (!out)
            return;
        for (const QString &name : pendingNames)
            out->addLineName(line, name);
    };

    const QStringList tokens = splitTopLevel(value);
    for (const QString &token : tokens) {
        if (token.startsWith(u'[') && token.endsWith(u']')) {
            const QStringList names = token.mid(1, token.size() - 2)
                                          .split(QRegularExpression(QStringLiteral("\\s+")),
                                                 Qt::SkipEmptyParts);
            pendingNames += names;
            continue;
        }

        // repeat(<count>, <list>) expands in place. `auto-fill` and `auto-fit`
        // need the container's size to know how many repetitions fit, which is
        // not known until layout runs; treating them as one repetition keeps the
        // grid usable rather than collapsing it to nothing.
        if (const QString body = functionBody(token, QStringLiteral("repeat")); !body.isEmpty()) {
            const QStringList parts = splitArguments(body);
            bool ok = false;
            const int count = parts.value(0).trimmed().toInt(&ok);
            if (parts.size() >= 2 && ok && count > 0) {
                const QString inner = QStringList(parts.mid(1)).join(QStringLiteral(", "));
                for (int i = 0; i < count; ++i) {
                    // The recursive call needs a destination of its own: it must
                    // not write names into `out` itself, because the line numbers
                    // it would record are relative to the repetition rather than
                    // to the whole template. Each repetition's names are shifted
                    // into place below.
                    GridTrackList repetition;
                    const QVector<GridTrackEntry> repeated
                        = parseTrackList(inner, fontSize, rootFontSize, availableWidth,
                                         &repetition);
                    const int offset = out ? out->tracks.size() : entries.size();
                    for (const GridTrackEntry &entry : repeated)
                        entries.append(entry);
                    if (out) {
                        // Carry the repetition's names, offset by the tracks
                        // already emitted, so `repeat(2, [col] 1fr)` names both
                        // of the lines rather than only the first.
                        for (auto it = repetition.lineNames.constBegin();
                             it != repetition.lineNames.constEnd(); ++it) {
                            for (int line : it.value())
                                out->addLineName(line + offset, it.key());
                        }
                        for (const GridTrackEntry &entry : repeated)
                            out->tracks.append(entry.track);
                    }
                }
            }
            continue;
        }

        GridTrack track;
        if (!parseTrackSize(token, fontSize, rootFontSize, availableWidth, &track))
            continue;

        GridTrackEntry entry;
        entry.track = track;
        entry.namesBefore = pendingNames;
        if (out)
            out->tracks.append(track);

        // The pending names belong to the line before this track, which is the
        // line index one more than the tracks already present.
        flushNames(entries.size() + 1);
        pendingNames.clear();
        entries.append(entry);
    }

    // A trailing `[name]` names the line after the last track.
    if (!pendingNames.isEmpty())
        flushNames((out ? out->tracks.size() : entries.size()) + 1);

    return entries;
}

// ---------------------------------------------------------------- placement

namespace {

/// One half of a placement: either a line number, a span, or a name.
struct PlacementPart
{
    /// >0 counts from the start, <0 counts back from the end, 0 is unset.
    int line = 0;
    int span = 0;
    QString name;
    bool isSpan = false;
};

/// Parses `span 2`, `span name`, `3`, `-1` or `content`.
PlacementPart parsePlacementPart(const QString &text)
{
    PlacementPart part;
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty() || trimmed.compare(QLatin1String("auto"), Qt::CaseInsensitive) == 0)
        return part;

    if (trimmed.startsWith(QLatin1String("span"), Qt::CaseInsensitive)) {
        part.isSpan = true;
        const QString rest = trimmed.mid(4).trimmed();
        bool ok = false;
        const int count = rest.toInt(&ok);
        if (ok && count > 0) {
            part.span = count;
        } else if (!rest.isEmpty()) {
            // `span name` spans to the next line with that name, which this
            // engine treats as a single-track span: resolving the name would
            // need the whole grid, which the parser does not have.
            part.span = 1;
            part.name = rest;
        } else {
            part.span = 1;
        }
        return part;
    }

    bool ok = false;
    const int line = trimmed.toInt(&ok);
    if (ok) {
        if (line != 0)
            part.line = line;
        return part;
    }

    part.name = trimmed;
    return part;
}

} // namespace

GridPlacement parsePlacement(const QString &value)
{
    GridPlacement placement;

    // A placement is one or two parts separated by a slash. With one part, the
    // span is implied to be one track - which is why `grid-column: 2` occupies a
    // single column starting at line 2.
    const QStringList parts = value.split(u'/', Qt::SkipEmptyParts);
    if (parts.isEmpty())
        return placement;

    const PlacementPart start = parsePlacementPart(parts.value(0));
    placement.startLine = start.line;
    placement.startName = start.name;
    placement.startSpan = start.isSpan ? start.span : 0;

    if (parts.size() >= 2) {
        const PlacementPart end = parsePlacementPart(parts.value(1));
        placement.endLine = end.line;
        placement.endName = end.name;
        placement.endSpan = end.isSpan ? end.span : 0;
    }

    return placement;
}

QStringList splitGridArea(const QString &value)
{
    // `grid-area` is up to four parts. The specification's rule for fewer than
    // four is not "the rest are auto" but "the missing ones mirror the ones
    // given": with two parts the row-end becomes row-start's span, and with
    // three the column-end mirrors the column-start. That is what makes
    // `grid-area: 1 / 1 / 3 / 3` and `grid-area: 1 / 1` behave differently from
    // a naive reading.
    QStringList parts = value.split(u'/', Qt::SkipEmptyParts);
    for (QString &part : parts)
        part = part.trimmed();

    if (parts.size() == 1) {
        // A single value names all four edges, which is how `grid-area: header`
        // or `grid-area: 2` places an item.
        parts << parts.value(0) << parts.value(0) << parts.value(0);
    } else if (parts.size() == 2) {
        // row-start / column-start: the row-end mirrors the column-start and the
        // column-end mirrors the row-start, which is the shorthand's quirk.
        parts << parts.value(1) << parts.value(0);
    } else if (parts.size() == 3) {
        parts << QString();
    }

    while (parts.size() < 4)
        parts << QString();
    return parts.mid(0, 4);
}

// -------------------------------------------------------------- track sizing

void resolveTrackSizes(QVector<GridTrack> *tracks, double availableSpace, double gap)
{
    if (!tracks || tracks->isEmpty())
        return;

    const int count = tracks->size();

    // The gaps are spoken for before any track is sized: they are space no track
    // can have. `count - 1` gaps run between the tracks, not `count`.
    const double gaps = gap * qMax(0, count - 1);
    const double space = qMax(0.0, availableSpace);

    // A track's content floor and ceiling. For a track with no measured content
    // both are zero, which is what makes an empty `auto` track collapse.
    const auto floorOf = [](const GridTrack &track) {
        // The sentinel a `minmax(auto, 1fr)` sets means "use the content
        // minimum", which is what an auto minimum means for a track.
        return track.contentMin < 0 ? qMax(0.0, track.contentMax) : qMax(0.0, track.contentMin);
    };
    const auto ceilingOf = [](const GridTrack &track) { return qMax(0.0, track.contentMax); };

    // ------------------------------------------------------- base sizes
    //
    // Every track gets the size it has without considering free space: a
    // definite track its length, a content-sized one the least its content can
    // take. Growth beyond that is the next step's business.
    for (GridTrack &track : *tracks) {
        switch (track.kind) {
        case GridTrack::Kind::Fixed:
            track.size = qMax(0.0, track.value);
            break;
        case GridTrack::Kind::FitContent:
            // Content-sized, but never past the argument.
            track.size = qMin(ceilingOf(track), qMax(0.0, track.value));
            track.size = qMax(track.size, floorOf(track));
            break;
        case GridTrack::Kind::MinContent:
            track.size = floorOf(track);
            break;
        case GridTrack::Kind::MaxContent:
        case GridTrack::Kind::Auto:
            track.size = floorOf(track);
            break;
        case GridTrack::Kind::Fraction:
            // A flexible track starts at its floor - zero for a plain `1fr`, or
            // the minimum of a `minmax(200px, 1fr)` - and takes its share later.
            track.size = floorOf(track);
            break;
        }
    }

    // ------------------------------------------------ flexible distribution
    //
    // CSS Grid §12.7.1 "Find the Size of an fr", implemented as written. The
    // subtlety is the restart rule: a flexible track whose floor is larger than
    // its share is taken out and treated as inflexible, and the rest are sized
    // again without it. Skipping that step gives a `minmax(200px, 1fr)` track
    // less than 200px whenever an equal share would have been smaller.
    QVector<int> flexible;
    for (int i = 0; i < count; ++i) {
        if (tracks->at(i).isFlexible() && tracks->at(i).value > 0)
            flexible.append(i);
    }

    if (!flexible.isEmpty()) {
        const auto inflexibleBase = [&]() {
            double total = 0;
            for (int i = 0; i < count; ++i) {
                if (!flexible.contains(i))
                    total += tracks->at(i).size;
            }
            return total;
        };

        double frSize = 0;
        while (!flexible.isEmpty()) {
            const double leftover = space - gaps - inflexibleBase();
            double factorSum = 0;
            for (int i : flexible)
                factorSum += tracks->at(i).value;
            // The factor sum is floored at 1, so a single `0.5fr` does not take
            // twice the space it should.
            factorSum = qMax(1.0, factorSum);

            frSize = qMax(0.0, leftover / factorSum);

            // Any track whose floor exceeds its share becomes inflexible, and
            // the algorithm restarts without it.
            QVector<int> restart;
            for (int i : flexible) {
                if (tracks->at(i).value * frSize < tracks->at(i).size)
                    restart.append(i);
            }
            if (restart.isEmpty())
                break;

            for (int i : restart) {
                flexible.removeAll(i);
                // Its floor stands as its size.
                tracks->data()[i].size = floorOf(tracks->data()[i]);
            }
            if (flexible.isEmpty())
                frSize = 0;
        }

        for (int i : flexible)
            tracks->data()[i].size = qMax(tracks->data()[i].size,
                                          tracks->at(i).value * frSize);
    }

    // --------------------------------------------------- content tracks grow
    //
    // An `auto` or `max-content` track grows towards its content's maximum from
    // whatever space the flexible tracks did not take. The order matters and is
    // the specification's: flexible tracks are expanded first (12.7), and only
    // then do auto tracks stretch (12.8). Doing it the other way round lets an
    // `auto` column swallow the space a `1fr` needed, so `200px auto 1fr` ends
    // up with an oversized middle column and a starved flexible one.
    {
        double used = 0;
        for (const GridTrack &track : *tracks)
            used += track.size;
        double free = space - gaps - used;

        double growable = 0;
        for (const GridTrack &track : *tracks) {
            if (track.kind != GridTrack::Kind::Auto && track.kind != GridTrack::Kind::MaxContent)
                continue;
            growable += qMax(0.0, ceilingOf(track) - track.size);
        }

        if (growable > 0 && free > 0) {
            const double growth = qMin(free, growable);
            for (GridTrack &track : *tracks) {
                if (track.kind != GridTrack::Kind::Auto
                    && track.kind != GridTrack::Kind::MaxContent) {
                    continue;
                }
                const double want = qMax(0.0, ceilingOf(track) - track.size);
                if (want > 0)
                    track.size += growth * (want / growable);
            }
        }
    }

    // No scaling of definite tracks to fit. A grid whose fixed columns exceed
    // its container overflows, exactly as a browser does: shrinking them would
    // silently produce a layout the author did not ask for, and would disagree
    // with every other engine.
}

double totalTrackSize(const QVector<GridTrack> &tracks, double gap)
{
    double total = 0;
    for (const GridTrack &track : tracks)
        total += track.size;
    if (tracks.size() > 1)
        total += gap * (tracks.size() - 1);
    return total;
}


} // namespace oqb::css
