#include "sketch/SketchDialogs.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QSpinBox>
#include <QString>

namespace lcad {
namespace SketchDialogs {

namespace {

// Same shape as SketchCommands.cpp's plane dialog: a form, an OK/Cancel
// row, and nothing else.
QDialogButtonBox* AddButtons(QDialog& theDialog, QFormLayout& theForm)
{
    QDialogButtonBox* buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &theDialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &theDialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &theDialog, &QDialog::reject);
    theForm.addRow(buttons);
    return buttons;
}

QSpinBox* AddCount(QDialog& theDialog, QFormLayout& theForm, const QString& theLabel,
                   int theValue, int theMinimum)
{
    QSpinBox* box = new QSpinBox(&theDialog);
    box->setRange(theMinimum, 512);
    box->setValue(theValue);
    theForm.addRow(theLabel, box);
    return box;
}

} // namespace

bool AskPolygonSides(QWidget* theParent, int& theSides)
{
    QDialog dialog(theParent);
    dialog.setWindowTitle(QStringLiteral("Polygon"));

    QFormLayout* form = new QFormLayout(&dialog);
    QSpinBox* sides = AddCount(dialog, *form, QStringLiteral("Sides"), theSides, 3);
    sides->setMaximum(64);

    QLabel* hint = new QLabel(
        QStringLiteral("[ and ] change the side count while you draw."), &dialog);
    hint->setWordWrap(true);
    form->addRow(hint);

    AddButtons(dialog, *form);

    if (dialog.exec() != QDialog::Accepted) {
        return false;
    }
    theSides = sides->value();
    return true;
}

bool AskRectangularPattern(QWidget* theParent, int& theAcross, int& theDown)
{
    QDialog dialog(theParent);
    dialog.setWindowTitle(QStringLiteral("Rectangular Pattern"));

    QFormLayout* form = new QFormLayout(&dialog);
    QSpinBox* across = AddCount(dialog, *form, QStringLiteral("Quantity 1"), theAcross, 1);
    QSpinBox* down = AddCount(dialog, *form, QStringLiteral("Quantity 2"), theDown, 1);

    QLabel* hint =
        new QLabel(QStringLiteral("Drag in the viewport to set the spacing."), &dialog);
    hint->setWordWrap(true);
    form->addRow(hint);

    AddButtons(dialog, *form);

    if (dialog.exec() != QDialog::Accepted) {
        return false;
    }
    theAcross = across->value();
    theDown = down->value();
    return theAcross > 1 || theDown > 1;
}

bool AskCircularPattern(QWidget* theParent, int& theCount, double& theAngleDegrees)
{
    QDialog dialog(theParent);
    dialog.setWindowTitle(QStringLiteral("Circular Pattern"));

    QFormLayout* form = new QFormLayout(&dialog);
    QSpinBox* count = AddCount(dialog, *form, QStringLiteral("Quantity"), theCount, 2);

    QDoubleSpinBox* angle = new QDoubleSpinBox(&dialog);
    angle->setRange(-360.0, 360.0);
    angle->setDecimals(2);
    angle->setSingleStep(15.0);
    angle->setSuffix(QStringLiteral(" deg"));
    angle->setValue(theAngleDegrees);
    form->addRow(QStringLiteral("Total angle"), angle);

    QLabel* hint =
        new QLabel(QStringLiteral("Click in the viewport to place the centre."), &dialog);
    hint->setWordWrap(true);
    form->addRow(hint);

    AddButtons(dialog, *form);

    if (dialog.exec() != QDialog::Accepted) {
        return false;
    }
    theCount = count->value();
    theAngleDegrees = angle->value();
    return true;
}

bool AskDimensionValue(QWidget*       theParent,
                       const QString& theTitle,
                       bool           theIsAngle,
                       double&        theValue)
{
    QDialog dialog(theParent);
    dialog.setWindowTitle(theTitle);

    QFormLayout* form = new QFormLayout(&dialog);

    QDoubleSpinBox* value = new QDoubleSpinBox(&dialog);
    value->setDecimals(theIsAngle ? 2 : 3);
    value->setRange(theIsAngle ? -360.0 : -1.0e6, theIsAngle ? 360.0 : 1.0e6);
    value->setSingleStep(theIsAngle ? 5.0 : 1.0);
    value->setSuffix(theIsAngle ? QStringLiteral(" deg") : QStringLiteral(" mm"));
    value->setValue(theValue);
    value->selectAll();
    form->addRow(QStringLiteral("Value"), value);

    QLabel* hint = new QLabel(
        QStringLiteral("This becomes a driving dimension: the sketch moves to match it."),
        &dialog);
    hint->setWordWrap(true);
    form->addRow(hint);

    AddButtons(dialog, *form);

    if (dialog.exec() != QDialog::Accepted) {
        return false;
    }
    theValue = value->value();
    return true;
}

} // namespace SketchDialogs
} // namespace lcad
