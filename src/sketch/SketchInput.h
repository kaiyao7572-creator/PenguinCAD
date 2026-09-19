#pragma once

#include "core/Units.h"

#include <gp_Pnt2d.hxx>

#include <string>
#include <vector>

namespace lcad {

// Fusion's on-canvas value boxes: the live length/angle readout that
// follows the cursor while a create tool is collecting, and which you can
// type straight into.
//
// Deliberately free of Qt, AIS and Document, exactly like SketchGeometry:
// the state machine and the maths are the part that can be wrong in a way
// a headless test can catch, and the drawing is not.

// The names the tools use for their boxes. Values are looked up by name
// so a tool can offer any subset without the input code knowing what an
// arc is.
namespace SketchInputFields {
inline constexpr const char* Length   = "Length";
inline constexpr const char* Angle    = "Angle";
inline constexpr const char* Width    = "Width";
inline constexpr const char* Height   = "Height";
inline constexpr const char* Radius   = "Radius";
inline constexpr const char* Diameter = "Diameter";
inline constexpr const char* Sweep    = "Sweep";
}  // namespace SketchInputFields

struct SketchInputField
{
    std::string name;
    UnitKind    kind = UnitKind::Length;

    // Internal units throughout: millimetres for Length, DEGREES for
    // Angle. Radians never appear in a field -- they belong to the
    // geometry, and mixing the two in the number a user types is how a
    // typed 45 becomes 0.785.
    double value = 0.0;

    // True once the user has typed this value. A locked field is the one
    // thing a mouse move may not touch; that is what makes a typed length
    // swing the line around instead of being overwritten.
    bool locked = false;

    // What the readout shows it in. Starts at the document's unit and
    // adopts whatever the user types an explicit unit in, the same way
    // UnitLineEdit does.
    LengthUnit lengthUnit = LengthUnit::Millimeter;
    AngleUnit  angleUnit  = AngleUnit::Degree;
};

// What happened to the text that was being typed.
enum class SketchInputResult
{
    Ignored,    // nothing was being typed, so the key was not ours
    Accepted,   // it parsed; the field now holds it and is locked
    Rejected    // it is not a number; the field keeps the value it had
};

class SketchInput
{
public:
    // Arm with the boxes one stage of one tool offers. Drops every lock
    // and anything half-typed -- a new stage measures new things.
    void Begin(const std::vector<SketchInputField>& theFields);
    void End();

    bool IsActive() const { return !myFields.empty(); }

    const std::vector<SketchInputField>& Fields() const { return myFields; }
    int  ActiveField() const { return myActive; }
    bool SetActiveField(int theIndex);

    // -1 when this tool offers no such box.
    int IndexOf(const std::string& theName) const;

    // Feed a live measurement from the cursor. Silently ignored for a
    // locked field: refusing here, in one place, is what keeps every
    // caller from having to remember the rule.
    void SetMeasured(int theIndex, double theValue);
    void SetMeasured(const std::string& theName, double theValue);

    double Value(const std::string& theName) const;
    bool   IsLocked(const std::string& theName) const;
    bool   AnyLocked() const;
    void   UnlockAll();

    // ---- keyboard ----

    // A printable character. Typing only STARTS on a digit, a decimal
    // point or a minus sign, so letters stay available as tool shortcuts
    // until a number is actually being entered.
    bool TypeCharacter(char theCharacter);
    bool Backspace();

    // Tab: banks what was typed, then moves on. A value that does not
    // parse holds the cursor in its own box rather than being dropped
    // silently on the way past.
    bool NextField();

    // Escape: clear what is being typed, and failing that release the
    // active lock. False when there was nothing to clear, which is what
    // lets the tool's own Escape handling take over.
    bool ClearTyping();

    // Enter.
    SketchInputResult Commit();

    const std::string& TypedText() const { return myTyped; }
    bool IsTyping() const { return !myTyped.empty(); }

    // ---- readout ----

    // Value of one box as the user should read it: what they are typing
    // if they are typing, otherwise the measurement in its own unit.
    std::string FieldText(int theIndex) const;

    // One line per box, e.g. "Length  25.4 mm". The active box is marked
    // so it is obvious which one a keystroke lands in.
    std::vector<std::string> ReadoutLines() const;

    // ---- the maths ----

    // Where the point actually goes, given what is locked. Reads the
    // Length and Angle boxes; a tool with neither gets the cursor back
    // unchanged.
    gp_Pnt2d ResolvePoint(const gp_Pnt2d& theAnchor, const gp_Pnt2d& theCursor) const;

    // Same for a box: reads Width and Height, which are measured along
    // the sketch axes.
    gp_Pnt2d ResolveCorner(const gp_Pnt2d& theAnchor, const gp_Pnt2d& theCursor) const;

    // Fill the Length/Angle boxes from the cursor.
    void MeasureFromAnchor(const gp_Pnt2d& theAnchor, const gp_Pnt2d& theCursor);

    // Fill the Width/Height boxes from the cursor. theHalf is for the
    // centre rectangle, whose cursor reaches only half the width.
    void MeasureBox(const gp_Pnt2d& theAnchor, const gp_Pnt2d& theCursor, bool theHalf = false);

private:
    // Parse what has been typed into the active box.
    SketchInputResult Apply();

    std::vector<SketchInputField> myFields;
    int                           myActive = -1;
    std::string                   myTyped;
};

// ---- free maths, so a test can hand-compute against it ----

// Distance from theAnchor to theCursor, in millimetres.
double SketchInputLength(const gp_Pnt2d& theAnchor, const gp_Pnt2d& theCursor);

// Direction to theCursor in degrees counter-clockwise from the sketch's
// X axis, normalised into [0, 360).
double SketchInputAngle(const gp_Pnt2d& theAnchor, const gp_Pnt2d& theCursor);

// The point the cursor is asking for, bent to whatever the user has
// typed:
//   nothing locked   the cursor, untouched
//   length only      that distance away, in the cursor's direction
//   angle only       along the typed ray, as far as the cursor reaches
//   both             pinned; the cursor no longer has any say
gp_Pnt2d SketchInputPoint(const gp_Pnt2d& theAnchor,
                          const gp_Pnt2d& theCursor,
                          bool            theHasLength,
                          double          theLength,
                          bool            theHasAngle,
                          double          theAngleDegrees);

// Same idea for an axis-aligned box. A typed extent keeps the SIDE the
// cursor is on, so typing 40 while dragging left builds the rectangle to
// the left rather than jumping it across the anchor.
gp_Pnt2d SketchInputCorner(const gp_Pnt2d& theAnchor,
                           const gp_Pnt2d& theCursor,
                           bool            theHasWidth,
                           double          theWidth,
                           bool            theHasHeight,
                           double          theHeight);

}  // namespace lcad
