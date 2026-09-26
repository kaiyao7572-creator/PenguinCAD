#pragma once

#include <QString>
#include <QStringList>

#include <string>
#include <vector>

class QWidget;

namespace lcad {

class Feature;
class ParameterTable;

// One row of a feature-creation dialog. The same struct carries the
// definition in and the entered value back out, so a command declares its
// form as a vector literal and reads the answers from the same place --
// ten dialogs' worth of boilerplate collapses into ten short lists.
struct DialogField
{
    enum class Kind
    {
        Number,
        Choice,
        Toggle
    };

    Kind    kind = Kind::Number;
    QString label;

    double  value    = 0.0;                       // Kind::Number
    // Out: the expression typed into a Number field when it names a
    // parameter ("plate_t * 2"), empty when it was a plain number. value
    // is what it gives. ApplyFieldExpressions hands it to the feature.
    std::string expression;
    double  minimum  = 0.001;
    double  maximum  = 1.0e6;
    int     decimals = 3;
    QString suffix   = QStringLiteral(" mm");

    QStringList choices;                          // Kind::Choice
    int         choice = 0;

    bool toggle = false;                          // Kind::Toggle

    static DialogField Number(QString theLabel,
                              double  theValue,
                              double  theMinimum = 0.001,
                              QString theSuffix  = QStringLiteral(" mm"));

    static DialogField Choice(QString theLabel, QStringList theChoices, int theChoice = 0);

    static DialogField Toggle(QString theLabel, bool theValue = false);
};

// Modal form dialog. Returns false when the user cancels; on OK the
// fields carry the entered values.
//
// Given the document's parameters, Number fields also take expressions
// over them, and OK stays greyed out while one names a parameter that
// does not exist. Without them a field still takes arithmetic.
bool ShowFeatureDialog(QWidget*                  theParent,
                       const QString&            theTitle,
                       std::vector<DialogField>& theFields,
                       const QString&            theHint = QString(),
                       const ParameterTable*     theParameters = nullptr);

// Drive theFeature's parameters by the expressions typed into theFields,
// matching each Number field to the Double or Int parameter with the same
// name as its label. Call it on a new feature before adding it to the
// document. False when a field carried an expression no parameter of the
// feature is named for -- the feature then holds only the number, so the
// command must not pass the parameters for such a field.
bool ApplyFieldExpressions(Feature& theFeature, const std::vector<DialogField>& theFields);

} // namespace lcad
