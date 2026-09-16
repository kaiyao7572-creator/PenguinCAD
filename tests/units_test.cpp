#include "core/Units.h"

#include <cmath>
#include <iostream>

using namespace lcad;

static int failures = 0;

static void expect(const std::string& theText, double theExpectedMm, bool theExplicit,
                   LengthUnit theExpectedUnit, UnitKind kind = UnitKind::Length)
{
    const ParsedValue p = ParseValue(theText, kind);
    const bool ok = p.ok
                    && std::fabs(p.value - theExpectedMm) < 1e-6
                    && p.hasExplicitUnit == theExplicit
                    && (kind != UnitKind::Length || p.lengthUnit == theExpectedUnit);
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << "\"" << theText << "\" -> " << p.value
              << (p.ok ? "" : " (rejected)")
              << " expected " << theExpectedMm << std::endl;
    if (!ok) ++failures;
}

static void expectReject(const std::string& theText, UnitKind kind = UnitKind::Length)
{
    const ParsedValue p = ParseValue(theText, kind);
    std::cout << (p.ok ? "  FAIL  " : "  PASS  ") << "reject \"" << theText << "\"" << std::endl;
    if (p.ok) ++failures;
}

static void expectFormat(double mm, LengthUnit u, const std::string& want)
{
    const std::string got = FormatValue(mm, UnitKind::Length, u);
    const bool ok = got == want;
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << "format " << mm << "mm -> \"" << got
              << "\" expected \"" << want << "\"" << std::endl;
    if (!ok) ++failures;
}

int main()
{
    std::cout << "-- bare numbers default to mm --" << std::endl;
    expect("20", 20.0, false, LengthUnit::Millimeter);
    expect("12.5", 12.5, false, LengthUnit::Millimeter);
    expect("-3", -3.0, false, LengthUnit::Millimeter);

    std::cout << "-- explicit metric --" << std::endl;
    expect("20mm", 20.0, true, LengthUnit::Millimeter);
    expect("20 mm", 20.0, true, LengthUnit::Millimeter);
    expect("2.5cm", 25.0, true, LengthUnit::Centimeter);
    expect("1m", 1000.0, true, LengthUnit::Meter);

    std::cout << "-- the headline case: inches --" << std::endl;
    expect("12.9inches", 12.9 * 25.4, true, LengthUnit::Inch);
    expect("12.9in", 12.9 * 25.4, true, LengthUnit::Inch);
    expect("12.9 in", 12.9 * 25.4, true, LengthUnit::Inch);
    expect("1\"", 25.4, true, LengthUnit::Inch);
    expect("1inch", 25.4, true, LengthUnit::Inch);

    std::cout << "-- fractions and feet --" << std::endl;
    expect("1/2in", 12.7, true, LengthUnit::Inch);
    expect("3/4\"", 19.05, true, LengthUnit::Inch);
    expect("2ft", 609.6, true, LengthUnit::Foot);
    expect("2'", 609.6, true, LengthUnit::Foot);
    expect("1'6\"", 304.8 + 6 * 25.4, true, LengthUnit::Inch);
    expect("5'11", 5 * 304.8 + 11 * 25.4, true, LengthUnit::Inch);
    expect("-1'6\"", -(304.8 + 6 * 25.4), true, LengthUnit::Inch);

    std::cout << "-- case and spacing are ignored --" << std::endl;
    expect("12.9IN", 12.9 * 25.4, true, LengthUnit::Inch);
    expect("  7 MM  ", 7.0, true, LengthUnit::Millimeter);

    std::cout << "-- nonsense is rejected, not guessed --" << std::endl;
    expectReject("");
    expectReject("abc");
    expectReject("12furlongs");
    expectReject("mm");

    std::cout << "-- angles --" << std::endl;
    {
        const ParsedValue p = ParseValue("90", UnitKind::Angle);
        const bool ok = p.ok && std::fabs(p.value - 90.0) < 1e-9;
        std::cout << (ok ? "  PASS  " : "  FAIL  ") << "\"90\" -> " << p.value << " deg" << std::endl;
        if (!ok) ++failures;
    }
    {
        const ParsedValue p = ParseValue("1.5707963268rad", UnitKind::Angle);
        const bool ok = p.ok && std::fabs(p.value - 90.0) < 1e-6 && p.hasExplicitUnit;
        std::cout << (ok ? "  PASS  " : "  FAIL  ") << "\"1.5708rad\" -> " << p.value << " deg"
                  << std::endl;
        if (!ok) ++failures;
    }
    expectReject("90kg", UnitKind::Angle);

    std::cout << "-- formatting --" << std::endl;
    expectFormat(20.0, LengthUnit::Millimeter, "20 mm");
    expectFormat(25.4, LengthUnit::Inch, "1 in");
    expectFormat(12.9 * 25.4, LengthUnit::Inch, "12.9 in");
    expectFormat(1000.0, LengthUnit::Meter, "1 m");
    expectFormat(0.0, LengthUnit::Millimeter, "0 mm");

    std::cout << "-- round trip through every unit --" << std::endl;
    for (LengthUnit u : SelectableLengthUnits()) {
        const double mm = 123.456;
        const double back = ToMillimeters(FromMillimeters(mm, u), u);
        const bool ok = std::fabs(back - mm) < 1e-9;
        std::cout << (ok ? "  PASS  " : "  FAIL  ") << "round trip via " << SymbolOf(u) << std::endl;
        if (!ok) ++failures;
    }

    std::cout << (failures == 0 ? "\nALL UNIT TESTS PASSED\n" : "\nFAILURES\n");
    return failures == 0 ? 0 : 1;
}
