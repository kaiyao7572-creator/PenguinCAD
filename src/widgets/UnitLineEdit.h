#pragma once

#include "core/Units.h"

#include <QColor>
#include <QLineEdit>

namespace lcad {

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

    LengthUnit DisplayLengthUnit() const { return myLengthUnit; }
    void SetDisplayLengthUnit(LengthUnit theUnit);

    AngleUnit DisplayAngleUnit() const { return myAngleUnit; }

signals:
    // Emitted when the value actually changes, in internal units.
    void ValueChanged(double theInternalValue);

private slots:
    void onEditingFinished();
    void onTextEdited(const QString& theText);

private:
    void Reformat();
    void ApplyValidityStyling(bool theIsValid);

    UnitKind   myKind = UnitKind::Length;
    LengthUnit myLengthUnit = LengthUnit::Millimeter;
    AngleUnit  myAngleUnit = AngleUnit::Degree;

    double myValue = 0.0;
    double myMinimum = 0.0;
    double myMaximum = 0.0;
    bool   myHasRange = false;

    // The palette's normal text colour, captured up front so invalid
    // input can be tinted red and then restored correctly in any theme.
    QColor myValidTextColor;
};

} // namespace lcad
