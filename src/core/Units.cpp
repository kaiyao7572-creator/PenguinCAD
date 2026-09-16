#include "core/Units.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace lcad {

namespace {

constexpr double kPi = 3.14159265358979323846;

LengthUnit theDefaultLengthUnit = LengthUnit::Millimeter;

std::string Lowered(const std::string& theText)
{
    std::string out = theText;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// Strip every space so "12.9 in" and "12.9in" take the same path.
std::string Compact(const std::string& theText)
{
    std::string out;
    out.reserve(theText.size());
    for (char c : theText) {
        if (!std::isspace(static_cast<unsigned char>(c))) {
            out.push_back(c);
        }
    }
    return out;
}

// Pull a leading number off the front, supporting "1/2" fractions since
// imperial input is usually written that way. Returns false if there
// isn't a number to read.
bool TakeNumber(const std::string& theText, std::size_t& thePos, double& theValue)
{
    const std::size_t start = thePos;

    if (thePos < theText.size() && (theText[thePos] == '+' || theText[thePos] == '-')) {
        ++thePos;
    }
    std::size_t digitsStart = thePos;
    while (thePos < theText.size()
           && (std::isdigit(static_cast<unsigned char>(theText[thePos])) || theText[thePos] == '.')) {
        ++thePos;
    }
    if (thePos == digitsStart) {
        thePos = start;
        return false;
    }

    theValue = std::strtod(theText.substr(start, thePos - start).c_str(), nullptr);

    // Fraction: "1/2" -> 0.5, keeping the sign of the numerator.
    if (thePos < theText.size() && theText[thePos] == '/') {
        const std::size_t slash = thePos;
        ++thePos;
        const std::size_t denomStart = thePos;
        while (thePos < theText.size()
               && std::isdigit(static_cast<unsigned char>(theText[thePos]))) {
            ++thePos;
        }
        if (thePos == denomStart) {
            thePos = slash;   // a stray '/': leave it for the unit parser
        } else {
            const double denominator =
                std::strtod(theText.substr(denomStart, thePos - denomStart).c_str(), nullptr);
            if (denominator != 0.0) {
                theValue /= denominator;
            }
        }
    }

    return true;
}

} // namespace

double MillimetersPer(LengthUnit theUnit)
{
    switch (theUnit) {
        case LengthUnit::Millimeter: return 1.0;
        case LengthUnit::Centimeter: return 10.0;
        case LengthUnit::Meter:      return 1000.0;
        case LengthUnit::Inch:       return 25.4;
        case LengthUnit::Foot:       return 304.8;
    }
    return 1.0;
}

std::string SymbolOf(LengthUnit theUnit)
{
    switch (theUnit) {
        case LengthUnit::Millimeter: return "mm";
        case LengthUnit::Centimeter: return "cm";
        case LengthUnit::Meter:      return "m";
        case LengthUnit::Inch:       return "in";
        case LengthUnit::Foot:       return "ft";
    }
    return "mm";
}

std::string SymbolOf(AngleUnit theUnit)
{
    return theUnit == AngleUnit::Radian ? "rad" : "deg";
}

bool LengthUnitFromText(const std::string& theText, LengthUnit& theUnit)
{
    const std::string text = Lowered(Compact(theText));
    if (text.empty()) {
        return false;
    }

    // The double-prime and prime characters people actually type for
    // inches and feet, plus the spelled-out forms.
    if (text == "mm" || text == "millimeter" || text == "millimeters"
        || text == "millimetre" || text == "millimetres") {
        theUnit = LengthUnit::Millimeter;
        return true;
    }
    if (text == "cm" || text == "centimeter" || text == "centimeters"
        || text == "centimetre" || text == "centimetres") {
        theUnit = LengthUnit::Centimeter;
        return true;
    }
    if (text == "m" || text == "meter" || text == "meters"
        || text == "metre" || text == "metres") {
        theUnit = LengthUnit::Meter;
        return true;
    }
    if (text == "in" || text == "inch" || text == "inches" || text == "\"" || text == "''") {
        theUnit = LengthUnit::Inch;
        return true;
    }
    if (text == "ft" || text == "foot" || text == "feet" || text == "'") {
        theUnit = LengthUnit::Foot;
        return true;
    }
    return false;
}

bool AngleUnitFromText(const std::string& theText, AngleUnit& theUnit)
{
    const std::string text = Lowered(Compact(theText));
    if (text.empty()) {
        return false;
    }
    if (text == "deg" || text == "degree" || text == "degrees" || text == "\xc2\xb0") {
        theUnit = AngleUnit::Degree;
        return true;
    }
    if (text == "rad" || text == "radian" || text == "radians") {
        theUnit = AngleUnit::Radian;
        return true;
    }
    return false;
}

ParsedValue ParseValue(const std::string& theText,
                       UnitKind           theKind,
                       LengthUnit         theFallbackLength,
                       AngleUnit          theFallbackAngle)
{
    ParsedValue result;
    result.lengthUnit = theFallbackLength;
    result.angleUnit = theFallbackAngle;

    const std::string text = Compact(theText);
    if (text.empty()) {
        return result;
    }

    std::size_t pos = 0;
    double number = 0.0;
    if (!TakeNumber(text, pos, number)) {
        return result;
    }

    std::string suffix = text.substr(pos);

    if (theKind == UnitKind::Unitless) {
        // Trailing junk on a plain count is a typo, not a unit.
        result.ok = suffix.empty();
        result.value = number;
        return result;
    }

    if (theKind == UnitKind::Angle) {
        AngleUnit unit = theFallbackAngle;
        if (!suffix.empty()) {
            if (!AngleUnitFromText(suffix, unit)) {
                return result;   // unrecognised suffix: reject rather than guess
            }
            result.hasExplicitUnit = true;
        }
        result.angleUnit = unit;
        result.value = ToDegrees(number, unit);
        result.ok = true;
        return result;
    }

    // ---- length ----
    LengthUnit unit = theFallbackLength;
    if (suffix.empty()) {
        result.lengthUnit = unit;
        result.value = ToMillimeters(number, unit);
        result.ok = true;
        return result;
    }

    if (!LengthUnitFromText(suffix, unit)) {
        // Could be combined feet+inches: 1'6", 1' 6in, 5'11.
        LengthUnit firstUnit = LengthUnit::Millimeter;
        std::size_t split = 0;
        bool matched = false;
        for (split = 1; split <= suffix.size(); ++split) {
            if (LengthUnitFromText(suffix.substr(0, split), firstUnit)) {
                matched = true;
                break;
            }
        }
        if (!matched) {
            return result;
        }

        const std::string rest = suffix.substr(split);
        double secondNumber = 0.0;
        std::size_t restPos = 0;
        if (rest.empty() || !TakeNumber(rest, restPos, secondNumber)) {
            return result;
        }

        // The trailing part defaults to inches after feet, which is how
        // 5'11 is universally read.
        LengthUnit secondUnit =
            (firstUnit == LengthUnit::Foot) ? LengthUnit::Inch : LengthUnit::Millimeter;
        const std::string secondSuffix = rest.substr(restPos);
        if (!secondSuffix.empty() && !LengthUnitFromText(secondSuffix, secondUnit)) {
            return result;
        }

        const double sign = (number < 0.0) ? -1.0 : 1.0;
        result.value = ToMillimeters(number, firstUnit)
                       + sign * ToMillimeters(std::fabs(secondNumber), secondUnit);
        result.hasExplicitUnit = true;
        // Show a combined measurement back in the finer of the two units.
        result.lengthUnit = secondUnit;
        result.ok = true;
        return result;
    }

    result.hasExplicitUnit = true;
    result.lengthUnit = unit;
    result.value = ToMillimeters(number, unit);
    result.ok = true;
    return result;
}

double FromMillimeters(double theMillimeters, LengthUnit theUnit)
{
    return theMillimeters / MillimetersPer(theUnit);
}

double ToMillimeters(double theValue, LengthUnit theUnit)
{
    return theValue * MillimetersPer(theUnit);
}

double FromDegrees(double theDegrees, AngleUnit theUnit)
{
    return theUnit == AngleUnit::Radian ? theDegrees * kPi / 180.0 : theDegrees;
}

double ToDegrees(double theValue, AngleUnit theUnit)
{
    return theUnit == AngleUnit::Radian ? theValue * 180.0 / kPi : theValue;
}

std::string FormatValue(double     theInternalValue,
                        UnitKind   theKind,
                        LengthUnit theLengthUnit,
                        AngleUnit  theAngleUnit,
                        int        theMaxDecimals)
{
    double shown = theInternalValue;
    std::string suffix;

    switch (theKind) {
        case UnitKind::Length:
            shown = FromMillimeters(theInternalValue, theLengthUnit);
            suffix = " " + SymbolOf(theLengthUnit);
            break;
        case UnitKind::Angle:
            shown = FromDegrees(theInternalValue, theAngleUnit);
            suffix = " " + SymbolOf(theAngleUnit);
            break;
        case UnitKind::Unitless:
            break;
    }

    // -0 reads badly in a dimension field.
    if (shown == 0.0) {
        shown = 0.0;
    }

    std::ostringstream stream;
    stream.setf(std::ios::fixed);
    stream.precision(std::max(0, theMaxDecimals));
    stream << shown;
    std::string digits = stream.str();

    // Trim trailing zeros, then a bare trailing point.
    if (digits.find('.') != std::string::npos) {
        digits.erase(digits.find_last_not_of('0') + 1);
        if (!digits.empty() && digits.back() == '.') {
            digits.pop_back();
        }
    }

    return digits + suffix;
}

LengthUnit DefaultLengthUnit()
{
    return theDefaultLengthUnit;
}

void SetDefaultLengthUnit(LengthUnit theUnit)
{
    theDefaultLengthUnit = theUnit;
}

std::vector<LengthUnit> SelectableLengthUnits()
{
    return {LengthUnit::Millimeter,
            LengthUnit::Centimeter,
            LengthUnit::Meter,
            LengthUnit::Inch,
            LengthUnit::Foot};
}

} // namespace lcad
