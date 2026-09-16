#pragma once

#include <QString>
#include <QStringList>

#include <vector>

class QWidget;

namespace lcad {

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
bool ShowFeatureDialog(QWidget*                  theParent,
                       const QString&            theTitle,
                       std::vector<DialogField>& theFields,
                       const QString&            theHint = QString());

} // namespace lcad
