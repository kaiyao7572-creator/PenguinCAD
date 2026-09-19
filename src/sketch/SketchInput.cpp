#include "sketch/SketchInput.h"

#include <gp_Vec2d.hxx>

#include <cmath>

namespace lcad {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Shorter than this and the cursor is on top of the anchor, so it names
// no direction at all.
constexpr double kTiny = 1.0e-9;

// A typed value longer than this is a stuck key, not a dimension.
constexpr std::size_t kMaxTyped = 24;

bool StartsTyping(char theCharacter)
{
    return (theCharacter >= '0' && theCharacter <= '9') || theCharacter == '.'
        || theCharacter == '-';
}

// Once a number is being typed the unit words become part of it, which is
// what lets "1in" and "1' 6\"" through to the parser whole.
bool ContinuesTyping(char theCharacter)
{
    return StartsTyping(theCharacter) || theCharacter == '+' || theCharacter == '/'
        || theCharacter == '\'' || theCharacter == '"' || theCharacter == ' '
        || (theCharacter >= 'a' && theCharacter <= 'z')
        || (theCharacter >= 'A' && theCharacter <= 'Z');
}

}  // namespace

void SketchInput::Begin(const std::vector<SketchInputField>& theFields)
{
    myFields = theFields;
    myTyped.clear();
    myActive = myFields.empty() ? -1 : 0;

    for (SketchInputField& field : myFields) {
        field.lengthUnit = DefaultLengthUnit();
        field.angleUnit = AngleUnit::Degree;
    }
}

void SketchInput::End()
{
    myFields.clear();
    myTyped.clear();
    myActive = -1;
}

bool SketchInput::SetActiveField(int theIndex)
{
    if (theIndex < 0 || theIndex >= static_cast<int>(myFields.size())) {
        return false;
    }
    myActive = theIndex;
    return true;
}

int SketchInput::IndexOf(const std::string& theName) const
{
    for (std::size_t i = 0; i < myFields.size(); ++i) {
        if (myFields[i].name == theName) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void SketchInput::SetMeasured(int theIndex, double theValue)
{
    if (theIndex < 0 || theIndex >= static_cast<int>(myFields.size())) {
        return;
    }
    if (myFields[theIndex].locked) {
        return;  // the whole point of a lock
    }
    myFields[theIndex].value = theValue;
}

void SketchInput::SetMeasured(const std::string& theName, double theValue)
{
    SetMeasured(IndexOf(theName), theValue);
}

double SketchInput::Value(const std::string& theName) const
{
    const int index = IndexOf(theName);
    return index < 0 ? 0.0 : myFields[index].value;
}

bool SketchInput::IsLocked(const std::string& theName) const
{
    const int index = IndexOf(theName);
    return index >= 0 && myFields[index].locked;
}

bool SketchInput::AnyLocked() const
{
    for (const SketchInputField& field : myFields) {
        if (field.locked) {
            return true;
        }
    }
    return false;
}

void SketchInput::UnlockAll()
{
    for (SketchInputField& field : myFields) {
        field.locked = false;
    }
    myTyped.clear();
}

bool SketchInput::TypeCharacter(char theCharacter)
{
    if (!IsActive() || myActive < 0) {
        return false;
    }
    if (myTyped.empty() ? !StartsTyping(theCharacter) : !ContinuesTyping(theCharacter)) {
        return false;
    }
    if (myTyped.size() >= kMaxTyped) {
        return true;  // consumed, but the field has had enough
    }
    myTyped.push_back(theCharacter);
    return true;
}

bool SketchInput::Backspace()
{
    if (myTyped.empty()) {
        return false;
    }
    myTyped.pop_back();
    return true;
}

SketchInputResult SketchInput::Apply()
{
    if (myActive < 0 || myActive >= static_cast<int>(myFields.size())) {
        return SketchInputResult::Ignored;
    }
    if (myTyped.empty()) {
        return SketchInputResult::Ignored;
    }

    SketchInputField& field = myFields[myActive];
    const ParsedValue parsed =
        ParseValue(myTyped, field.kind, field.lengthUnit, field.angleUnit);
    if (!parsed.ok || !std::isfinite(parsed.value)) {
        return SketchInputResult::Rejected;
    }

    field.value = parsed.value;
    field.locked = true;
    if (parsed.hasExplicitUnit) {
        // Typing inches into a box makes that box an inches box, exactly
        // as it does in every other numeric field in the app.
        field.lengthUnit = parsed.lengthUnit;
        field.angleUnit = parsed.angleUnit;
    }
    myTyped.clear();
    return SketchInputResult::Accepted;
}

bool SketchInput::NextField()
{
    if (!IsActive()) {
        return false;
    }
    if (!myTyped.empty() && Apply() == SketchInputResult::Rejected) {
        return true;  // stay put so the user can see and fix what they typed
    }
    myActive = (myActive + 1) % static_cast<int>(myFields.size());
    return true;
}

bool SketchInput::ClearTyping()
{
    if (!myTyped.empty()) {
        myTyped.clear();
        return true;
    }
    if (myActive >= 0 && myActive < static_cast<int>(myFields.size())
        && myFields[myActive].locked) {
        // Escape walks back out of the locks one at a time before it is
        // allowed to reach the tool and cancel the curve.
        myFields[myActive].locked = false;
        return true;
    }
    return false;
}

SketchInputResult SketchInput::Commit()
{
    return Apply();
}

std::string SketchInput::FieldText(int theIndex) const
{
    if (theIndex < 0 || theIndex >= static_cast<int>(myFields.size())) {
        return std::string();
    }
    if (theIndex == myActive && !myTyped.empty()) {
        return myTyped;
    }

    const SketchInputField& field = myFields[theIndex];
    return FormatValue(field.value, field.kind, field.lengthUnit, field.angleUnit, 2);
}

std::vector<std::string> SketchInput::ReadoutLines() const
{
    std::vector<std::string> lines;
    lines.reserve(myFields.size());

    for (std::size_t i = 0; i < myFields.size(); ++i) {
        const SketchInputField& field = myFields[i];
        // "=" is how this app already writes a driven dimension, so a
        // locked box reads as one; "> " marks where the next keystroke
        // lands.
        // No "> " marker for the active box: the box itself is
        // highlighted, which is how Fusion says it and costs no width.
        // "=" still marks a value the user has pinned, so a driven box is
        // distinguishable from one still following the mouse.
        // Padded with a space either side: the filled plate behind the
        // text is sized to the glyphs exactly, and text touching the edge
        // of its own box reads as cramped rather than as a field.
        lines.push_back(" " + field.name + (field.locked ? " = " : "  ")
                        + FieldText(static_cast<int>(i)) + " ");
    }
    return lines;
}

gp_Pnt2d SketchInput::ResolvePoint(const gp_Pnt2d& theAnchor, const gp_Pnt2d& theCursor) const
{
    const int length = IndexOf(SketchInputFields::Length);
    const int angle = IndexOf(SketchInputFields::Angle);
    const bool hasLength = length >= 0 && myFields[length].locked;
    const bool hasAngle = angle >= 0 && myFields[angle].locked;

    return SketchInputPoint(theAnchor, theCursor, hasLength,
                            hasLength ? myFields[length].value : 0.0, hasAngle,
                            hasAngle ? myFields[angle].value : 0.0);
}

gp_Pnt2d SketchInput::ResolveCorner(const gp_Pnt2d& theAnchor, const gp_Pnt2d& theCursor) const
{
    const int width = IndexOf(SketchInputFields::Width);
    const int height = IndexOf(SketchInputFields::Height);
    const bool hasWidth = width >= 0 && myFields[width].locked;
    const bool hasHeight = height >= 0 && myFields[height].locked;

    return SketchInputCorner(theAnchor, theCursor, hasWidth,
                             hasWidth ? myFields[width].value : 0.0, hasHeight,
                             hasHeight ? myFields[height].value : 0.0);
}

void SketchInput::MeasureFromAnchor(const gp_Pnt2d& theAnchor, const gp_Pnt2d& theCursor)
{
    SetMeasured(SketchInputFields::Length, SketchInputLength(theAnchor, theCursor));
    SetMeasured(SketchInputFields::Angle, SketchInputAngle(theAnchor, theCursor));
}

void SketchInput::MeasureBox(const gp_Pnt2d& theAnchor, const gp_Pnt2d& theCursor, bool theHalf)
{
    const double scale = theHalf ? 2.0 : 1.0;
    SetMeasured(SketchInputFields::Width, std::fabs(theCursor.X() - theAnchor.X()) * scale);
    SetMeasured(SketchInputFields::Height, std::fabs(theCursor.Y() - theAnchor.Y()) * scale);
}

// ---- free maths ----

double SketchInputLength(const gp_Pnt2d& theAnchor, const gp_Pnt2d& theCursor)
{
    return theAnchor.Distance(theCursor);
}

double SketchInputAngle(const gp_Pnt2d& theAnchor, const gp_Pnt2d& theCursor)
{
    const double dx = theCursor.X() - theAnchor.X();
    const double dy = theCursor.Y() - theAnchor.Y();
    if (std::fabs(dx) <= kTiny && std::fabs(dy) <= kTiny) {
        return 0.0;
    }

    double degrees = std::atan2(dy, dx) * 180.0 / kPi;
    if (degrees < 0.0) {
        degrees += 360.0;
    }
    return degrees;
}

gp_Pnt2d SketchInputPoint(const gp_Pnt2d& theAnchor,
                          const gp_Pnt2d& theCursor,
                          bool            theHasLength,
                          double          theLength,
                          bool            theHasAngle,
                          double          theAngleDegrees)
{
    if (!theHasLength && !theHasAngle) {
        return theCursor;
    }

    if (theHasAngle) {
        const double radians = theAngleDegrees * kPi / 180.0;
        const gp_Vec2d direction(std::cos(radians), std::sin(radians));

        double length = theLength;
        if (!theHasLength) {
            // How far along the typed ray the cursor has got. A cursor
            // BEHIND the anchor gives zero rather than a line flipped
            // through 180 degrees: the angle the user typed is the one
            // thing here that must stay exactly true.
            length = direction.X() * (theCursor.X() - theAnchor.X())
                   + direction.Y() * (theCursor.Y() - theAnchor.Y());
            if (length < 0.0) {
                length = 0.0;
            }
        }
        return theAnchor.Translated(direction * length);
    }

    gp_Vec2d direction(theCursor.X() - theAnchor.X(), theCursor.Y() - theAnchor.Y());
    if (direction.SquareMagnitude() <= kTiny * kTiny) {
        direction = gp_Vec2d(1.0, 0.0);  // no direction to keep: lie along +X
    } else {
        direction.Normalize();
    }
    return theAnchor.Translated(direction * theLength);
}

gp_Pnt2d SketchInputCorner(const gp_Pnt2d& theAnchor,
                           const gp_Pnt2d& theCursor,
                           bool            theHasWidth,
                           double          theWidth,
                           bool            theHasHeight,
                           double          theHeight)
{
    double dx = theCursor.X() - theAnchor.X();
    double dy = theCursor.Y() - theAnchor.Y();

    if (theHasWidth) {
        dx = (dx < 0.0 ? -1.0 : 1.0) * std::fabs(theWidth);
    }
    if (theHasHeight) {
        dy = (dy < 0.0 ? -1.0 : 1.0) * std::fabs(theHeight);
    }
    return gp_Pnt2d(theAnchor.X() + dx, theAnchor.Y() + dy);
}

}  // namespace lcad
