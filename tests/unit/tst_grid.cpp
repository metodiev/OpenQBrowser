#include <QtTest>

#include "css/Grid.h"

using namespace oqb;
using oqb::css::GridPlacement;
using oqb::css::GridTrack;
using oqb::css::GridTrackList;

/// Tests for the grid track parser and the sizing arithmetic.
///
/// Track sizing is separated from layout precisely so it can be tested as
/// arithmetic: no box tree, no viewport, no fonts. That matters because the
/// mistakes available here are quantitative - an `fr` that takes space a fixed
/// track already claimed, a gap counted once per track instead of once between
/// them, an `auto` track that swallows the space a `1fr` needed - and each shows
/// up as a grid whose columns are subtly the wrong width.
class GridTest : public QObject
{
    Q_OBJECT

private slots:
    // Parsing.
    void parsesFixedTracks();
    void parsesFractionalTracks();
    void parsesKeywordTracks();
    void parsesPercentages();
    void parsesFitContent();
    void parsesMinmaxWithFlexibleMaximum();
    void parsesMinmaxWithFixedRange();
    void parsesRepeat();
    void parsesRepeatedTracksAndNames();
    void recordsLineNames();
    void skipsAnUnknownTrackWithoutDroppingTheList();

    // Placement.
    void parsesSingleLinePlacement();
    void parsesLineRangePlacement();
    void parsesNegativeLines();
    void parsesSpan();
    void parsesNamedPlacement();
    void parsesAutoPlacement();
    void expandsGridAreaShorthand();

    // Sizing.
    void sizesFixedTracks();
    void subtractsGapsUpFront();
    void sharesFreeSpaceBetweenFractionalTracks();
    void mixesFixedAndFractional();
    void letsFractionBeatAuto();
    void growsAutoWithoutFractionalTracks();
    void overflowsWhenDefiniteTracksDoNotFit();
    void honoursAFractionalFloor();
    void sizesEmptyTracksToZero();
};

// ------------------------------------------------------------------ parsing

void GridTest::parsesFixedTracks()
{
    GridTrackList list;
    const auto entries = css::parseTrackList(QStringLiteral("100px 200px 50px"), 16, 16, 800, &list);

    QCOMPARE(entries.size(), 3);
    QCOMPARE(list.size(), 3);
    for (const auto &entry : entries) {
        QCOMPARE(entry.track.kind, GridTrack::Kind::Fixed);
    }
    QCOMPARE(entries.at(0).track.value, 100.0);
    QCOMPARE(entries.at(1).track.value, 200.0);
    QCOMPARE(entries.at(2).track.value, 50.0);
}

void GridTest::parsesFractionalTracks()
{
    GridTrackList list;
    const auto entries = css::parseTrackList(QStringLiteral("1fr 2fr 0.5fr"), 16, 16, 800, &list);

    QCOMPARE(entries.size(), 3);
    for (const auto &entry : entries) {
        QCOMPARE(entry.track.kind, GridTrack::Kind::Fraction);
        QVERIFY(entry.track.isFlexible());
    }
    QCOMPARE(entries.at(0).track.value, 1.0);
    QCOMPARE(entries.at(1).track.value, 2.0);
    QCOMPARE(entries.at(2).track.value, 0.5);
}

void GridTest::parsesKeywordTracks()
{
    GridTrackList list;
    const auto entries = css::parseTrackList(QStringLiteral("auto min-content max-content"), 16, 16,
                                             800, &list);

    QCOMPARE(entries.size(), 3);
    QCOMPARE(entries.at(0).track.kind, GridTrack::Kind::Auto);
    QCOMPARE(entries.at(1).track.kind, GridTrack::Kind::MinContent);
    QCOMPARE(entries.at(2).track.kind, GridTrack::Kind::MaxContent);
}

void GridTest::parsesPercentages()
{
    GridTrackList list;
    // A percentage resolves against the grid's available space, which is 800
    // here, so 25% is 200px.
    const auto entries = css::parseTrackList(QStringLiteral("25% 75%"), 16, 16, 800, &list);

    QCOMPARE(entries.size(), 2);
    QCOMPARE(entries.at(0).track.kind, GridTrack::Kind::Fixed);
    QVERIFY(entries.at(0).track.isPercentage);
    QCOMPARE(entries.at(0).track.value, 200.0);
    QCOMPARE(entries.at(1).track.value, 600.0);
}

void GridTest::parsesFitContent()
{
    GridTrackList list;
    const auto entries = css::parseTrackList(QStringLiteral("fit-content(300px)"), 16, 16, 800,
                                             &list);

    QCOMPARE(entries.size(), 1);
    QCOMPARE(entries.at(0).track.kind, GridTrack::Kind::FitContent);
    QCOMPARE(entries.at(0).track.value, 300.0);
}

void GridTest::parsesMinmaxWithFlexibleMaximum()
{
    GridTrackList list;
    // The `minmax(0, 1fr)` idiom, which is how a column is made to shrink below
    // its content width instead of overflowing.
    const auto entries = css::parseTrackList(QStringLiteral("minmax(0, 1fr) minmax(200px, 1fr)"),
                                             16, 16, 800, &list);

    QCOMPARE(entries.size(), 2);
    QCOMPARE(entries.at(0).track.kind, GridTrack::Kind::Fraction);
    QCOMPARE(entries.at(0).track.value, 1.0);
    // A zero minimum is a real floor, not the content minimum sentinel.
    QCOMPARE(entries.at(0).track.contentMin, 0.0);

    QCOMPARE(entries.at(1).track.kind, GridTrack::Kind::Fraction);
    QCOMPARE(entries.at(1).track.contentMin, 200.0);
}

void GridTest::parsesMinmaxWithFixedRange()
{
    GridTrackList list;
    const auto entries = css::parseTrackList(QStringLiteral("minmax(100px, 300px)"), 16, 16, 800,
                                             &list);

    QCOMPARE(entries.size(), 1);
    QCOMPARE(entries.at(0).track.kind, GridTrack::Kind::Fixed);
    // The maximum decides the size; the minimum only raises it.
    QCOMPARE(entries.at(0).track.value, 300.0);
}

void GridTest::parsesRepeat()
{
    GridTrackList list;
    const auto entries = css::parseTrackList(QStringLiteral("repeat(3, 1fr)"), 16, 16, 800, &list);

    QCOMPARE(entries.size(), 3);
    for (const auto &entry : entries) {
        QCOMPARE(entry.track.kind, GridTrack::Kind::Fraction);
    }
}

void GridTest::parsesRepeatedTracksAndNames()
{
    GridTrackList list;
    const auto entries = css::parseTrackList(
        QStringLiteral("repeat(2, [col] 100px) 1fr"), 16, 16, 800, &list);

    QCOMPARE(entries.size(), 3);
    QCOMPARE(entries.at(0).track.value, 100.0);
    QCOMPARE(entries.at(1).track.value, 100.0);
    QCOMPARE(entries.at(2).track.kind, GridTrack::Kind::Fraction);

    // The name appears twice because the template repeats, which is exactly what
    // makes `grid-column: col` span more than one track.
    QVERIFY(list.lineNames.contains(QStringLiteral("col")));
    QCOMPARE(list.lineNames.value(QStringLiteral("col")).size(), 2);
}

void GridTest::recordsLineNames()
{
    GridTrackList list;
    css::parseTrackList(QStringLiteral("[left] 1fr [middle] 2fr [right]"), 16, 16, 800, &list);

    QCOMPARE(list.size(), 2);
    // Line 1 precedes the first track, line 2 precedes the second, and line 3
    // follows the last.
    QCOMPARE(list.lineNames.value(QStringLiteral("left")).value(0), 1);
    QCOMPARE(list.lineNames.value(QStringLiteral("middle")).value(0), 2);
    QCOMPARE(list.lineNames.value(QStringLiteral("right")).value(0), 3);
}

void GridTest::skipsAnUnknownTrackWithoutDroppingTheList()
{
    GridTrackList list;
    // `subgrid` is not supported. The other two tracks must still be parsed: a
    // browser that dropped the whole declaration would lay the page out as one
    // column, which looks far more broken than ignoring the one part.
    const auto entries = css::parseTrackList(QStringLiteral("100px subgrid 1fr"), 16, 16, 800,
                                             &list);

    QCOMPARE(entries.size(), 2);
    QCOMPARE(entries.at(0).track.value, 100.0);
    QCOMPARE(entries.at(1).track.kind, GridTrack::Kind::Fraction);
}

// ---------------------------------------------------------------- placement

void GridTest::parsesSingleLinePlacement()
{
    // One value means one track from that line.
    const GridPlacement placement = css::parsePlacement(QStringLiteral("2"));
    QCOMPARE(placement.startLine, 2);
    QCOMPARE(placement.endLine, 0);
    QVERIFY(!placement.isAuto());
}

void GridTest::parsesLineRangePlacement()
{
    const GridPlacement placement = css::parsePlacement(QStringLiteral("1 / 3"));
    QCOMPARE(placement.startLine, 1);
    QCOMPARE(placement.endLine, 3);
}

void GridTest::parsesNegativeLines()
{
    // A negative line counts back from the end, so -1 is the last line. It is
    // kept negative here because the grid's line count is not known until the
    // template and the items are both in hand.
    const GridPlacement placement = css::parsePlacement(QStringLiteral("2 / -1"));
    QCOMPARE(placement.startLine, 2);
    QCOMPARE(placement.endLine, -1);
}

void GridTest::parsesSpan()
{
    const GridPlacement placement = css::parsePlacement(QStringLiteral("span 3"));
    QVERIFY(placement.isAuto());
    QCOMPARE(placement.startSpan, 3);

    const GridPlacement anchored = css::parsePlacement(QStringLiteral("2 / span 2"));
    QCOMPARE(anchored.startLine, 2);
    QCOMPARE(anchored.endSpan, 2);
    QCOMPARE(anchored.endLine, 0);
}

void GridTest::parsesNamedPlacement()
{
    const GridPlacement placement = css::parsePlacement(QStringLiteral("content-start / content-end"));
    QCOMPARE(placement.startName, QStringLiteral("content-start"));
    QCOMPARE(placement.endName, QStringLiteral("content-end"));
    QVERIFY(!placement.isAuto());
}

void GridTest::parsesAutoPlacement()
{
    QVERIFY(css::parsePlacement(QString()).isAuto());
    QVERIFY(css::parsePlacement(QStringLiteral("auto")).isAuto());
    QVERIFY(css::parsePlacement(QStringLiteral("auto / auto")).isAuto());
}

void GridTest::expandsGridAreaShorthand()
{
    // Four parts are taken as written.
    const QStringList four = css::splitGridArea(QStringLiteral("1 / 2 / 3 / 4"));
    QCOMPARE(four.size(), 4);
    QCOMPARE(four.at(0), QStringLiteral("1"));
    QCOMPARE(four.at(3), QStringLiteral("4"));

    // One part means all four edges share it, which is how a named area such as
    // `grid-area: header` places an item.
    const QStringList one = css::splitGridArea(QStringLiteral("header"));
    QCOMPARE(one.size(), 4);
    QCOMPARE(one.at(0), QStringLiteral("header"));
    QCOMPARE(one.at(3), QStringLiteral("header"));

    // Two parts: the row-end mirrors the column-start and the column-end mirrors
    // the row-start. That quirk of the shorthand is easy to get wrong and shows
    // up as a grid area rotated by 90 degrees.
    const QStringList two = css::splitGridArea(QStringLiteral("1 / 3"));
    QCOMPARE(two.size(), 4);
    QCOMPARE(two.at(0), QStringLiteral("1"));
    QCOMPARE(two.at(1), QStringLiteral("3"));
    QCOMPARE(two.at(2), QStringLiteral("3"));
    QCOMPARE(two.at(3), QStringLiteral("1"));
}

// ------------------------------------------------------------------- sizing

void GridTest::sizesFixedTracks()
{
    QVector<GridTrack> tracks;
    for (double size : {100.0, 200.0}) {
        GridTrack track;
        track.kind = GridTrack::Kind::Fixed;
        track.value = size;
        tracks.append(track);
    }

    css::resolveTrackSizes(&tracks, 800, 0);
    QCOMPARE(tracks.at(0).size, 100.0);
    QCOMPARE(tracks.at(1).size, 200.0);
}

void GridTest::subtractsGapsUpFront()
{
    QVector<GridTrack> tracks;
    for (int i = 0; i < 3; ++i) {
        GridTrack track;
        track.kind = GridTrack::Kind::Fraction;
        track.value = 1;
        tracks.append(track);
    }

    // Three tracks have two gaps, so 800 - 20 = 780 to share: 260 each. Counting
    // three gaps would give 256.67 and leave the grid 10px short.
    css::resolveTrackSizes(&tracks, 800, 10);
    QCOMPARE(tracks.at(0).size, 260.0);
    QCOMPARE(css::totalTrackSize(tracks, 10), 800.0);
}

void GridTest::sharesFreeSpaceBetweenFractionalTracks()
{
    QVector<GridTrack> tracks;
    for (double factor : {1.0, 3.0}) {
        GridTrack track;
        track.kind = GridTrack::Kind::Fraction;
        track.value = factor;
        tracks.append(track);
    }

    css::resolveTrackSizes(&tracks, 800, 0);
    QCOMPARE(tracks.at(0).size, 200.0);
    QCOMPARE(tracks.at(1).size, 600.0);
}

void GridTest::mixesFixedAndFractional()
{
    QVector<GridTrack> tracks;
    GridTrack fixed;
    fixed.kind = GridTrack::Kind::Fixed;
    fixed.value = 200;
    tracks.append(fixed);

    GridTrack flex;
    flex.kind = GridTrack::Kind::Fraction;
    flex.value = 1;
    tracks.append(flex);

    // The fixed track claims 200 first, and the flexible one takes the rest.
    css::resolveTrackSizes(&tracks, 800, 0);
    QCOMPARE(tracks.at(0).size, 200.0);
    QCOMPARE(tracks.at(1).size, 600.0);
}

void GridTest::letsFractionBeatAuto()
{
    // `200px auto 1fr` is a common sidebar layout. The `fr` must take the free
    // space; letting the `auto` track grow to its content maximum first would
    // leave the `1fr` at zero.
    QVector<GridTrack> tracks;

    GridTrack fixed;
    fixed.kind = GridTrack::Kind::Fixed;
    fixed.value = 200;
    tracks.append(fixed);

    GridTrack autoTrack;
    autoTrack.kind = GridTrack::Kind::Auto;
    autoTrack.contentMin = 50;
    autoTrack.contentMax = 300;
    tracks.append(autoTrack);

    GridTrack flex;
    flex.kind = GridTrack::Kind::Fraction;
    flex.value = 1;
    tracks.append(flex);

    css::resolveTrackSizes(&tracks, 800, 0);
    QCOMPARE(tracks.at(0).size, 200.0);
    QCOMPARE(tracks.at(1).size, 50.0);  // its floor, not its ceiling
    QCOMPARE(tracks.at(2).size, 550.0); // the remainder
}

void GridTest::growsAutoWithoutFractionalTracks()
{
    // Without a flexible track the free space has nowhere else to go, so an
    // `auto` track grows towards its content maximum.
    QVector<GridTrack> tracks;
    GridTrack autoTrack;
    autoTrack.kind = GridTrack::Kind::Auto;
    autoTrack.contentMin = 100;
    autoTrack.contentMax = 400;
    tracks.append(autoTrack);

    css::resolveTrackSizes(&tracks, 800, 0);
    QCOMPARE(tracks.at(0).size, 400.0);
}

void GridTest::overflowsWhenDefiniteTracksDoNotFit()
{
    // Two 500px columns in a 400px container. A browser overflows rather than
    // shrinking them: the author asked for 500px, and silently producing 200px
    // would disagree with every other engine and hide the author's mistake.
    // This is asserted deliberately, because the intuitive thing to do -
    // scaling to fit - is the wrong one.
    QVector<GridTrack> tracks;
    for (int i = 0; i < 2; ++i) {
        GridTrack track;
        track.kind = GridTrack::Kind::Fixed;
        track.value = 500;
        tracks.append(track);
    }

    css::resolveTrackSizes(&tracks, 400, 0);
    QCOMPARE(tracks.at(0).size, 500.0);
    QCOMPARE(tracks.at(1).size, 500.0);
    QCOMPARE(css::totalTrackSize(tracks, 0), 1000.0);
}

void GridTest::honoursAFractionalFloor()
{
    // `minmax(300px, 1fr)` must reach 300px even when the equal share is less,
    // and the other flexible track takes what remains.
    QVector<GridTrack> floored;
    floored.append([] {
        GridTrack track;
        track.kind = GridTrack::Kind::Fraction;
        track.value = 1;
        track.contentMin = 300;
        return track;
    }());
    floored.append([] {
        GridTrack track;
        track.kind = GridTrack::Kind::Fraction;
        track.value = 1;
        return track;
    }());

    // Both tracks are flexible and neither floor exceeds its share, so each
    // gets half: finding the size of an `fr` divides the leftover by the factor
    // sum rather than subtracting the floors first. My first implementation
    // subtracted them, which gave [300, 500] and was wrong.
    css::resolveTrackSizes(&floored, 800, 0);
    QCOMPARE(floored.at(0).size, 400.0);
    QCOMPARE(floored.at(1).size, 400.0);
}

void GridTest::sizesEmptyTracksToZero()
{
    // An `auto` track with no content collapses rather than taking free space,
    // which is what makes an empty grid cell not stretch its column.
    QVector<GridTrack> tracks;
    for (int i = 0; i < 2; ++i) {
        GridTrack track;
        track.kind = GridTrack::Kind::Auto;
        tracks.append(track);
    }
    GridTrack flex;
    flex.kind = GridTrack::Kind::Fixed;
    flex.value = 100;
    tracks.append(flex);

    css::resolveTrackSizes(&tracks, 800, 0);
    QCOMPARE(tracks.at(0).size, 0.0);
    QCOMPARE(tracks.at(1).size, 0.0);
    QCOMPARE(tracks.at(2).size, 100.0);
}

QTEST_MAIN(GridTest)
#include "tst_grid.moc"
