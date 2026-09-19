#include "core/Command.h"
#include "core/Document.h"
#include "core/Registration.h"
#include "gizmos/MoveGizmoTool.h"
#include "gizmos/PressPullGizmoTool.h"
#include "gizmos/TransformFeature.h"

#include <memory>

#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QMainWindow>
#include <QStatusBar>
#include <QString>

#include "widgets/UnitLineEdit.h"

namespace lcad {

namespace {

const char* const kSolidGroup  = "Solid";
const char* const kModifyGroup = "Modify";

void ShowStatus(const CommandContext& theContext, const QString& theText)
{
    if (QMainWindow* window = qobject_cast<QMainWindow*>(theContext.parent)) {
        window->statusBar()->showMessage(theText);
    }
}

// A single spin box for one translation/rotation field, matching the
// numeric style SketchCommands.cpp uses for its offset field.
UnitLineEdit* MakeSpinBox(QWidget* theParent, const QString& theSuffix, double theRange)
{
    // Distances accept any length unit ("2in"), angles any angle unit.
    const UnitKind kind = (theSuffix == " deg") ? UnitKind::Angle : UnitKind::Length;
    UnitLineEdit* box = new UnitLineEdit(kind, theParent);
    box->SetRange(-theRange, theRange);
    box->SetValue(0.0);
    return box;
}

// Numeric fallback for the interactive gizmo: six plain fields rather than
// handles, so a move/rotate is always reachable even if picking a handle
// in the viewport proves fiddly (trackpad, tiny body, etc).
bool AskForTransform(const CommandContext& theContext,
                     double& theTx, double& theTy, double& theTz,
                     double& theRxDeg, double& theRyDeg, double& theRzDeg)
{
    QDialog dialog(theContext.parent);
    dialog.setWindowTitle("Move/Rotate");

    QFormLayout* form = new QFormLayout(&dialog);

    UnitLineEdit* txBox = MakeSpinBox(&dialog, " mm", 100000.0);
    UnitLineEdit* tyBox = MakeSpinBox(&dialog, " mm", 100000.0);
    UnitLineEdit* tzBox = MakeSpinBox(&dialog, " mm", 100000.0);
    UnitLineEdit* rxBox = MakeSpinBox(&dialog, " deg", 3600.0);
    UnitLineEdit* ryBox = MakeSpinBox(&dialog, " deg", 3600.0);
    UnitLineEdit* rzBox = MakeSpinBox(&dialog, " deg", 3600.0);

    form->addRow("Move X", txBox);
    form->addRow("Move Y", tyBox);
    form->addRow("Move Z", tzBox);
    form->addRow("Rotate X", rxBox);
    form->addRow("Rotate Y", ryBox);
    form->addRow("Rotate Z", rzBox);

    QLabel* hint = new QLabel("Rotation is about the world origin; translation is applied after it.",
                              &dialog);
    hint->setWordWrap(true);
    form->addRow(hint);

    QDialogButtonBox* buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);

    if (dialog.exec() != QDialog::Accepted) {
        return false;
    }

    theTx = txBox->Value();
    theTy = tyBox->Value();
    theTz = tzBox->Value();
    theRxDeg = rxBox->Value();
    theRyDeg = ryBox->Value();
    theRzDeg = rzBox->Value();
    return true;
}

// ---- commands ----

// The interactive gizmo: attaches AIS_Manipulator to the current
// selection and lets the user drag translation arrows / rotation rings.
// See MoveGizmoTool for the drag-to-TransformFeature plumbing.
class MoveGizmoCommand : public Command
{
public:
    std::string Id() const override { return "gizmo.move"; }
    std::string Title() const override { return "Move/Rotate"; }
    std::string Group() const override { return kSolidGroup; }
    std::string Section() const override { return kModifyGroup; }
    std::string Icon() const override { return "🕹️"; }  // joystick: direct manipulation
    std::string Shortcut() const override { return "M"; }
    std::string Description() const override
    {
        return "Drag the selected body with translation and rotation handles; "
               "Esc or M again finishes";
    }

    bool IsEnabled(const CommandContext& theContext) const override
    {
        // Fusion's press/pull arrow answers to the SELECTION, not to a
        // button, so it needs a hook that fires when the selection
        // changes. src/gizmos/ has none of its own: the viewport's single
        // selection callback belongs to MainWindow. Command states are
        // refreshed on every selection change (and on a 300ms timer), so
        // that poll is the hook -- the same bargain the view cube makes to
        // materialise itself, for the same reason.
        PressPullGizmoTool::Instance().Sync(theContext);

        return theContext.document != nullptr && !theContext.document->Shape().IsNull();
    }

    bool IsCheckable() const override { return true; }
    bool IsChecked(const CommandContext& theContext) const override
    {
        (void)theContext;
        return MoveGizmoTool::Instance().IsActive();
    }

    void Execute(CommandContext& theContext) override
    {
        MoveGizmoTool& tool = MoveGizmoTool::Instance();
        if (tool.IsActive()) {
            tool.Stop();  // second invocation detaches
            return;
        }
        tool.Start(theContext);  // no-op with a status hint if nothing is selected
    }
};

// Numeric fallback / precise-entry alternative to the gizmo. TransformFeature
// transforms the whole evaluated shape, so this moves every body at once
// regardless of what is selected -- which is why it needs no selection, and
// why it is NOT a per-body move now that the document holds several.
class MoveRotateDialogCommand : public Command
{
public:
    std::string Id() const override { return "gizmo.moveRotateDialog"; }
    std::string Title() const override { return "Move/Rotate (Numeric)"; }
    std::string Group() const override { return kSolidGroup; }
    std::string Section() const override { return kModifyGroup; }
    std::string Icon() const override { return "🔢"; }
    std::string Shortcut() const override { return "Ctrl+M"; }
    std::string Description() const override
    {
        return "Move and/or rotate the whole model by typed-in distances and angles";
    }

    bool IsEnabled(const CommandContext& theContext) const override
    {
        return theContext.document != nullptr && !theContext.document->Shape().IsNull();
    }

    void Execute(CommandContext& theContext) override
    {
        double tx = 0.0, ty = 0.0, tz = 0.0;
        double rx = 0.0, ry = 0.0, rz = 0.0;
        if (!AskForTransform(theContext, tx, ty, tz, rx, ry, rz)) {
            return;
        }
        if (tx == 0.0 && ty == 0.0 && tz == 0.0 && rx == 0.0 && ry == 0.0 && rz == 0.0) {
            ShowStatus(theContext, "Nothing to move -- all fields were zero.");
            return;
        }

        auto feature = std::make_shared<TransformFeature>(tx, ty, tz, rx, ry, rz);
        feature->SetName(theContext.document->MakeUniqueName("Move"));
        theContext.document->AddFeature(feature);
        ShowStatus(theContext, "Move added to the timeline.");
    }
};

} // namespace

void RegisterGizmoCommands(CommandRegistry& theRegistry)
{
    theRegistry.Add(std::make_unique<MoveGizmoCommand>());
    theRegistry.Add(std::make_unique<MoveRotateDialogCommand>());
}

} // namespace lcad
