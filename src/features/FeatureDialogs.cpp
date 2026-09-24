#include "features/FeatureDialogs.h"

#include "core/Feature.h"

#include <algorithm>

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>

#include "widgets/UnitLineEdit.h"
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QWidget>

namespace lcad {

DialogField DialogField::Number(QString theLabel,
                                double  theValue,
                                double  theMinimum,
                                QString theSuffix)
{
    DialogField field;
    field.kind = Kind::Number;
    field.label = std::move(theLabel);
    field.value = theValue;
    field.minimum = theMinimum;
    field.suffix = std::move(theSuffix);
    return field;
}

DialogField DialogField::Choice(QString theLabel, QStringList theChoices, int theChoice)
{
    DialogField field;
    field.kind = Kind::Choice;
    field.label = std::move(theLabel);
    field.choices = std::move(theChoices);
    field.choice = theChoice;
    return field;
}

DialogField DialogField::Toggle(QString theLabel, bool theValue)
{
    DialogField field;
    field.kind = Kind::Toggle;
    field.label = std::move(theLabel);
    field.toggle = theValue;
    return field;
}

bool ShowFeatureDialog(QWidget*                  theParent,
                       const QString&            theTitle,
                       std::vector<DialogField>& theFields,
                       const QString&            theHint,
                       const ParameterTable*     theParameters)
{
    QDialog dialog(theParent);
    dialog.setWindowTitle(theTitle);

    QFormLayout* form = new QFormLayout(&dialog);

    // Parallel to theFields; the widget for a row is read back by index
    // once the dialog is accepted.
    std::vector<QWidget*> editors(theFields.size(), nullptr);

    for (std::size_t i = 0; i < theFields.size(); ++i) {
        const DialogField& field = theFields[i];
        switch (field.kind) {
            case DialogField::Kind::Number: {
                // The declared suffix says what the number means; the field
                // itself then accepts any unit of that kind ("12.9in") and
                // re-displays in whatever the user typed.
                //
                // Revolve's angle says " °", not "deg": reading only "deg"
                // made it a LENGTH field showing "360 mm".
                const UnitKind kind =
                    (field.suffix.contains("deg") || field.suffix.contains(QChar(0x00B0)))
                        ? UnitKind::Angle
                    : field.suffix.trimmed().isEmpty() ? UnitKind::Unitless
                                                       : UnitKind::Length;
                UnitLineEdit* box = new UnitLineEdit(kind, &dialog);
                box->SetParameterTable(theParameters);
                box->SetRange(field.minimum, field.maximum);
                box->SetExpression(field.expression, field.value);
                form->addRow(field.label, box);
                editors[i] = box;
                break;
            }
            case DialogField::Kind::Choice: {
                QComboBox* box = new QComboBox(&dialog);
                box->addItems(field.choices);
                if (field.choice >= 0 && field.choice < field.choices.size()) {
                    box->setCurrentIndex(field.choice);
                }
                form->addRow(field.label, box);
                editors[i] = box;
                break;
            }
            case DialogField::Kind::Toggle: {
                QCheckBox* box = new QCheckBox(&dialog);
                box->setChecked(field.toggle);
                form->addRow(field.label, box);
                editors[i] = box;
                break;
            }
        }
    }

    if (!theHint.isEmpty()) {
        QLabel* hint = new QLabel(theHint, &dialog);
        hint->setWordWrap(true);
        form->addRow(hint);
    }

    QDialogButtonBox* buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);

    // OK waits while any field holds text it could not read, so an
    // expression naming a missing parameter cannot slip through as the
    // number the field had before.
    QPushButton* ok = buttons->button(QDialogButtonBox::Ok);
    const auto refreshOk = [&editors, ok]() {
        bool acceptable = true;
        for (QWidget* editor : editors) {
            if (UnitLineEdit* box = qobject_cast<UnitLineEdit*>(editor)) {
                acceptable = acceptable && box->IsAcceptable();
            }
        }
        ok->setEnabled(acceptable);
    };
    for (QWidget* editor : editors) {
        if (UnitLineEdit* box = qobject_cast<UnitLineEdit*>(editor)) {
            QObject::connect(box, &UnitLineEdit::AcceptableChanged, &dialog, refreshOk);
        }
    }

    if (dialog.exec() != QDialog::Accepted) {
        return false;
    }

    for (std::size_t i = 0; i < theFields.size(); ++i) {
        DialogField& field = theFields[i];
        switch (field.kind) {
            case DialogField::Kind::Number:
                if (UnitLineEdit* box = qobject_cast<UnitLineEdit*>(editors[i])) {
                    field.value = box->Value();
                    field.expression = box->Expression();
                }
                break;
            case DialogField::Kind::Choice:
                if (QComboBox* box = qobject_cast<QComboBox*>(editors[i])) {
                    field.choice = box->currentIndex();
                }
                break;
            case DialogField::Kind::Toggle:
                if (QCheckBox* box = qobject_cast<QCheckBox*>(editors[i])) {
                    field.toggle = box->isChecked();
                }
                break;
        }
    }
    return true;
}

bool ApplyFieldExpressions(Feature& theFeature, const std::vector<DialogField>& theFields)
{
    const std::vector<Parameter> parameters = theFeature.Parameters();
    bool allMatched = true;
    for (const DialogField& field : theFields) {
        if (field.kind != DialogField::Kind::Number || field.expression.empty()) {
            continue;
        }
        const std::string name = field.label.toStdString();
        const bool matched =
            std::any_of(parameters.begin(), parameters.end(), [&name](const Parameter& theP) {
                return theP.name == name && theP.type == Parameter::Type::Double;
            });
        if (matched) {
            theFeature.SetExpression(name, field.expression);
        } else {
            allMatched = false;
        }
    }
    return allMatched;
}

} // namespace lcad
