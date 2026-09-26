#pragma once

#include "core/Units.h"

#include <QColor>
#include <QLineEdit>

#include <string>

namespace lcad {

class ParameterTable;

// A numeric field that speaks units.
//
// It always stores its value internally in the base unit (millimetres for
// lengths, degrees for angles), so model code never deals with unit
// conversion. What changes is only what the user sees and types:
//
//   type "20"        -> 20 of whatever unit the field is showing
//   type "12.9in"    -> 327.66 mm internally, AND the field switches
//                       itself to inches and keeps showing inches
//   type "1'6\""     -> 457.2 mm
//
// That last behaviour is the point: a field adopts whatever unit you
// typed into it, so you can work in inches on one dimension without
// changing the document default.
//
// It also takes arithmetic ("3 * 4", "1/2 in + 2 mm"), and, given the
// document's parameters, an EXPRESSION over them ("plate_t * 2"). A field
// driven by an expression shows the expression, not the number, and the
// tooltip says what it comes to -- the way Fusion's fields behave.
class UnitLineEdit : public QLineEdit
{
    Q_OBJECT

public:
    explicit UnitLineEdit(UnitKind theKind, QWidget* theParent = nullptr);

    // Value in internal units: millimetres for Length, degrees for Angle.
    double Value() const { return myValue; }
    void SetValue(double theInternalValue);

    // Clamp typed input. Defaults to no limit.
    void SetRange(double theMinimum, double theMaximum);

    UnitKind Kind() const { return myKind; }

    // Let the field take expressions over these parameters. Without a
    // table it still takes arithmetic on numbers, which names nothing.
    // The table must outlive the field; a Document's does.
    void SetParameterTable(const ParameterTable* theTable);

    // The expression driving the value, or empty for a plain number. Only
    // an expression that names a parameter is kept as one: "3 * 4" has
    // nothing that can change, so it becomes the number 12.
    const std::string& Expression() const { return myExpression; }

    // Show a driven value: the field displays theExpression and reports
    // theInternalValue. An empty expression is the same as SetValue.
    void SetExpression(const std::string& theExpression, double theInternalValue);

    // False while the field holds text it could not read -- an expression
    // naming a parameter that does not exist, say. The typed text stays,
    // in red, so it can be fixed rather than retyped; a dialog should
    // refuse OK meanwhile.
    bool IsAcceptable() const { return myIsAcceptable; }

    LengthUnit DisplayLengthUnit() const { return myLengthUnit; }
    void SetDisplayLengthUnit(LengthUnit theUnit);

    AngleUnit DisplayAngleUnit() const { return myAngleUnit; }

signals:
    // Emitted when the value actually changes, in internal units -- or
    // when the expression driving it does, even to one that gives the same
    // number, since that changes what the value will do later.
    void ValueChanged(double theInternalValue);

    void AcceptableChanged(bool theIsAcceptable);

private slots:
    void onEditingFinished();
    void onTextEdited(const QString& theText);

private:
    void Reformat();
    void ApplyValidityStyling(bool theIsValid);
    void SetAcceptable(bool theIsAcceptable, const QString& theWhy = QString());
    void RefreshToolTip();

    // Evaluate text that is not a plain value. False with theError when
    // it does not evaluate against the table (or, without one, at all).
    bool EvaluateText(const std::string& theText, double& theValue, std::string& theError) const;

    // EvaluateText, refusing first a lone literal of the wrong kind -- "2 in"
    // in an angle field -- which the evaluator would happily convert.
    bool Read(const std::string& theText, double& theValue, std::string& theError) const;

    UnitKind   myKind = UnitKind::Length;
    LengthUnit myLengthUnit = LengthUnit::Millimeter;
    AngleUnit  myAngleUnit = AngleUnit::Degree;

    const ParameterTable* myTable = nullptr;
    std::string myExpression;
    bool myIsAcceptable = true;
    // The text Reformat last put in the field: finishing an edit that left
    // it exactly so is not an edit.
    QString myShownText;
    QString myProblem;

    double myValue = 0.0;
    double myMinimum = 0.0;
    double myMaximum = 0.0;
    bool   myHasRange = false;

    // The palette's normal text colour, captured up front so invalid
    // input can be tinted red and then restored correctly in any theme.
    QColor myValidTextColor;
};

} // namespace lcad
