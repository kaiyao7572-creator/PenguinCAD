// Functional test of the unit-aware input field: does typing "12.9in"
// actually convert AND flip the field into inches, the way the user asked?
#include "widgets/UnitLineEdit.h"

#include <QApplication>
#include <QSignalSpy>

#include <cmath>
#include <iostream>

using namespace lcad;

static int failures = 0;

static void check(bool ok, const std::string& what)
{
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << what << std::endl;
    if (!ok) ++failures;
}

// Simulate a user typing into the field and tabbing away.
static void typeInto(UnitLineEdit& theField, const QString& theText)
{
    theField.setText(theText);
    emit theField.editingFinished();
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);

    UnitLineEdit field(UnitKind::Length);

    std::cout << "-- starts in the document default (mm) --" << std::endl;
    field.SetValue(20.0);
    check(field.text() == "20 mm", "shows \"20 mm\", got \"" + field.text().toStdString() + "\"");
    check(field.DisplayLengthUnit() == LengthUnit::Millimeter, "display unit is mm");

    std::cout << "-- a bare number stays in the current unit --" << std::endl;
    typeInto(field, "35");
    check(std::fabs(field.Value() - 35.0) < 1e-9, "\"35\" -> 35 mm internally");

    std::cout << "-- THE HEADLINE: typing inches converts and switches the field --" << std::endl;
    QSignalSpy spy(&field, &UnitLineEdit::ValueChanged);
    typeInto(field, "12.9inches");
    check(std::fabs(field.Value() - 12.9 * 25.4) < 1e-6,
          "\"12.9inches\" -> 327.66 mm internally");
    check(field.DisplayLengthUnit() == LengthUnit::Inch, "field switched itself to inches");
    check(field.text() == "12.9 in",
          "redisplays as \"12.9 in\", got \"" + field.text().toStdString() + "\"");
    check(spy.count() == 1, "emitted ValueChanged once");

    std::cout << "-- it STAYS in inches: a bare number now means inches --" << std::endl;
    typeInto(field, "2");
    check(std::fabs(field.Value() - 50.8) < 1e-6, "\"2\" now means 2 in = 50.8 mm");
    check(field.text() == "2 in", "still displaying inches");

    std::cout << "-- switching back to metric works too --" << std::endl;
    typeInto(field, "10mm");
    check(std::fabs(field.Value() - 10.0) < 1e-9, "\"10mm\" -> 10 mm");
    check(field.DisplayLengthUnit() == LengthUnit::Millimeter, "field switched back to mm");

    std::cout << "-- fractions and feet --" << std::endl;
    typeInto(field, "1/2in");
    check(std::fabs(field.Value() - 12.7) < 1e-9, "\"1/2in\" -> 12.7 mm");
    typeInto(field, "1'6\"");
    check(std::fabs(field.Value() - 457.2) < 1e-9, "\"1'6\\\"\" -> 457.2 mm");

    std::cout << "-- garbage reverts instead of becoming zero --" << std::endl;
    const double before = field.Value();
    typeInto(field, "banana");
    check(std::fabs(field.Value() - before) < 1e-12, "invalid input kept the previous value");

    std::cout << "-- range clamping --" << std::endl;
    UnitLineEdit limited(UnitKind::Length);
    limited.SetRange(0.0, 100.0);
    typeInto(limited, "500");
    check(std::fabs(limited.Value() - 100.0) < 1e-9, "500 clamps to the 100 max");

    std::cout << "-- angles are separate --" << std::endl;
    UnitLineEdit angle(UnitKind::Angle);
    typeInto(angle, "90");
    check(std::fabs(angle.Value() - 90.0) < 1e-9, "\"90\" -> 90 deg");
    typeInto(angle, "1.5707963268rad");
    check(std::fabs(angle.Value() - 90.0) < 1e-6, "\"1.5708rad\" -> 90 deg");

    std::cout << "-- the document default drives new fields --" << std::endl;
    SetDefaultLengthUnit(LengthUnit::Inch);
    UnitLineEdit imperial(UnitKind::Length);
    imperial.SetValue(25.4);
    check(imperial.text() == "1 in", "new field honours the inch default, got \""
                                         + imperial.text().toStdString() + "\"");
    SetDefaultLengthUnit(LengthUnit::Millimeter);

    std::cout << (failures == 0 ? "\nALL WIDGET TESTS PASSED\n" : "\nFAILURES\n");
    return failures == 0 ? 0 : 1;
}
