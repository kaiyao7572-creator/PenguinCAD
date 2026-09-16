#include "core/Command.h"
#include "core/Registration.h"
#include "core/Units.h"

#include <memory>

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QMainWindow>
#include <QStatusBar>
#include <QString>

namespace lcad {

namespace {

// Changes what a bare, unit-less number means everywhere: type 5 with the
// document set to inches and you get five inches. Fields that were given an
// explicit unit keep showing that unit regardless -- this only moves the
// default.
class UnitsCommand : public Command
{
public:
    std::string Id() const override { return "view.units"; }
    std::string Title() const override { return "Units"; }
    std::string Group() const override { return "View"; }
    std::string Section() const override { return "Show"; }
    std::string Icon() const override { return "📏"; }
    std::string Description() const override
    {
        return "Set the document's default unit. Any field still accepts an explicit "
               "unit, e.g. 12.9in, 1/2\", 1'6\", 2.5cm";
    }

    void Execute(CommandContext& theContext) override
    {
        QDialog dialog(theContext.parent);
        dialog.setWindowTitle("Units");

        QFormLayout* form = new QFormLayout(&dialog);

        QComboBox* unitBox = new QComboBox(&dialog);
        const std::vector<LengthUnit> units = SelectableLengthUnits();
        int current = 0;
        for (std::size_t i = 0; i < units.size(); ++i) {
            unitBox->addItem(QString::fromStdString(NameOf(units[i])));
            if (units[i] == DefaultLengthUnit()) {
                current = static_cast<int>(i);
            }
        }
        unitBox->setCurrentIndex(current);
        form->addRow("Default unit", unitBox);

        QLabel* hint = new QLabel(
            "This is only the default for numbers typed without a unit.\n"
            "Any field still accepts 12.9in, 1/2\", 1'6\", 2.5cm or 1m,\n"
            "and will keep displaying whichever unit you typed.",
            &dialog);
        hint->setWordWrap(true);
        form->addRow(hint);

        QDialogButtonBox* buttons =
            new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        form->addRow(buttons);

        if (dialog.exec() != QDialog::Accepted) {
            return;
        }

        const int chosen = unitBox->currentIndex();
        if (chosen < 0 || chosen >= static_cast<int>(units.size())) {
            return;
        }
        SetDefaultLengthUnit(units[static_cast<std::size_t>(chosen)]);

        if (QMainWindow* window = qobject_cast<QMainWindow*>(theContext.parent)) {
            window->statusBar()->showMessage(
                "Default unit is now " + QString::fromStdString(NameOf(units[chosen]))
                + ". Fields already showing another unit keep it.");
        }
    }

private:
    static std::string NameOf(LengthUnit theUnit)
    {
        switch (theUnit) {
            case LengthUnit::Millimeter: return "Millimeter (mm)";
            case LengthUnit::Centimeter: return "Centimeter (cm)";
            case LengthUnit::Meter:      return "Meter (m)";
            case LengthUnit::Inch:       return "Inch (in)";
            case LengthUnit::Foot:       return "Foot (ft)";
        }
        return "Millimeter (mm)";
    }
};

} // namespace

void RegisterUnitsCommand(CommandRegistry& theRegistry)
{
    theRegistry.Add(std::make_unique<UnitsCommand>());
}

} // namespace lcad
