#pragma once

#include <string>
#include <vector>

namespace lcad {

// What a number means, which decides both the unit table it's parsed
// against and how it's shown back.
enum class UnitKind
{
    Length,     // stored internally in millimetres
    Angle,      // stored internally in degrees
    Unitless    // counts, ratios -- no suffix
};

// Every length unit the parser understands. Internal storage is always
// millimetres; these only affect what the user types and reads.
enum class LengthUnit
{
    Millimeter,
    Centimeter,
    Meter,
    Inch,
    Foot
};

enum class AngleUnit
{
    Degree,
    Radian
};

// How many millimetres one of these is.
double MillimetersPer(LengthUnit theUnit);

// Canonical short symbol, e.g. "mm", "in".
std::string SymbolOf(LengthUnit theUnit);
std::string SymbolOf(AngleUnit theUnit);

// Resolve a typed unit word ("in", "inch", "inches", "\"") to a unit.
// Returns false when the text names no unit we know.
bool LengthUnitFromText(const std::string& theText, LengthUnit& theUnit);
bool AngleUnitFromText(const std::string& theText, AngleUnit& theUnit);

// The outcome of reading what a user typed into a numeric field.
struct ParsedValue
{
    bool ok = false;

    // Value converted to the internal base unit: millimetres for Length,
    // degrees for Angle, as-typed for Unitless.
    double value = 0.0;

    // True when the text carried an explicit unit ("12.9in") rather than
    // relying on the field's current one ("12.9"). This is what lets a
    // field switch itself to inches the moment you type inches.
    bool hasExplicitUnit = false;

    LengthUnit lengthUnit = LengthUnit::Millimeter;
    AngleUnit  angleUnit  = AngleUnit::Degree;
};

// Parse user input for a field of the given kind.
//
// theFallbackLength/theFallbackAngle are used when no unit is typed, so a
// bare "20" means whatever the field is currently showing.
//
// Understands: "20", "20mm", "20 mm", "12.9in", "12.9 inches", "1/2in",
// "2'", "1' 6\"", "2.5cm", "1m", "90deg", "90°", "1.5708rad", and a
// leading +/-. Whitespace and case are ignored.
ParsedValue ParseValue(const std::string& theText,
                       UnitKind           theKind,
                       LengthUnit         theFallbackLength = LengthUnit::Millimeter,
                       AngleUnit          theFallbackAngle  = AngleUnit::Degree);

// Render an internal value (mm / degrees) for display in the given unit,
// with trailing zeros trimmed and the unit suffix appended.
std::string FormatValue(double     theInternalValue,
                        UnitKind   theKind,
                        LengthUnit theLengthUnit = LengthUnit::Millimeter,
                        AngleUnit  theAngleUnit  = AngleUnit::Degree,
                        int        theMaxDecimals = 4);

// Convert an internal (mm) value into the given display unit, and back.
double FromMillimeters(double theMillimeters, LengthUnit theUnit);
double ToMillimeters(double theValue, LengthUnit theUnit);
double FromDegrees(double theDegrees, AngleUnit theUnit);
double ToDegrees(double theValue, AngleUnit theUnit);

// Document-wide default display unit for lengths. New fields start here;
// a field that sees an explicit unit typed into it overrides this for
// itself only.
LengthUnit DefaultLengthUnit();
void SetDefaultLengthUnit(LengthUnit theUnit);

// Unit choices offered in a units picker, in display order.
std::vector<LengthUnit> SelectableLengthUnits();

} // namespace lcad
