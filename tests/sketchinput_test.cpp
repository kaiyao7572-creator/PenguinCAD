// Typed input while drawing: the state machine and the maths behind
// Fusion's on-canvas value boxes.
//
// Every expected number here is hand-computed in the comment beside it.
// Nothing is read back out of the implementation.

#include "core/Units.h"
#include "sketch/SketchAnnotations.h"
#include "sketch/SketchFeature.h"
#include "sketch/SketchInput.h"

#include <cmath>
#include <iostream>
#include <string>

using namespace lcad;

static int failures = 0;

static void check(bool theOk, const std::string& theWhat)
{
    std::cout << (theOk ? "  PASS  " : "  FAIL  ") << theWhat << std::endl;
    if (!theOk) {
        ++failures;
    }
}

static void checkNear(double theGot, double theWant, const std::string& theWhat,
                      double theTolerance = 1.0e-9)
{
    const bool ok = std::fabs(theGot - theWant) <= theTolerance;
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << theWhat << ": got " << theGot << ", expected "
              << theWant << std::endl;
    if (!ok) {
        ++failures;
    }
}

static void checkText(const std::string& theGot, const std::string& theWant,
                      const std::string& theWhat)
{
    const bool ok = theGot == theWant;
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << theWhat << ": got \"" << theGot
              << "\", expected \"" << theWant << "\"" << std::endl;
    if (!ok) {
        ++failures;
    }
}

static void checkPoint(const gp_Pnt2d& theGot, double theX, double theY,
                       const std::string& theWhat, double theTolerance = 1.0e-9)
{
    const bool ok = std::fabs(theGot.X() - theX) <= theTolerance
                 && std::fabs(theGot.Y() - theY) <= theTolerance;
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << theWhat << ": got (" << theGot.X() << ", "
              << theGot.Y() << "), expected (" << theX << ", " << theY << ")" << std::endl;
    if (!ok) {
        ++failures;
    }
}

// The two boxes a line tool offers.
static std::vector<SketchInputField> LineFields()
{
    SketchInputField length;
    length.name = SketchInputFields::Length;
    length.kind = UnitKind::Length;

    SketchInputField angle;
    angle.name = SketchInputFields::Angle;
    angle.kind = UnitKind::Angle;

    return {length, angle};
}

static std::vector<SketchInputField> BoxFields()
{
    SketchInputField width;
    width.name = SketchInputFields::Width;
    width.kind = UnitKind::Length;

    SketchInputField height;
    height.name = SketchInputFields::Height;
    height.kind = UnitKind::Length;

    return {width, height};
}

// Type a whole string, one character at a time, the way the keyboard
// delivers it.
static void typeText(SketchInput& theInput, const std::string& theText)
{
    for (char character : theText) {
        theInput.TypeCharacter(character);
    }
}

int main()
{
    std::cout << "-- typing only starts on a number --" << std::endl;
    {
        SketchInput input;
        input.Begin(LineFields());
        check(!input.TypeCharacter('m'), "a bare letter is not ours to consume");
        check(!input.TypeCharacter('['), "a bracket stays available as a tool shortcut");
        check(input.TypeCharacter('2'), "a digit starts capture");
        check(input.TypeCharacter('5'), "a second digit extends it");
        check(input.TypeCharacter('m'), "a letter mid-number belongs to the unit");
        checkText(input.TypedText(), "25m", "what has been typed so far");
        check(input.Backspace() && input.TypedText() == "25", "backspace edits");
    }

    std::cout << "-- Enter applies the value exactly --" << std::endl;
    {
        SketchInput input;
        input.Begin(LineFields());
        typeText(input, "25");
        check(input.Commit() == SketchInputResult::Accepted, "\"25\" is accepted");
        check(input.IsLocked(SketchInputFields::Length), "the length is now locked");
        checkNear(input.Value(SketchInputFields::Length), 25.0, "25 mm");
        check(input.TypedText().empty(), "the box is cleared once banked");
    }

    std::cout << "-- a locked value survives the mouse --" << std::endl;
    {
        SketchInput input;
        input.Begin(LineFields());
        typeText(input, "25");
        input.Commit();

        // This is what a mouse move does, and it must not land.
        input.MeasureFromAnchor(gp_Pnt2d(0.0, 0.0), gp_Pnt2d(80.0, 12.0));
        checkNear(input.Value(SketchInputFields::Length), 25.0, "still exactly 25 mm");

        // Angle was NOT locked, so it does follow the cursor:
        // atan2(12, 80) = 8.530765609948133 degrees.
        checkNear(input.Value(SketchInputFields::Angle), 8.530765609948133,
                  "the free angle still follows the cursor");

        // Cursor at 45 degrees, any distance: the endpoint sits at
        // 25 * cos45 = 25 * 0.7071067811865476 = 17.67766952966369 in both
        // axes.
        checkPoint(input.ResolvePoint(gp_Pnt2d(0.0, 0.0), gp_Pnt2d(100.0, 100.0)),
                   17.67766952966369, 17.67766952966369, "swings at exactly 25 mm");

        // ... and the same length whatever the cursor does.
        const gp_Pnt2d anchor(3.0, -7.0);
        for (double x = -50.0; x <= 50.0; x += 25.0) {
            const gp_Pnt2d point = input.ResolvePoint(anchor, gp_Pnt2d(x, 40.0));
            checkNear(anchor.Distance(point), 25.0, "length holds as the cursor sweeps");
        }
    }

    std::cout << "-- a typed angle pins the direction --" << std::endl;
    {
        SketchInput input;
        input.Begin(LineFields());
        input.NextField();  // Length -> Angle
        typeText(input, "90");
        check(input.Commit() == SketchInputResult::Accepted, "\"90\" into the angle box");

        // Angle only: the point runs as far along the ray as the cursor
        // has got. Cursor (5, 40) projected onto (0, 1) is 40.
        checkPoint(input.ResolvePoint(gp_Pnt2d(0.0, 0.0), gp_Pnt2d(5.0, 40.0)), 0.0, 40.0,
                   "projects onto the locked ray");

        // A cursor behind the anchor gives zero rather than a line flipped
        // through 180 degrees.
        checkPoint(input.ResolvePoint(gp_Pnt2d(0.0, 0.0), gp_Pnt2d(5.0, -40.0)), 0.0, 0.0,
                   "never flips the typed angle");
    }

    std::cout << "-- length AND angle pin the point completely --" << std::endl;
    {
        SketchInput input;
        input.Begin(LineFields());
        typeText(input, "25");
        input.NextField();  // banks 25 into Length and moves to Angle
        check(input.IsLocked(SketchInputFields::Length), "Tab banks the typed value");
        check(input.ActiveField() == 1, "Tab moved to the angle box");
        typeText(input, "30");
        input.Commit();

        // 25 at 30 degrees: (25 * 0.8660254037844387, 25 * 0.5)
        //               = (21.650635094610966, 12.5)
        checkPoint(input.ResolvePoint(gp_Pnt2d(0.0, 0.0), gp_Pnt2d(-999.0, -999.0)),
                   21.650635094610966, 12.5, "the cursor has no say left");
    }

    std::cout << "-- Tab cycles --" << std::endl;
    {
        SketchInput input;
        input.Begin(LineFields());
        check(input.ActiveField() == 0, "starts in the first box");
        input.NextField();
        check(input.ActiveField() == 1, "Tab: length -> angle");
        input.NextField();
        check(input.ActiveField() == 0, "Tab: angle -> length");
    }

    std::cout << "-- Escape clears, then releases --" << std::endl;
    {
        SketchInput input;
        input.Begin(LineFields());
        typeText(input, "12");
        check(input.ClearTyping(), "Escape consumed the half-typed value");
        check(input.TypedText().empty(), "and cleared it");
        check(!input.ClearTyping(), "a second Escape is the tool's to handle");

        typeText(input, "25");
        input.Commit();
        check(input.ClearTyping(), "Escape releases the lock");
        check(!input.IsLocked(SketchInputFields::Length), "the length follows the mouse again");
    }

    std::cout << "-- the unit parser does the reading --" << std::endl;
    {
        SketchInput input;
        input.Begin(LineFields());
        typeText(input, "1in");
        check(input.Commit() == SketchInputResult::Accepted, "\"1in\" is accepted");
        checkNear(input.Value(SketchInputFields::Length), 25.4, "1 inch is 25.4 mm");
        checkText(input.FieldText(0), "1 in", "and the box adopts inches");

        input.Begin(LineFields());
        typeText(input, "1'6\"");
        input.Commit();
        // 1 foot + 6 inches = 18 in = 18 * 25.4 = 457.2 mm
        checkNear(input.Value(SketchInputFields::Length), 457.2, "1'6\" is 457.2 mm");

        input.Begin(LineFields());
        input.NextField();
        typeText(input, "0.5rad");
        input.Commit();
        // 0.5 rad = 0.5 * 180 / pi = 28.64788975654116 degrees
        checkNear(input.Value(SketchInputFields::Angle), 28.64788975654116, "0.5 rad in degrees");
    }

    std::cout << "-- rubbish is refused and changes nothing --" << std::endl;
    {
        SketchInput input;
        input.Begin(LineFields());
        typeText(input, "25");
        input.Commit();

        typeText(input, "12xy");
        check(input.Commit() == SketchInputResult::Rejected, "\"12xy\" is refused");
        checkNear(input.Value(SketchInputFields::Length), 25.0, "the previous value stands");
        check(input.IsLocked(SketchInputFields::Length), "and is still locked");
        checkText(input.TypedText(), "12xy", "the bad text is left to be fixed");

        // Tab must not carry a value that does not parse into the next box.
        check(input.NextField() && input.ActiveField() == 0, "Tab holds on a bad value");
    }

    std::cout << "-- measuring from the anchor --" << std::endl;
    {
        // 3-4-5 triangle: length 5, angle atan2(4,3) = 53.13010235415598 deg
        checkNear(SketchInputLength(gp_Pnt2d(0.0, 0.0), gp_Pnt2d(3.0, 4.0)), 5.0, "3-4-5 length");
        checkNear(SketchInputAngle(gp_Pnt2d(0.0, 0.0), gp_Pnt2d(3.0, 4.0)), 53.13010235415598,
                  "3-4-5 angle");
        checkNear(SketchInputAngle(gp_Pnt2d(0.0, 0.0), gp_Pnt2d(-1.0, 0.0)), 180.0, "due west");
        checkNear(SketchInputAngle(gp_Pnt2d(0.0, 0.0), gp_Pnt2d(0.0, -1.0)), 270.0,
                  "due south normalises into [0,360)");
        checkNear(SketchInputAngle(gp_Pnt2d(2.0, 2.0), gp_Pnt2d(2.0, 2.0)), 0.0,
                  "a cursor on the anchor names no direction");

        // Nothing locked: the cursor comes straight back.
        checkPoint(SketchInputPoint(gp_Pnt2d(1.0, 1.0), gp_Pnt2d(9.0, -3.0), false, 0.0, false,
                                    0.0),
                   9.0, -3.0, "an untouched cursor is untouched");
    }

    std::cout << "-- rectangles: width and height --" << std::endl;
    {
        SketchInput input;
        input.Begin(BoxFields());
        typeText(input, "40");
        input.Commit();

        // Cursor down-left of the corner: a typed 40 keeps the rectangle
        // on the side the cursor is, so the corner lands at x = -40.
        checkPoint(input.ResolveCorner(gp_Pnt2d(0.0, 0.0), gp_Pnt2d(-30.0, 50.0)), -40.0, 50.0,
                   "typed width keeps the cursor's side");

        input.NextField();
        typeText(input, "20");
        input.Commit();
        checkPoint(input.ResolveCorner(gp_Pnt2d(0.0, 0.0), gp_Pnt2d(-30.0, 50.0)), -40.0, 20.0,
                   "both extents typed");
        checkPoint(input.ResolveCorner(gp_Pnt2d(0.0, 0.0), gp_Pnt2d(-30.0, -50.0)), -40.0, -20.0,
                   "and both follow the quadrant the cursor is in");

        // Measuring: a corner-to-corner drag reads off the deltas...
        SketchInput corner;
        corner.Begin(BoxFields());
        corner.MeasureBox(gp_Pnt2d(0.0, 0.0), gp_Pnt2d(-30.0, 50.0));
        checkNear(corner.Value(SketchInputFields::Width), 30.0, "width from a corner drag");
        checkNear(corner.Value(SketchInputFields::Height), 50.0, "height from a corner drag");

        // ... while a centre rectangle's cursor reaches only halfway.
        SketchInput centre;
        centre.Begin(BoxFields());
        centre.MeasureBox(gp_Pnt2d(0.0, 0.0), gp_Pnt2d(10.0, 5.0), true);
        checkNear(centre.Value(SketchInputFields::Width), 20.0, "centre rectangle is twice out");
        checkNear(centre.Value(SketchInputFields::Height), 10.0, "in both axes");
    }

    std::cout << "-- arming a new stage drops the locks --" << std::endl;
    {
        SketchInput input;
        input.Begin(LineFields());
        typeText(input, "25");
        input.Commit();
        check(input.AnyLocked(), "locked while the segment is being drawn");

        input.Begin(LineFields());
        check(!input.AnyLocked(), "the next segment starts free");
        check(input.ActiveField() == 0, "and back in the first box");

        input.End();
        check(!input.IsActive(), "and a tool with nothing to measure takes no keys");
        check(!input.TypeCharacter('2'), "a digit is not consumed when no box is up");
    }

    std::cout << "-- the readout --" << std::endl;
    {
        SketchInput input;
        input.Begin(LineFields());
        input.MeasureFromAnchor(gp_Pnt2d(0.0, 0.0), gp_Pnt2d(25.4, 0.0));
        const std::vector<std::string> lines = input.ReadoutLines();
        check(lines.size() == 2, "one line per box");
        // Padded either side: the plate drawn behind a box is sized to the
        // glyphs exactly, so unpadded text touches its own border.
        checkText(lines[0], " Length  25.4 mm ", "a box reads as name then value");
        checkText(lines[1], " Angle  0 deg ", "and the angle reads in degrees");

        // Which box is active is carried as state, not as a character in
        // the text: the box itself is highlighted, the way Fusion says it.
        check(input.ActiveField() == 0, "the first box is the active one");
        check(input.SetActiveField(1) && input.ActiveField() == 1, "and it can move");
        check(input.SetActiveField(0), "back again");

        typeText(input, "3");
        checkText(input.ReadoutLines()[0], " Length  3 ", "what is being typed shows as typed");
        input.Commit();
        checkText(input.ReadoutLines()[0], " Length = 3 mm ", "a locked box reads as driven");

        // Precision: a quarter of an inch in mm is 6.35, not 6.350000.
        input.Begin(LineFields());
        input.MeasureFromAnchor(gp_Pnt2d(0.0, 0.0), gp_Pnt2d(6.35, 0.0));
        checkText(input.FieldText(0), "6.35 mm", "trailing zeros are trimmed");
    }

    std::cout << "-- how a dimension reads once it is on the sketch --" << std::endl;
    {
        SketchFeature sketch;
        const int line = sketch.AddEntity(
            SketchEntity::MakeLine(gp_Pnt2d(0.0, 0.0), gp_Pnt2d(25.4, 0.0)));
        const int circle = sketch.AddEntity(SketchEntity::MakeCircle(gp_Pnt2d(0.0, 40.0), 6.0));
        const int second =
            sketch.AddEntity(SketchEntity::MakeLine(gp_Pnt2d(0.0, 0.0), gp_Pnt2d(0.0, 10.0)));

        SketchConstraint length;
        length.type = SketchConstraintType::Distance;
        length.a = SketchPointRef{line, SketchPointRole::Start};
        length.b = SketchPointRef{line, SketchPointRole::End};
        length.value = 25.4;
        length.labelPosition = gp_Pnt2d(12.7, 5.0);
        sketch.AddConstraint(length);

        SketchConstraint diameter;
        diameter.type = SketchConstraintType::Diameter;
        diameter.a = SketchPointRef{circle, SketchPointRole::Whole};
        diameter.value = 12.0;
        diameter.labelPosition = gp_Pnt2d(10.0, 50.0);
        sketch.AddConstraint(diameter);

        SketchConstraint angle;
        angle.type = SketchConstraintType::Angle;
        angle.a = SketchPointRef{line, SketchPointRole::Whole};
        angle.b = SketchPointRef{second, SketchPointRole::Whole};
        angle.value = 3.14159265358979323846 / 4.0;  // 45 degrees, stored in radians
        angle.labelPosition = gp_Pnt2d(6.0, 6.0);
        sketch.AddConstraint(angle);

        const std::vector<SketchConstraint>& added = sketch.Constraints();
        check(added.size() == 3, "three dimensions on the sketch");
        checkText(added[0].label, "d1", "dimensions number from d1");
        checkText(added[1].label, "d2", "and count up");
        checkText(added[2].label, "d3", "in the order they were added");

        checkText(SketchAnnotations::FormatDimension(added[0]), "25.4 mm",
                  "a length reads in the document's unit, zeros trimmed");
        checkText(SketchAnnotations::FormatDimension(added[1]), "D12 mm",
                  "a diameter says so");
        checkText(SketchAnnotations::FormatDimension(added[2]), "45 deg",
                  "an angle reads in degrees, not radians");

        // The number is lifted clear of the dimension line drawn through
        // the spot it was dropped on. The span runs along +X, so the
        // perpendicular is +Y and the label sits one glyph above where it
        // was placed: 5.0 + 1.2 * 2.0 = 7.4.
        const std::vector<SketchAnnotations::Label> labels =
            SketchAnnotations::DimensionLabels(sketch, 2.0);
        check(labels.size() == 3, "one number per dimension");
        checkNear(labels[0].position.X(), 12.7, "lifted straight up, not sideways");
        checkNear(labels[0].position.Y(), 7.4, "and clear of its own dimension line");
    }

    std::cout << std::endl;
    if (failures == 0) {
        std::cout << "All sketch input assertions passed." << std::endl;
    } else {
        std::cout << failures << " assertion(s) FAILED." << std::endl;
    }
    return failures == 0 ? 0 : 1;
}
