// Parameters: named values and the expressions that read them.
//
// This is the engine behind Fusion's MODIFY > Change Parameters. Two
// things in it are the kind of wrong that never announces itself, so they
// are pinned down here rather than left to prose:
//
//  1. TRIG IS IN DEGREES. The document stores degrees, Fusion's functions
//     take degrees, <cmath> takes radians. sin(30) == 0.5 is the single
//     assertion that tells the two apart -- in radians it is -0.988, and
//     a model built on that is wrong by 57x with nothing to show for it.
//  2. A UNIT IS NEVER APPLIED TWICE. A row shown in inches reading "1" is
//     one inch; the same row reading "plate_width / 4" is a quarter of
//     plate_width in millimetres, because a quarter of 100mm is not a
//     quarter of 100in. Getting this wrong is a silent factor of 25.4.
//
// Expected values here are worked out by hand in the comment beside them,
// deliberately not read back from the implementation.
#include "core/Expression.h"
#include "core/ParameterTable.h"
#include "core/Units.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

using namespace lcad;

static int failures = 0;

static void check(bool theOk, const std::string& theWhat)
{
    std::cout << (theOk ? "  PASS  " : "  FAIL  ") << theWhat << std::endl;
    if (!theOk) {
        ++failures;
    }
}

// An expression that must evaluate, to this number.
static void expectValue(const std::string& theText, double theExpected,
                        const VariableLookup& theLookup = VariableLookup(),
                        double theTolerance = 1.0e-9)
{
    const ExpressionResult r = EvaluateExpression(theText, theLookup);
    const bool ok = r.ok && std::fabs(r.value - theExpected) <= theTolerance;
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << "\"" << theText << "\" -> " << r.value
              << " (expect " << theExpected << ")";
    if (!r.ok) {
        std::cout << "  rejected: " << r.error;
    }
    std::cout << std::endl;
    if (!ok) {
        ++failures;
    }
}

// An expression that must be refused, with theFragment somewhere in the
// message. A refusal nobody can read is only half a refusal.
static void expectError(const std::string& theText, const std::string& theFragment,
                        const VariableLookup& theLookup = VariableLookup())
{
    const ExpressionResult r = EvaluateExpression(theText, theLookup);
    const bool ok = !r.ok && r.value == 0.0
                    && r.error.find(theFragment) != std::string::npos;
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << "reject \"" << theText << "\"";
    if (r.ok) {
        std::cout << " -- but it evaluated to " << r.value;
    } else {
        std::cout << " -- \"" << r.error << "\"";
    }
    std::cout << std::endl;
    if (!ok) {
        ++failures;
    }
}

static void expectNear(double theValue, double theExpected, double theTolerance,
                       const std::string& theWhat)
{
    const bool ok = std::fabs(theValue - theExpected) <= theTolerance;
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << theWhat << "  (got " << theValue << ", expect "
              << theExpected << ")" << std::endl;
    if (!ok) {
        ++failures;
    }
}

static bool Contains(const std::vector<std::string>& theNames, const std::string& theName)
{
    return std::find(theNames.begin(), theNames.end(), theName) != theNames.end();
}

static std::string Join(const std::vector<std::string>& theNames)
{
    std::string out;
    for (const std::string& name : theNames) {
        out += (out.empty() ? "" : ", ") + name;
    }
    return out.empty() ? "(none)" : out;
}

// The value of a row that must be there and must have resolved.
static double ValueOf(const ParameterTable& theTable, const std::string& theName)
{
    const UserParameter* p = theTable.Find(theName);
    if (p == nullptr) {
        std::cout << "  FAIL  no parameter named \"" << theName << "\"" << std::endl;
        ++failures;
        return 0.0;
    }
    if (!p->isValid) {
        std::cout << "  FAIL  \"" << theName << "\" did not resolve: " << p->error << std::endl;
        ++failures;
        return 0.0;
    }
    return p->value;
}

static UserParameter MakeRow(const std::string& theName, const std::string& theExpression,
                             UnitKind theKind = UnitKind::Length,
                             LengthUnit theLengthUnit = LengthUnit::Millimeter,
                             AngleUnit theAngleUnit = AngleUnit::Degree)
{
    UserParameter row;
    row.name = theName;
    row.expression = theExpression;
    row.kind = theKind;
    row.lengthUnit = theLengthUnit;
    row.angleUnit = theAngleUnit;
    return row;
}

int main()
{
    std::string error;

    // ------------------------------------------------------------------
    std::cout << "-- 1. arithmetic and precedence --" << std::endl;
    // ------------------------------------------------------------------
    expectValue("1", 1.0);
    expectValue("1+2*3", 7.0);            // not 9: * binds tighter
    expectValue("(1+2)*3", 9.0);
    expectValue("2*3+4*5", 26.0);         // 6 + 20
    expectValue("100/4/5", 5.0);          // left to right: 25/5, not 100/0.8
    expectValue("10-3-4", 3.0);           // left to right: 7-4, not 10-(-1)
    expectValue("-2*3", -6.0);
    expectValue("2*-3", -6.0);
    expectValue("-(2+3)", -5.0);
    expectValue("--3", 3.0);
    expectValue("1+-2", -1.0);
    expectValue("+7", 7.0);
    expectValue("((((1+1))))", 2.0);
    expectValue("2 * ( 3 + 4 ) - 5", 9.0);
    expectValue("0.25*8", 2.0);
    expectValue(".5+.25", 0.75);

    // ------------------------------------------------------------------
    std::cout << "-- 2. DEGREES, the assertion this whole file exists for --" << std::endl;
    // ------------------------------------------------------------------
    // sin(30 degrees) is exactly 1/2. sin(30 radians) is -0.988. Nothing
    // else in the suite tells those two apart.
    expectValue("sin(30)", 0.5, VariableLookup(), 1.0e-12);
    {
        const ExpressionResult r = EvaluateExpression("sin(30)");
        check(r.ok && std::fabs(r.value - std::sin(30.0)) > 1.0,
              "sin(30) is NOT the radian answer (-0.988)");
    }
    expectValue("cos(60)", 0.5, VariableLookup(), 1.0e-12);
    expectValue("sin(90)", 1.0, VariableLookup(), 1.0e-12);
    expectValue("cos(0)", 1.0);
    expectValue("tan(45)", 1.0, VariableLookup(), 1.0e-12);
    expectValue("asin(0.5)", 30.0, VariableLookup(), 1.0e-9);   // degrees back out
    expectValue("acos(0.5)", 60.0, VariableLookup(), 1.0e-9);
    expectValue("atan(1)", 45.0, VariableLookup(), 1.0e-9);
    // Round trip: a degree in is the same degree out.
    expectValue("asin(sin(37))", 37.0, VariableLookup(), 1.0e-9);

    // PI is the number 3.14159..., which as an ANGLE is 3.14 degrees --
    // so sin(PI) is 0.0548, not 0. That is Fusion's answer too, and it is
    // the proof that PI is a plain constant rather than a secret radian.
    expectValue("PI", 3.14159265358979323846, VariableLookup(), 1.0e-12);
    expectValue("pi", 3.14159265358979323846, VariableLookup(), 1.0e-12);
    expectValue("Pi*2", 6.28318530717958647692, VariableLookup(), 1.0e-12);
    expectValue("sin(PI)", 0.0548036650411, VariableLookup(), 1.0e-9);
    // A literal in radians is converted to degrees at the door, so this
    // is sin(90 degrees).
    expectValue("sin(1.5707963267948966rad)", 1.0, VariableLookup(), 1.0e-12);

    // ------------------------------------------------------------------
    std::cout << "-- 3. the rest of Fusion's functions --" << std::endl;
    // ------------------------------------------------------------------
    expectValue("sqrt(16)", 4.0);
    expectValue("abs(0-3)", 3.0);
    expectValue("abs(-3.5)", 3.5);
    expectValue("floor(2.7)", 2.0);
    expectValue("floor(-2.1)", -3.0);
    expectValue("ceil(2.1)", 3.0);
    expectValue("round(2.5)", 3.0);
    expectValue("round(2.4)", 2.0);
    expectValue("pow(2,10)", 1024.0);
    expectValue("min(3,4)", 3.0);
    expectValue("max(3,4)", 4.0);
    expectValue("max(1, min(9, 5))", 5.0);
    expectValue("SQRT(16)", 4.0);   // case is ignored, as IsReservedExpressionName says
    expectValue("sqrt(pow(3,2)+pow(4,2))", 5.0, VariableLookup(), 1.0e-12);

    // ------------------------------------------------------------------
    std::cout << "-- 4. unit-suffixed literals, read by core/Units.h --" << std::endl;
    // ------------------------------------------------------------------
    expectValue("1in", 25.4);
    expectValue("0.5in", 12.7);
    expectValue("1/2in", 12.7);              // the imperial fraction, one literal
    expectValue("3/8in", 9.525);             // 0.375 * 25.4
    expectValue("1'", 304.8);
    expectValue("1'6\"", 457.2);             // 18in
    expectValue("2.5cm", 25.0);
    expectValue("1m", 1000.0);
    expectValue("10mm+1in", 35.4);
    expectValue("2*1in", 50.8);
    expectValue("90deg", 90.0);              // angles are stored in degrees
    expectValue("1.5707963267948966rad", 90.0, VariableLookup(), 1.0e-9);
    // A bare number is a count and keeps its face value: the 4 in
    // "width / 4" must not become 4mm or 4in.
    expectValue("4", 4.0);
    expectValue("100/4", 25.0);
    expectValue("1/2", 0.5);                 // no unit: plain division, same number

    // Which expressions a field's own unit may be applied to -- the
    // question section 11 turns into millimetres.
    check(IsUnitlessExpression("1"), "\"1\" is dimensionless");
    check(IsUnitlessExpression("1/2"), "\"1/2\" is dimensionless");
    check(IsUnitlessExpression("sin(30)*2"), "\"sin(30)*2\" is -- a function of a number");
    check(IsUnitlessExpression("PI"), "\"PI\" is");
    check(!IsUnitlessExpression("1in"), "\"1in\" is already a length");
    check(!IsUnitlessExpression("10mm+1"), "\"10mm+1\" is -- one unit is enough");
    check(!IsUnitlessExpression("plate_width/4"), "\"plate_width/4\" reads a parameter");
    check(!IsUnitlessExpression("1.2.3"), "and what cannot be read is never scaled");

    // ------------------------------------------------------------------
    std::cout << "-- 5. nothing wrong escapes quietly --" << std::endl;
    // ------------------------------------------------------------------
    expectError("1/0", "division by zero");
    expectError("1/(3-3)", "division by zero");
    expectError("sqrt(-1)", "sqrt");
    expectError("asin(2)", "between -1 and 1");
    expectError("acos(-4)", "between -1 and 1");
    expectError("tan(90)", "undefined");      // 1.6e16 is not a dimension
    expectError("pow(0,-1)", "overflow");
    expectError("pow(-8,0.5)", "not a number");
    expectError("", "empty");
    expectError("   ", "empty");
    expectError("1.2.3", "not a number");     // strtod would have said 1.2
    expectError("12furlongs", "not a unit");
    expectError("10mm2", "not expected");     // a typo, not 12mm
    expectError("1 2", "not expected");
    expectError("(1+2", "expected \")\"");
    expectError("1+", "expected a value");
    expectError("*3", "expected a value");
    expectError("2 in", "not expected");      // a unit has to be joined to its number
    expectError("1e3", "1e3");                // say which of us is wrong
    expectError("what", "no parameter named");
    expectError("sin", "is a function");
    expectError("sin(1,2)", "takes 1 argument");
    expectError("pow(1)", "takes 2 arguments");
    expectError("nope(1)", "not a function");
    expectError("PI(1)", "constant");
    expectError("1 & 2", "means nothing");
    {
        // Deep nesting reports instead of running the C stack out.
        std::string deep(400, '(');
        deep += "1";
        deep += std::string(400, ')');
        expectError(deep, "nested too deeply");
    }

    // ------------------------------------------------------------------
    std::cout << "-- 6. variables, by callback --" << std::endl;
    // ------------------------------------------------------------------
    const VariableLookup lookup = [](const std::string& theName, double& theValue) {
        if (theName == "plate_width") { theValue = 100.0; return true; }
        if (theName == "count")       { theValue = 4.0;   return true; }
        return false;
    };
    expectValue("plate_width", 100.0, lookup);
    expectValue("plate_width/count", 25.0, lookup);
    expectValue("plate_width / count + 1in", 50.4, lookup);   // 25 + 25.4
    expectValue("sin(30)*plate_width", 50.0, lookup, 1.0e-9);
    expectError("Plate_Width", "no parameter named", lookup);   // names are exact

    // ------------------------------------------------------------------
    std::cout << "-- 7. reading the names out of an expression --" << std::endl;
    // ------------------------------------------------------------------
    {
        const std::vector<std::string> v = ExpressionVariables("plate_width/4 + hole*2");
        check(v.size() == 2 && v[0] == "plate_width" && v[1] == "hole",
              "variables come back in order of first appearance: " + Join(v));
    }
    {
        const std::vector<std::string> v = ExpressionVariables("a+a+b");
        check(v.size() == 2 && v[0] == "a" && v[1] == "b", "each name once: " + Join(v));
    }
    {
        const std::vector<std::string> v = ExpressionVariables("sin(a)+PI");
        check(v.size() == 1 && v[0] == "a",
              "function names and PI are not variables: " + Join(v));
    }
    {
        // The case the token-based scan exists for: "mm" the parameter is
        // a name, "mm" the suffix of 10mm is part of the number.
        const std::vector<std::string> v = ExpressionVariables("10mm + mm");
        check(v.size() == 1 && v[0] == "mm", "a unit suffix is not a variable: " + Join(v));
    }

    // ------------------------------------------------------------------
    std::cout << "-- 8. renaming rewrites references, not text --" << std::endl;
    // ------------------------------------------------------------------
    check(RenameExpressionVariable("plate_width/4", "plate_width", "w") == "w/4",
          "reference rewritten");
    check(RenameExpressionVariable("10mm * mm", "mm", "gap") == "10mm * gap",
          "the mm inside 10mm is left alone");
    check(RenameExpressionVariable("a_mm+mm", "mm", "gap") == "a_mm+gap",
          "a name ENDING in the old name is left alone");
    check(RenameExpressionVariable("sin(x) + x", "x", "y") == "sin(y) + y",
          "inside a call too");
    check(RenameExpressionVariable("  a  +  b  ", "a", "alpha") == "  alpha  +  b  ",
          "spacing survives exactly");
    check(RenameExpressionVariable("a+b", "c", "d") == "a+b", "nothing to do is a no-op");

    // ------------------------------------------------------------------
    std::cout << "-- 9. what a name may be --" << std::endl;
    // ------------------------------------------------------------------
    check(IsExpressionIdentifier("plate_width"), "plate_width is a name");
    check(IsExpressionIdentifier("_x9"), "_x9 is a name");
    check(!IsExpressionIdentifier("2x"), "2x is not");
    check(!IsExpressionIdentifier("a b"), "\"a b\" is not");
    check(!IsExpressionIdentifier("a-b"), "a-b is not");
    check(!IsExpressionIdentifier(""), "the empty string is not");
    check(IsReservedExpressionName("sin") && IsReservedExpressionName("Sin")
              && IsReservedExpressionName("SIN"),
          "sin is reserved whatever its case");
    check(IsReservedExpressionName("PI") && IsReservedExpressionName("pi"), "so is PI");
    check(!IsReservedExpressionName("width"), "width is not");
    {
        const std::vector<std::string>& reserved = ReservedExpressionNames();
        bool allReserved = !reserved.empty();
        for (const std::string& name : reserved) {
            allReserved = allReserved && IsReservedExpressionName(name);
        }
        check(allReserved && Contains(reserved, "PI") && Contains(reserved, "pow"),
              "the published list and the check agree");
    }

    // ------------------------------------------------------------------
    std::cout << "-- 10. a table that propagates --" << std::endl;
    // ------------------------------------------------------------------
    {
        ParameterTable table;
        check(table.Add("plate_width", "100", error), "add plate_width = 100");
        check(table.Add("hole_dia", "plate_width / 4", error), "add hole_dia = plate_width / 4");
        expectNear(ValueOf(table, "hole_dia"), 25.0, 1.0e-9, "hole_dia is 25");

        // The headline: change the input, the output follows.
        check(table.SetExpression("plate_width", "200", error), "plate_width := 200");
        expectNear(ValueOf(table, "hole_dia"), 50.0, 1.0e-9, "hole_dia followed to 50");

        // Through a unit, too. 4in is 101.6mm, a quarter of it is 25.4mm.
        check(table.SetExpression("plate_width", "4in", error), "plate_width := 4in");
        expectNear(ValueOf(table, "plate_width"), 101.6, 1.0e-9, "plate_width is 101.6mm");
        expectNear(ValueOf(table, "hole_dia"), 25.4, 1.0e-9, "hole_dia is 25.4mm");

        // Three deep, and out of order: the middle one was written before
        // the one it needs existed.
        check(table.Add("rim", "hole_dia * 2 + edge", error),
              "add rim = hole_dia * 2 + edge, before edge exists");
        check(table.Find("rim") != nullptr && !table.Find("rim")->isValid,
              "rim is invalid for now");
        check(table.Find("rim")->error.find("edge") != std::string::npos,
              "and says which name is missing: " + table.Find("rim")->error);
        check(table.Add("edge", "5", error), "add edge = 5");
        expectNear(ValueOf(table, "rim"), 55.8, 1.0e-9, "rim resolves to 25.4*2 + 5");
        check(table.Resolve(), "the whole table resolves");
    }

    // ------------------------------------------------------------------
    std::cout << "-- 11. a row's unit, applied exactly once --" << std::endl;
    // ------------------------------------------------------------------
    {
        ParameterTable table;
        // A row that is nothing but a number means that number in its own
        // unit -- one inch, the same as typing 1 into an inch field.
        check(table.Add(MakeRow("plate", "1", UnitKind::Length, LengthUnit::Inch), error),
              "add plate = 1, shown in inches");
        expectNear(ValueOf(table, "plate"), 25.4, 1.0e-9, "plate is 25.4mm");
        check(table.Find("plate")->DisplayText() == "1 in",
              "and shows as \"" + table.Find("plate")->DisplayText() + "\"");

        // The moment it does arithmetic the unit can no longer be applied
        // to the whole of it, so it is internal millimetres and the unit
        // only decides how it is shown. This is the deliberate divergence
        // -- a quarter of 100mm is not a quarter of 100in.
        check(table.Add(MakeRow("half", "plate / 2", UnitKind::Length, LengthUnit::Inch), error),
              "add half = plate / 2, also in inches");
        expectNear(ValueOf(table, "half"), 12.7, 1.0e-9, "half is 12.7mm, not 12.7in");
        check(table.Find("half")->DisplayText() == "0.5 in",
              "shown as \"" + table.Find("half")->DisplayText() + "\"");

        // Written with its unit, it means that unit wherever it appears.
        check(table.Add(MakeRow("stock", "1in", UnitKind::Length, LengthUnit::Millimeter), error),
              "add stock = 1in, shown in mm");
        expectNear(ValueOf(table, "stock"), 25.4, 1.0e-9, "stock is 25.4mm");

        // Angles, the same rule.
        check(table.Add(MakeRow("draft", "3", UnitKind::Angle), error), "add draft = 3 deg");
        expectNear(ValueOf(table, "draft"), 3.0, 1.0e-12, "draft is 3 degrees");
        check(table.Add(MakeRow("quarter", "1.5707963267948966", UnitKind::Angle,
                                LengthUnit::Millimeter, AngleUnit::Radian),
                        error),
              "add quarter = 1.5708, shown in radians");
        expectNear(ValueOf(table, "quarter"), 90.0, 1.0e-9, "stored as 90 degrees");

        // A count is a count in every unit.
        check(table.Add(MakeRow("rows", "6", UnitKind::Unitless), error), "add rows = 6");
        expectNear(ValueOf(table, "rows"), 6.0, 1.0e-12, "rows is 6");

        // The same rule for a field that is not a row of the table --
        // what a feature's own expression goes through.
        {
            const ExpressionResult bare =
                table.EvaluateValue("2", UnitKind::Length, LengthUnit::Inch);
            check(bare.ok && std::fabs(bare.value - 50.8) < 1.0e-9,
                  "a feature field showing inches reads \"2\" as 50.8mm");
            const ExpressionResult expr =
                table.EvaluateValue("plate * 2", UnitKind::Length, LengthUnit::Inch);
            check(expr.ok && std::fabs(expr.value - 50.8) < 1.0e-9,
                  "and \"plate * 2\" as 50.8mm -- once, not 25.4 times over");
            const ExpressionResult count = table.Evaluate("rows + 1");
            check(count.ok && std::fabs(count.value - 7.0) < 1.0e-12, "a count stays a count");
        }
    }

    // ------------------------------------------------------------------
    std::cout << "-- 12. cycles are named, refused, and rolled back --" << std::endl;
    // ------------------------------------------------------------------
    {
        ParameterTable table;
        check(table.Add("a", "10", error), "add a = 10");
        check(table.Add("b", "a * 2", error), "add b = a * 2");
        check(table.Add("c", "b + 1", error), "add c = b + 1");
        expectNear(ValueOf(table, "c"), 21.0, 1.0e-9, "c is 21");

        // Direct: a = a + 1.
        check(!table.SetExpression("a", "a + 1", error), "a = a + 1 is refused");
        check(error.find("circular reference") != std::string::npos
                  && error.find("a") != std::string::npos,
              "saying so: " + error);

        // Indirect, the one that needs a real graph walk: a -> c -> b -> a.
        check(!table.SetExpression("a", "c + 1", error), "a = c + 1 is refused");
        check(error.find("circular reference") != std::string::npos
                  && error.find("a") != std::string::npos
                  && error.find("b") != std::string::npos
                  && error.find("c") != std::string::npos,
              "naming all three: " + error);

        // And the table is exactly as it was -- a refused edit that half
        // applied would be worse than one that was allowed.
        check(table.Find("a")->expression == "10", "a's expression is untouched");
        expectNear(ValueOf(table, "a"), 10.0, 1.0e-9, "a is still 10");
        expectNear(ValueOf(table, "b"), 20.0, 1.0e-9, "b is still 20");
        expectNear(ValueOf(table, "c"), 21.0, 1.0e-9, "c is still 21");
        check(table.InvalidParameters().empty(), "nothing is broken");

        // A rename can close a loop too: b refers to "ghost", which is
        // nothing -- until a takes that name.
        check(table.SetExpression("b", "a * 2 + ghost", error), "b = a * 2 + ghost");
        check(!table.Find("b")->isValid, "b is invalid, ghost being nothing");
        std::vector<std::string> rewritten;
        check(!table.Rename("c", "ghost", rewritten, error),
              "renaming c to ghost is refused -- it would close b -> c -> b");
        check(error.find("circular reference") != std::string::npos, "saying so: " + error);
        check(table.Find("c") != nullptr && table.Find("ghost") == nullptr,
              "and c is still called c");
    }

    // ------------------------------------------------------------------
    std::cout << "-- 13. renaming carries the model with it --" << std::endl;
    // ------------------------------------------------------------------
    {
        ParameterTable table;
        check(table.Add("mm", "2", error), "add a parameter actually called mm");
        check(table.Add("w", "10mm + mm", error), "add w = 10mm + mm");
        expectNear(ValueOf(table, "w"), 12.0, 1.0e-9, "w is 10mm plus the parameter, = 12");

        std::vector<std::string> rewritten;
        check(table.Rename("mm", "gap", rewritten, error), "rename mm -> gap");
        check(rewritten.size() == 1 && rewritten[0] == "w",
              "w is reported as rewritten: " + Join(rewritten));
        check(table.Find("w")->expression == "10mm + gap",
              "and reads \"" + table.Find("w")->expression + "\" -- the literal untouched");
        expectNear(ValueOf(table, "w"), 12.0, 1.0e-9, "w is still 12");
        check(table.Find("gap") != nullptr && table.Find("mm") == nullptr, "the row moved name");

        // A rename that nothing refers to touches nothing.
        rewritten.clear();
        check(table.Rename("w", "width", rewritten, error), "rename w -> width");
        check(rewritten.empty(), "nothing needed rewriting: " + Join(rewritten));
    }

    // ------------------------------------------------------------------
    std::cout << "-- 14. deleting reports exactly what it broke --" << std::endl;
    // ------------------------------------------------------------------
    {
        ParameterTable table;
        check(table.Add("base", "50", error), "add base = 50");
        check(table.Add("mid", "base * 2", error), "add mid = base * 2");
        check(table.Add("top", "mid + 1", error), "add top = mid + 1");
        check(table.Add("loner", "7", error), "add loner = 7");
        expectNear(ValueOf(table, "top"), 101.0, 1.0e-9, "top is 101");

        const std::vector<std::string> dependents = table.Dependents("base");
        check(dependents.size() == 2 && Contains(dependents, "mid") && Contains(dependents, "top"),
              "Dependents(base) is mid and top, through the chain: " + Join(dependents));
        check(table.Dependents("loner").empty(), "and loner has none");

        std::vector<std::string> stale;
        check(table.Remove("base", stale, error), "delete base");
        check(stale.size() == 2 && Contains(stale, "mid") && Contains(stale, "top"),
              "mid and top are reported stale: " + Join(stale));
        check(!Contains(stale, "loner"), "loner is not");
        expectNear(ValueOf(table, "loner"), 7.0, 1.0e-9, "and loner still resolves");

        // The two stale rows keep their text and say different things: one
        // lost a name, the other lost a value. Those are two different
        // repairs and the message has to tell them apart.
        check(table.Find("mid")->expression == "base * 2", "mid keeps its expression");
        check(table.Find("mid")->error.find("base") != std::string::npos,
              "mid: " + table.Find("mid")->error);
        check(table.Find("top")->error.find("mid") != std::string::npos,
              "top: " + table.Find("top")->error);
        check(table.Find("mid")->value == 0.0 && table.Find("mid")->DisplayText().empty(),
              "a broken row shows no number at all");

        // Putting it back fixes both, with no further edit.
        check(table.Add("base", "50", error), "add base = 50 again");
        expectNear(ValueOf(table, "top"), 101.0, 1.0e-9, "top is 101 again");
        check(table.InvalidParameters().empty(), "and the table is whole");
    }

    // ------------------------------------------------------------------
    std::cout << "-- 15. names the table will not take --" << std::endl;
    // ------------------------------------------------------------------
    {
        ParameterTable table;
        check(table.Add("width", "10", error), "add width");

        check(!table.Add("sin", "1", error), "\"sin\" is refused");
        check(error.find("expression language") != std::string::npos, error);
        check(!table.Add("PI", "1", error), "\"PI\" is refused");
        check(!table.Add("Cos", "1", error), "\"Cos\" is refused -- case is not a way round it");
        check(!table.Add("pow", "1", error), "\"pow\" is refused");
        check(!table.Add("2x", "1", error), "\"2x\" is refused");
        check(!table.Add("a b", "1", error), "\"a b\" is refused");
        check(!table.Add("", "1", error), "the empty name is refused");
        check(!table.Add("width", "1", error), "a duplicate is refused");
        check(!table.Add("Width", "1", error),
              "and so is one differing only in case: " + error);
        check(table.Count() == 1, "none of them got in");

        // A row can still be added with an expression that does not work;
        // it is the row that is wrong, not the edit.
        check(table.Add("broken", "1/0", error), "a row with a bad expression IS added");
        check(!table.Find("broken")->isValid
                  && table.Find("broken")->error.find("division by zero") != std::string::npos,
              "carrying its error: " + table.Find("broken")->error);
        check(table.Add("after", "broken + 1", error), "and one that depends on it");
        check(!table.Find("after")->isValid
                  && table.Find("after")->error.find("has an error") != std::string::npos,
              "says the input is broken, not missing: " + table.Find("after")->error);
    }

    // ------------------------------------------------------------------
    std::cout << "-- 16. a long chain, walked without recursion --" << std::endl;
    // ------------------------------------------------------------------
    {
        // Each row adds 1 to the one before, so the last one is also a
        // check that the ORDER was right all the way down: get it wrong
        // anywhere and the total is not n. Kept to a few hundred because
        // building a table a row at a time re-resolves the whole of it
        // each time, which is n-squared and is what a person typing does
        // anyway.
        const int kChain = 400;
        ParameterTable table;
        bool built = table.Add("p0", "0", error);
        for (int i = 1; i < kChain && built; ++i) {
            built = table.Add("p" + std::to_string(i),
                              "p" + std::to_string(i - 1) + " + 1", error);
        }
        check(built, "built a chain of " + std::to_string(kChain) + " parameters");
        expectNear(ValueOf(table, "p" + std::to_string(kChain - 1)), kChain - 1, 1.0e-9,
                   "the last one counted every step");

        // Close the chain into one enormous loop: it must report, not run
        // out of stack, and the message must stay readable.
        check(!table.SetExpression("p0", "p" + std::to_string(kChain - 1), error),
              "closing the chain into a loop is refused");
        check(error.find("circular reference") != std::string::npos
                  && error.find(std::to_string(kChain) + " parameters") != std::string::npos,
              "counting them rather than listing them: " + error);
        expectNear(ValueOf(table, "p" + std::to_string(kChain - 1)), kChain - 1, 1.0e-9,
                   "and the chain still resolves");
    }

    std::cout << std::endl;
    if (failures == 0) {
        std::cout << "ALL PARAMETER TESTS PASSED" << std::endl;
        return 0;
    }
    std::cout << failures << " PARAMETER TEST(S) FAILED" << std::endl;
    return 1;
}
