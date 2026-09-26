#pragma once

#include "core/Units.h"

#include <functional>
#include <string>
#include <vector>

namespace lcad {

// What evaluating an expression produced.
//
// Errors come back as a value, never as an exception: an expression is
// user input, a bad one is ordinary rather than exceptional, and every
// caller is on a path -- rebuild, properties panel, parameter dialog --
// that must not unwind. A failed evaluation also leaves `value` at zero
// rather than a NaN, because a NaN entering a gp_Pnt propagates through
// OCCT and surfaces much later as an unexplainable empty shape.
struct ExpressionResult
{
    bool        ok = false;
    double      value = 0.0;
    std::string error;
};

// How a name in an expression becomes a number. Return false for a name
// you don't know: that becomes a readable error rather than a silent
// zero standing in for a parameter the user thought they had defined.
using VariableLookup = std::function<bool(const std::string& theName, double& theValue)>;

// Evaluate "plate_width / 4", "2 * sin(30) + 1/2in", "(a + b) * PI".
//
// Supports + - * /, unary minus, parentheses, the usual precedence,
// decimal literals with an optional unit suffix, named variables, and
// the functions ReservedExpressionNames() lists. Function names and PI
// are matched without case, since a parameter may not take one of those
// names either way.
//
// ANGLES ARE IN DEGREES, going in and coming out: sin(30) is 0.5 and
// asin(0.5) is 30, exactly as they are in Fusion and exactly as this
// document stores an angle. Radians appear only inside a literal that
// says so ("1.5708rad"), which core/Units.h converts on the way in.
//
// Never throws, whatever it is handed. NaN and infinity are reported
// rather than returned: pow(0,-1), sqrt(-1), tan(90) and a division by
// zero all come back as errors, because a number that is not a number
// reaches OCCT and surfaces days later as an empty shape.
//
// A literal carrying a unit ("1in", "90deg", "1'6\"") is converted to
// this document's internal units -- millimetres for lengths, degrees for
// angles -- by core/Units.h, the same parser the input fields use, so
// there is exactly one place in the app that knows what "1/2in" means.
// A bare literal is left alone: the 4 in "plate_width / 4" is a count,
// and scaling it by whatever unit the field happens to display would
// quietly produce geometry 25.4x wrong. Applying a unit to a value the
// user typed as a plain number is therefore the caller's job, and
// ParameterTable does it only where it is unambiguous.
ExpressionResult EvaluateExpression(const std::string&    theText,
                                    const VariableLookup& theLookup = VariableLookup());

// Every name the expression reads, in order of first appearance, with
// function names and constants left out. This is what a dependency graph
// is built from.
std::vector<std::string> ExpressionVariables(const std::string& theText);

// True when the expression is pure dimensionless arithmetic: no names, no
// unit written into any literal. "1", "1/2", "100/4" and "sin(30)*2" are;
// "plate_width/4" and "10mm+1" are not.
//
// This is the question "may the field's own unit be applied to this?",
// and only the tokens can answer it. Nothing in a dimensionless
// expression is already a length or an angle, so reading the whole of it
// in inches is exactly what a user means by typing 1 into an inch field.
// The moment a name or a unit appears the expression is already in
// internal units and scaling it again would be a silent factor of 25.4.
bool IsUnitlessExpression(const std::string& theText);

// Rewrite every reference to theOldName as theNewName, leaving spacing,
// unit suffixes and everything else exactly as the user typed it. Works
// on tokens rather than text, so a parameter named "mm" cannot corrupt
// the "10mm" sitting next to it.
std::string RenameExpressionVariable(const std::string& theText,
                                     const std::string& theOldName,
                                     const std::string& theNewName);

// True when the text is shaped like a name the evaluator would read:
// a letter or underscore, then letters, digits or underscores. The rule
// lives here rather than with the parameter table because the grammar
// that has to accept a name is what decides what a name may look like.
bool IsExpressionIdentifier(const std::string& theText);

// True for a name the evaluator already gives a meaning -- a function or
// PI. Case is ignored, so "Sin" and "PI" are as reserved as "sin" and
// "pi"; a parameter may not take one of these or it would shadow
// something and never be readable again.
bool IsReservedExpressionName(const std::string& theName);

// True, with theError saying so, when the whole of theText is one literal
// of the WRONG kind for a field of theKind: "2 in" typed where an angle
// goes, "90 deg" where a length goes, any unit where a count goes. The
// evaluator converts every unit to internal units and does no dimension
// analysis, so without this "2 in" in an angle field would quietly be 50.8
// degrees. Only a lone literal is judged -- "10 mm * sin(30 deg)" mixes
// kinds legitimately, and telling those apart needs real dimensional
// analysis this engine does not have.
bool IsLiteralOfWrongKind(const std::string& theText, UnitKind theKind, std::string& theError);

// Every reserved word, for a dialog that wants to list what it can offer.
const std::vector<std::string>& ReservedExpressionNames();

} // namespace lcad
