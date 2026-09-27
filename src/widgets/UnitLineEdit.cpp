#include "widgets/UnitLineEdit.h"

#include "core/Expression.h"
#include "core/ParameterTable.h"

#include <algorithm>
#include <cmath>

#include <QPalette>

namespace lcad {

UnitLineEdit::UnitLineEdit(UnitKind theKind, QWidget* theParent)
    : QLineEdit(theParent), myKind(theKind)
{
    myLengthUnit = DefaultLengthUnit();
    myValidTextColor = palette().color(QPalette::Text);

    setAlignment(Qt::AlignRight);

    connect(this, &QLineEdit::editingFinished, this, &UnitLineEdit::onEditingFinished);
    connect(this, &QLineEdit::textEdited, this, &UnitLineEdit::onTextEdited);

    Reformat();
    RefreshToolTip();
}

void UnitLineEdit::SetValue(double theInternalValue)
{
    if (myHasRange) {
        theInternalValue = std::clamp(theInternalValue, myMinimum, myMaximum);
    }
    myValue = theInternalValue;
    myExpression.clear();
    ApplyValidityStyling(true);
    SetAcceptable(true);
    Reformat();
}

void UnitLineEdit::SetExpression(const std::string& theExpression, double theInternalValue)
{
    if (theExpression.empty()) {
        SetValue(theInternalValue);
        return;
    }
    myExpression = theExpression;
    myValue = theInternalValue;
    ApplyValidityStyling(true);
    SetAcceptable(true);
    Reformat();
}

void UnitLineEdit::SetParameterTable(const ParameterTable* theTable)
{
    myTable = theTable;
    RefreshToolTip();
}

void UnitLineEdit::SetRange(double theMinimum, double theMaximum)
{
    myMinimum = theMinimum;
    myMaximum = theMaximum;
    myHasRange = theMaximum > theMinimum;
}

void UnitLineEdit::SetDisplayLengthUnit(LengthUnit theUnit)
{
    myLengthUnit = theUnit;
    Reformat();
    RefreshToolTip();
}

bool UnitLineEdit::EvaluateText(const std::string& theText, double& theValue,
                                std::string& theError) const
{
    const ExpressionResult result =
        myTable != nullptr
            ? myTable->EvaluateValue(theText, myKind, myLengthUnit, myAngleUnit)
            : ParameterTable::EvaluateValueWith(theText, myKind, myLengthUnit, myAngleUnit,
                                                TypedVariableLookup());
    theValue = result.value;
    theError = result.error;
    return result.ok;
}

bool UnitLineEdit::Read(const std::string& theText, double& theValue, std::string& theError) const
{
    if (IsLiteralOfWrongKind(theText, myKind, theError)) {
        return false;
    }
    return EvaluateText(theText, theValue, theError);
}

void UnitLineEdit::onTextEdited(const QString& theText)
{
    // Only tint while typing -- reformatting mid-keystroke would fight the
    // user for the cursor.
    const std::string typed = theText.trimmed().toStdString();
    std::string error;
    double value = 0.0;
    const bool readable = typed.empty() || Read(typed, value, error);
    ApplyValidityStyling(readable);
    // Live, so a dialog's OK comes back the moment the text is fixed rather
    // than only once the field loses focus.
    SetAcceptable(readable, QString::fromStdString(error));
}

void UnitLineEdit::onEditingFinished()
{
    // Enter or a focus change on text nobody touched is not an edit. Read
    // back, the shown text would also round the value to the four decimals
    // it displays -- and every commit is an undo step that clears Redo.
    if (text() == myShownText) {
        // Typed back to what it was: whatever was held red is gone.
        ApplyValidityStyling(true);
        SetAcceptable(true);
        return;
    }

    const std::string typed = text().trimmed().toStdString();
    const bool names = !ExpressionVariables(typed).empty();

    // The evaluator decides the number, always. Units.h's own reader is
    // only right for a single literal: it takes "5 m - 1 m" for a compound
    // measurement like 1'6" and ADDS the parts, 6000 mm where 4000 was
    // meant.
    double value = 0.0;
    std::string error;
    if (!Read(typed, value, error)) {
        if (myTable != nullptr && names) {
            // An expression that names a parameter is worth more than a
            // number to retype: hold it, red, with the reason in the
            // tooltip, and leave the value alone until it is fixed.
            ApplyValidityStyling(false);
            SetAcceptable(false, QString::fromStdString(error));
            return;
        }
        // Unreadable input reverts rather than silently becoming zero --
        // a wrong dimension is worse than a rejected keystroke.
        ApplyValidityStyling(true);
        SetAcceptable(true);
        Reformat();
        return;
    }

    // The field adopts a unit the moment one is typed into it -- but only
    // when what was typed is that one literal, which is when Units.h and
    // the evaluator agree on the number.
    const ParsedValue parsed = ParseValue(typed, myKind, myLengthUnit, myAngleUnit);
    if (!names && parsed.ok && parsed.hasExplicitUnit
        && std::fabs(parsed.value - value) <= 1.0e-9 * std::max(1.0, std::fabs(value))) {
        myLengthUnit = parsed.lengthUnit;
        myAngleUnit = parsed.angleUnit;
    }

    std::string expression;
    if (names) {
        // Clamping would leave the field showing an expression that does
        // not give the number it holds -- a lie that outlives the edit.
        // Refuse it instead, as a rebuild would.
        if (myHasRange && (value < myMinimum || value > myMaximum)) {
            ApplyValidityStyling(false);
            SetAcceptable(false, QString("Must be between %1 and %2, not %3")
                                     .arg(QString::fromStdString(
                                         FormatValue(myMinimum, myKind, myLengthUnit, myAngleUnit)))
                                     .arg(QString::fromStdString(
                                         FormatValue(myMaximum, myKind, myLengthUnit, myAngleUnit)))
                                     .arg(QString::fromStdString(
                                         FormatValue(value, myKind, myLengthUnit, myAngleUnit))));
            return;
        }
        expression = typed;
    } else if (myHasRange) {
        // A number, however it was written, clamps like a typed one.
        value = std::clamp(value, myMinimum, myMaximum);
    }

    const bool changed = (value != myValue) || (expression != myExpression);
    myValue = value;
    myExpression = expression;
    ApplyValidityStyling(true);
    SetAcceptable(true);
    Reformat();

    if (changed) {
        emit ValueChanged(myValue);
    }
}

void UnitLineEdit::Reformat()
{
    const QString shown =
        myExpression.empty()
            ? QString::fromStdString(FormatValue(myValue, myKind, myLengthUnit, myAngleUnit))
            : QString::fromStdString(myExpression);
    if (text() != shown) {
        const bool blocked = blockSignals(true);
        setText(shown);
        blockSignals(blocked);
    }
    myShownText = shown;
    RefreshToolTip();
}

void UnitLineEdit::SetAcceptable(bool theIsAcceptable, const QString& theWhy)
{
    myProblem = theIsAcceptable ? QString() : theWhy;
    RefreshToolTip();
    if (theIsAcceptable != myIsAcceptable) {
        myIsAcceptable = theIsAcceptable;
        emit AcceptableChanged(myIsAcceptable);
    }
}

void UnitLineEdit::RefreshToolTip()
{
    if (!myProblem.isEmpty()) {
        setToolTip(myProblem);
        return;
    }
    if (!myExpression.empty()) {
        // The field shows the expression; this is where the number lives.
        setToolTip(QString::fromStdString(myExpression) + " = "
                   + QString::fromStdString(
                       FormatValue(myValue, myKind, myLengthUnit, myAngleUnit)));
        return;
    }
    QString help;
    if (myKind == UnitKind::Length) {
        help = "Type a plain number for " + QString::fromStdString(SymbolOf(myLengthUnit))
               + ", or include a unit: 12.9in, 1/2\", 1'6\", 2.5cm, 1m";
    } else if (myKind == UnitKind::Angle) {
        help = "Type a plain number for degrees, or include a unit: 90deg, 1.5708rad";
    }
    if (myTable != nullptr) {
        help += (help.isEmpty() ? "" : "\n")
                + QString("Or an expression over your parameters, e.g. plate_t * 2");
    }
    setToolTip(help);
}

void UnitLineEdit::ApplyValidityStyling(bool theIsValid)
{
    // Restore the theme's own colour rather than a hardcoded one, so this
    // stays right in both light and dark.
    QPalette fieldPalette = palette();
    fieldPalette.setColor(QPalette::Text,
                          theIsValid ? myValidTextColor : QColor(0xE5, 0x6B, 0x6B));
    setPalette(fieldPalette);
}

} // namespace lcad
