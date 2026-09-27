#pragma once

#include "core/Expression.h"
#include "core/Units.h"

#include <cstddef>
#include <string>
#include <vector>

namespace lcad {

// One row of Fusion's MODIFY > Change Parameters dialog: a name, the
// expression behind it, what the number means, and a comment.
//
// It is called UserParameter rather than Parameter because core/Feature.h
// already owns that word for the reflection struct the properties panel
// edits features through. They are different things: a Parameter is one
// editable slot ON a feature, a UserParameter is a named value the whole
// document can refer to, and a feature's slot ends up holding an
// expression that READS user parameters.
struct UserParameter
{
    std::string name;         // "plate_width" -- an expression identifier
    std::string expression;   // "hole_dia * 4", exactly as the user typed it
    std::string comment;      // Fusion's last column; never interpreted

    // What the number means, which decides both how a bare literal is read
    // and how the value is shown back.
    UnitKind   kind = UnitKind::Length;
    LengthUnit lengthUnit = LengthUnit::Millimeter;
    AngleUnit  angleUnit = AngleUnit::Degree;

    // Filled in by Resolve(). `value` is in internal units -- millimetres
    // for Length, degrees for Angle, as written for Unitless -- so a
    // feature can use it without knowing what the row displays.
    //
    // A row that failed keeps its last good value out of `value` (it is
    // reset to zero) and explains itself in `error`. A stale number that
    // still looks plausible is the thing this table exists to prevent.
    double      value = 0.0;
    bool        isValid = false;
    std::string error;

    // The value as the row's own unit shows it, e.g. "3.937 in".
    std::string DisplayText(int theMaxDecimals = 4) const;
};

// The document's named parameters, in the order the user made them.
//
// Resolution runs the whole table in dependency order, so a change to one
// parameter propagates to everything downstream in a single pass, which is
// what makes `hole_dia = plate_width / 4` mean anything.
//
// Two invariants hold at all times, and every mutator preserves them:
//
//  1. THE TABLE NEVER CONTAINS A CYCLE. An edit that would create one is
//     refused and the table is left exactly as it was, with an error
//     naming the parameters that form the loop. A cycle is the one kind
//     of bad state that cannot be shown in a row and fixed later, because
//     there is no order in which to compute the rows at all.
//  2. A ROW IS EITHER VALID OR CARRIES AN ERROR. Removing a parameter
//     other rows use is allowed -- refusing would leave the user unable to
//     take a model apart -- but those rows go invalid and say why, and
//     Remove() hands their names back so the caller can point at them.
class ParameterTable
{
public:
    // ---- reading ----

    const std::vector<UserParameter>& Parameters() const { return myParameters; }
    std::size_t Count() const { return myParameters.size(); }
    bool IsEmpty() const { return myParameters.empty(); }

    // Position of theName, or -1. Exact match: names differing only in
    // case cannot both exist (Add refuses the second), so an exact lookup
    // can never find the wrong one.
    int IndexOf(const std::string& theName) const;
    const UserParameter* Find(const std::string& theName) const;

    // Rows that did not resolve, in table order. Empty means the whole
    // table is good.
    std::vector<std::string> InvalidParameters() const;

    // Every parameter that would break without theName, directly or
    // through a chain, in table order.
    std::vector<std::string> Dependents(const std::string& theName) const;

    // ---- editing ----
    //
    // Each of these validates, applies, and re-resolves the whole table.
    // They return false with theError set and the table untouched when the
    // edit itself is bad (a malformed name, a duplicate, a cycle). An
    // expression that merely fails to evaluate is NOT a bad edit: it is
    // accepted, and the row carries the error.

    bool Add(const UserParameter& theParameter, std::string& theError);

    // Convenience for the common case: a length in millimetres.
    bool Add(const std::string& theName, const std::string& theExpression,
             std::string& theError);

    bool SetExpression(const std::string& theName, const std::string& theExpression,
                       std::string& theError);
    bool SetComment(const std::string& theName, const std::string& theComment,
                    std::string& theError);

    // Change what the row's number means. The expression text is left
    // alone, so a row reading "1" and shown in inches becomes 25.4mm --
    // see the bare-literal rule below.
    bool SetUnit(const std::string& theName, UnitKind theKind,
                 LengthUnit theLengthUnit, AngleUnit theAngleUnit, std::string& theError);

    // Rename, rewriting every expression that mentioned the old name so
    // the model keeps working -- the same thing Fusion does. The names of
    // the rows whose text was rewritten come back in theRewritten.
    bool Rename(const std::string& theOldName, const std::string& theNewName,
                std::vector<std::string>& theRewritten, std::string& theError);

    // Delete. Rows that referred to it keep their text and go invalid;
    // the ones that were good until now come back in theStale, in table
    // order, so the caller can say what it just broke.
    bool Remove(const std::string& theName, std::vector<std::string>& theStale,
                std::string& theError);

    void Clear();

    // Replace every row and resolve, WITHOUT the edit-time checks: a cycle
    // or a duplicate lands in the rows' errors instead of being refused.
    // For the document's evaluation table -- the user parameters plus one
    // row per model parameter -- which is rebuilt, not edited.
    void Assign(std::vector<UserParameter> theRows);

    // Take each row's value, validity and error from the row of the same
    // name in theResolved, so user parameters that read model parameters
    // (hole = d3 / 2) show what the whole design resolves them to.
    void AdoptResults(const ParameterTable& theResolved);

    // ---- evaluating ----

    // Recompute every row in dependency order. Returns true when they all
    // resolved. Called automatically by every mutator; public because a
    // caller that loaded a document needs it once at the end.
    bool Resolve();

    // Read parameter values by name. Only rows that resolved answer, so an
    // expression can never be built on a number that is not really there.
    //
    // The callback holds this table by pointer: it is valid for as long as
    // the table is.
    TypedVariableLookup Lookup() const;

    // Evaluate an expression that is not itself a row -- a feature's
    // distance, a dialog field -- against this table's parameters.
    //
    // THE BARE-LITERAL RULE, which lives here because it is a question
    // about a FIELD rather than about arithmetic: an expression that is
    // nothing but a number means that number in theKind's given unit, so
    // typing 1 into a field showing inches is one inch, exactly as it is
    // in UnitLineEdit. The moment the expression does arithmetic the unit
    // can no longer be applied to the whole of it -- half of 100mm is not
    // half of 100in -- so it is evaluated in internal units instead, with
    // any unit written into a literal ("1in", "90deg") converted by
    // core/Units.h, and the display unit only decides how the answer is
    // shown. Anything else would make "plate_width / 2" 25.4x wrong the
    // day someone switched a field to inches.
    ExpressionResult EvaluateValue(const std::string& theText, UnitKind theKind,
                                   LengthUnit theLengthUnit = LengthUnit::Millimeter,
                                   AngleUnit  theAngleUnit = AngleUnit::Degree) const;

    // Same, for a plain count or ratio.
    ExpressionResult Evaluate(const std::string& theText) const;

    // EvaluateValue against a lookup of the caller's own. Resolving a
    // table builds its lookup once and comes through here, rather than
    // making a fresh one per row.
    static ExpressionResult EvaluateValueWith(const std::string& theText, UnitKind theKind,
                                              LengthUnit            theLengthUnit,
                                              AngleUnit             theAngleUnit,
                                              const TypedVariableLookup& theLookup);

    // ---- names ----

    // Is theName usable as a parameter name here? Rejects the empty
    // string, anything the expression grammar would not read back as a
    // name, the reserved words (functions and PI), and a name another row
    // already has -- compared without case, because two parameters
    // differing only in case are a trap for whoever reads the model next.
    //
    // theIgnoredIndex lets a rename skip the row being renamed; pass -1.
    bool IsNameAvailable(const std::string& theName, int theIgnoredIndex,
                         std::string& theError) const;

private:
    // Finish a mutator: re-resolve, or put theSnapshot back and report the
    // loop if the edit created one. Every mutator ends here, which is what
    // keeps invariant 1 true.
    bool FinishEdit(const std::vector<UserParameter>& theSnapshot, std::string& theError);

    // Dependencies of row i on other rows, as indices. Names that are not
    // rows of this table are left out: the evaluator reports those, with
    // the name in the message.
    std::vector<std::vector<std::size_t>> BuildGraph() const;

    // Dependency order, plus a readable "a -> b -> c -> a" for each cycle
    // found, keyed by the index of every row caught in one. Returns false
    // when the table contains a cycle; theOrder is still usable for the
    // rows that are not part of one.
    //
    // Takes the graph rather than building its own, so that resolving a
    // table walks the names once instead of once per row.
    bool Order(const std::vector<std::vector<std::size_t>>& theGraph,
               std::vector<std::size_t>&                    theOrder,
               std::vector<std::string>&                    theCycleErrors) const;

    // The resolution pass itself, over a graph and an order already
    // worked out, so an edit does not walk the names twice.
    bool ResolveWith(const std::vector<std::vector<std::size_t>>& theGraph,
                     const std::vector<std::size_t>&              theOrder,
                     const std::vector<std::string>&              theCycleErrors);

    std::vector<UserParameter> myParameters;
};

} // namespace lcad
