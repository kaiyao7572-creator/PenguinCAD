#include "widgets/UnitLineEdit.h"

#include <algorithm>

#include <QPalette>

namespace lcad {

UnitLineEdit::UnitLineEdit(UnitKind theKind, QWidget* theParent)
    : QLineEdit(theParent), myKind(theKind)
{
    myLengthUnit = DefaultLengthUnit();
    myValidTextColor = palette().color(QPalette::Text);

    setAlignment(Qt::AlignRight);
    if (myKind == UnitKind::Length) {
        setToolTip("Type a plain number for " + QString::fromStdString(SymbolOf(myLengthUnit))
                   + ", or include a unit: 12.9in, 1/2\", 1'6\", 2.5cm, 1m");
    } else if (myKind == UnitKind::Angle) {
        setToolTip("Type a plain number for degrees, or include a unit: 90deg, 1.5708rad");
    }

    connect(this, &QLineEdit::editingFinished, this, &UnitLineEdit::onEditingFinished);
    connect(this, &QLineEdit::textEdited, this, &UnitLineEdit::onTextEdited);

    Reformat();
}

void UnitLineEdit::SetValue(double theInternalValue)
{
    if (myHasRange) {
        theInternalValue = std::clamp(theInternalValue, myMinimum, myMaximum);
    }
    if (myValue == theInternalValue) {
        Reformat();
        return;
    }
    myValue = theInternalValue;
    Reformat();
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
}

void UnitLineEdit::onTextEdited(const QString& theText)
{
    // Only tint while typing -- reformatting mid-keystroke would fight the
    // user for the cursor.
    const ParsedValue parsed =
        ParseValue(theText.toStdString(), myKind, myLengthUnit, myAngleUnit);
    ApplyValidityStyling(parsed.ok || theText.trimmed().isEmpty());
}

void UnitLineEdit::onEditingFinished()
{
    const ParsedValue parsed =
        ParseValue(text().toStdString(), myKind, myLengthUnit, myAngleUnit);

    if (!parsed.ok) {
        // Unreadable input reverts rather than silently becoming zero --
        // a wrong dimension is worse than a rejected keystroke.
        ApplyValidityStyling(true);
        Reformat();
        return;
    }

    // The field adopts a unit the moment one is typed into it.
    if (parsed.hasExplicitUnit) {
        myLengthUnit = parsed.lengthUnit;
        myAngleUnit = parsed.angleUnit;
    }

    double value = parsed.value;
    if (myHasRange) {
        value = std::clamp(value, myMinimum, myMaximum);
    }

    const bool changed = (value != myValue);
    myValue = value;
    ApplyValidityStyling(true);
    Reformat();

    if (changed) {
        emit ValueChanged(myValue);
    }
}

void UnitLineEdit::Reformat()
{
    const QString shown =
        QString::fromStdString(FormatValue(myValue, myKind, myLengthUnit, myAngleUnit));
    if (text() != shown) {
        const bool blocked = blockSignals(true);
        setText(shown);
        blockSignals(blocked);
    }
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
