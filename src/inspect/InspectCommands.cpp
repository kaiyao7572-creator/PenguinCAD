#include "core/Command.h"
#include "core/Document.h"
#include "core/Registration.h"
#include "core/ViewportInteraction.h"

#include "OcctViewport.h"

#include <AIS_InteractiveContext.hxx>
#include <AIS_Line.hxx>
#include <AIS_Shape.hxx>
#include <AIS_TextLabel.hxx>
#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <Geom_CartesianPoint.hxx>
#include <Graphic3d_ClipPlane.hxx>
#include <Graphic3d_Vec2.hxx>
#include <Standard_Failure.hxx>
#include <StdSelect_BRepOwner.hxx>
#include <TCollection_ExtendedString.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Vertex.hxx>
#include <V3d_View.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>

#include <string>

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>
#include <QStatusBar>
#include <QString>

namespace lcad {

namespace {

const char* const kInspectGroup = "Inspect";

// Anything shorter than this is a mis-click rather than a second point the
// user meant to pick (mirrors sketch/SketchTools.cpp's kMinimumLength).
constexpr double kMinimumDistance = 1.0e-6;

Quantity_Color MeasureColor()
{
    return Quantity_Color(1.0, 0.65, 0.0, Quantity_TOC_RGB);   // orange: distinct from the model
}

void ShowStatus(const CommandContext& theContext, const QString& theText)
{
    if (QMainWindow* window = qobject_cast<QMainWindow*>(theContext.parent)) {
        window->statusBar()->showMessage(theText);
    }
}

QString FormatNumber(double theValue, const char* theUnit)
{
    return QString("%1 %2").arg(theValue, 0, 'f', 3).arg(theUnit);
}

QString FormatPoint(const gp_Pnt& thePoint)
{
    return QString("(%1, %2, %3) mm")
        .arg(thePoint.X(), 0, 'f', 3)
        .arg(thePoint.Y(), 0, 'f', 3)
        .arg(thePoint.Z(), 0, 'f', 3);
}

// ---- Model Properties ----

class ModelPropertiesCommand : public Command
{
public:
    std::string Id() const override { return "inspect.model_properties"; }
    std::string Title() const override { return "Model Properties"; }
    std::string Group() const override { return kInspectGroup; }
    std::string Section() const override { return "Analyze"; }
    std::string Icon() const override { return "📊"; }
    // No shortcut: Fusion gives Model Properties none, and I is Measure.
    std::string Description() const override
    {
        return "Report volume, surface area, center of mass and bounding box of the model";
    }

    bool IsEnabled(const CommandContext& theContext) const override
    {
        return theContext.document != nullptr && !theContext.document->Shape().IsNull();
    }

    void Execute(CommandContext& theContext) override
    {
        if (theContext.document == nullptr) {
            return;
        }
        const TopoDS_Shape& shape = theContext.document->Shape();
        if (shape.IsNull()) {
            return;
        }

        GProp_GProps volumeProps;
        GProp_GProps surfaceProps;
        Bnd_Box      box;
        try {
            // OnlyClosed=false: an open/imperfect shape still gets a (less
            // meaningful but non-crashing) number rather than nothing.
            BRepGProp::VolumeProperties(shape, volumeProps);
            BRepGProp::SurfaceProperties(shape, surfaceProps);
            BRepBndLib::Add(shape, box);
        } catch (const Standard_Failure& e) {
            QMessageBox::warning(
                theContext.parent, "Model Properties",
                QString("Could not compute properties:\n%1")
                    .arg(e.GetMessageString() != nullptr ? e.GetMessageString() : "OCCT error"));
            return;
        }

        QDialog dialog(theContext.parent);
        dialog.setWindowTitle("Model Properties");
        dialog.setMinimumWidth(340);

        QFormLayout* form = new QFormLayout(&dialog);

        auto addRow = [form](const QString& theLabel, const QString& theValue) {
            QLabel* value = new QLabel(theValue);
            // Selectable so the user can copy a number out of the dialog.
            value->setTextInteractionFlags(Qt::TextSelectableByMouse);
            form->addRow(theLabel, value);
        };

        addRow("Volume", FormatNumber(volumeProps.Mass(), "mm³"));
        addRow("Surface Area", FormatNumber(surfaceProps.Mass(), "mm²"));
        addRow("Center of Mass", FormatPoint(volumeProps.CentreOfMass()));

        if (box.IsVoid()) {
            addRow("Bounding Box", "(empty)");
        } else {
            Standard_Real xmin = 0.0, ymin = 0.0, zmin = 0.0;
            Standard_Real xmax = 0.0, ymax = 0.0, zmax = 0.0;
            box.Get(xmin, ymin, zmin, xmax, ymax, zmax);
            addRow("Bounding Box Min", FormatPoint(gp_Pnt(xmin, ymin, zmin)));
            addRow("Bounding Box Max", FormatPoint(gp_Pnt(xmax, ymax, zmax)));
            addRow("Dimensions (X x Y x Z)",
                   QString("%1 x %2 x %3 mm")
                       .arg(xmax - xmin, 0, 'f', 3)
                       .arg(ymax - ymin, 0, 'f', 3)
                       .arg(zmax - zmin, 0, 'f', 3));
        }

        QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Ok, &dialog);
        QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        form->addRow(buttons);

        dialog.exec();
    }
};

// ---- Measure Distance ----
//
// A ViewportInteraction, same shape as the sketch tools in
// sketch/SketchTools.cpp: push onto the viewport while active, consume
// left-click/move/release, let everything else (orbit/pan, Right/Middle
// drags) fall through by returning false.
class MeasureDistanceTool : public ViewportInteraction
{
public:
    static MeasureDistanceTool& Instance()
    {
        static MeasureDistanceTool theTool;
        return theTool;
    }

    void Start(const CommandContext& theContext)
    {
        myContext = theContext;

        if (myIsRunning) {
            // Re-running the active tool just restarts the pick in
            // progress, sketch-tool style.
            myHasStart = false;
            ClearMarkers();
            ShowHint();
            return;
        }

        ActivateVertexPicking();
        myIsRunning = true;
        if (myContext.viewport != nullptr) {
            myContext.viewport->PushInteraction(this);
        }
        ShowHint();
    }

    void Stop()
    {
        if (!myIsRunning) {
            return;
        }
        // Only pop when still on top: another subsystem may have pushed its
        // own handler over ours, and popping would take away theirs.
        if (myContext.viewport != nullptr && myContext.viewport->CurrentInteraction() == this) {
            myContext.viewport->PopInteraction();   // calls OnDeactivated()
        } else {
            OnDeactivated();
        }
    }

    bool OnMousePress(const Graphic3d_Vec2i& thePos, Qt::MouseButton theButton,
                      Qt::KeyboardModifiers theModifiers) override
    {
        (void)theModifiers;
        if (!myIsRunning) {
            return false;
        }
        // Right/middle stay with the viewport so orbit and pan keep working
        // while the tool is armed.
        if (theButton != Qt::LeftButton) {
            return false;
        }

        gp_Pnt point;
        bool   snapped = false;
        PointAt(thePos, point, snapped);

        if (!myHasStart) {
            ClearMarkers();   // drop the previous result before starting a new one
            myStart    = point;
            myHasStart = true;
        } else if (myStart.Distance(point) > kMinimumDistance) {
            ShowResult(myStart, point);
            myHasStart = false;
        } else {
            myHasStart = false;   // clicked on top of the start point: just drop it
        }
        ShowHint();
        return true;
    }

    bool OnMouseMove(const Graphic3d_Vec2i& thePos, Qt::MouseButtons theButtons,
                     Qt::KeyboardModifiers theModifiers) override
    {
        (void)theModifiers;
        if (!myIsRunning) {
            return false;
        }
        if (theButtons.testFlag(Qt::RightButton) || theButtons.testFlag(Qt::MiddleButton)) {
            return false;   // orbit/pan drag in progress
        }

        gp_Pnt point;
        bool   snapped = false;
        PointAt(thePos, point, snapped);

        if (myHasStart) {
            UpdatePreview(myStart, point);
            ShowStatus(myContext, QString("Distance so far: %1%2 - click to set the second point.")
                                      .arg(FormatNumber(myStart.Distance(point), "mm"))
                                      .arg(snapped ? " (vertex)" : ""));
        }
        return true;
    }

    bool OnMouseRelease(const Graphic3d_Vec2i& thePos, Qt::MouseButton theButton,
                        Qt::KeyboardModifiers theModifiers) override
    {
        (void)thePos;
        (void)theModifiers;
        // The press was consumed, so the viewport's controller never saw
        // the button go down; handing it the release would leave it in a
        // state it can't make sense of (see sketch/SketchTools.cpp).
        return myIsRunning && theButton == Qt::LeftButton;
    }

    bool OnKeyPress(int theKey, Qt::KeyboardModifiers theModifiers) override
    {
        (void)theModifiers;
        if (!myIsRunning || theKey != Qt::Key_Escape) {
            return false;
        }

        // First Escape abandons the point half-picked so far, second one
        // exits the tool -- same convention the sketch tools use.
        if (myHasStart) {
            myHasStart = false;
            ClearPreviewLine();
            ShowHint();
            return true;
        }

        Stop();
        return true;
    }

    void OnDeactivated() override
    {
        myIsRunning = false;
        myHasStart  = false;
        RestoreSelectionModes();
        ClearMarkers();
        ShowStatus(myContext, QString());
        myContext.Redraw();
    }

private:
    MeasureDistanceTool() = default;

    // Vertex-level picking isn't active by default (MainWindow displays the
    // model at selection mode 0, whole-shape only), so it's switched on for
    // the duration of the tool and switched back off on exit. Context-wide
    // rather than per-object, so it also covers a shape MainWindow swaps in
    // while the tool is running (e.g. an Undo) without having to track
    // individual AIS_Shape handles.
    void ActivateVertexPicking()
    {
        const Handle(AIS_InteractiveContext) ctx = myContext.AisContext();
        if (!ctx.IsNull()) {
            ctx->Activate(AIS_Shape::SelectionMode(TopAbs_VERTEX));
        }
    }

    void RestoreSelectionModes()
    {
        const Handle(AIS_InteractiveContext) ctx = myContext.AisContext();
        if (!ctx.IsNull()) {
            ctx->Deactivate(AIS_Shape::SelectionMode(TopAbs_VERTEX));
        }
    }

    // Snaps to a vertex under the cursor when there is one; otherwise falls
    // back to the raw 3D point under the cursor.
    void PointAt(const Graphic3d_Vec2i& thePos, gp_Pnt& thePoint, bool& theSnapped) const
    {
        theSnapped = false;

        const Handle(AIS_InteractiveContext) ctx  = myContext.AisContext();
        const Handle(V3d_View)               view = myContext.View();
        if (ctx.IsNull() || view.IsNull()) {
            return;
        }

        ctx->MoveTo(thePos.x(), thePos.y(), view, Standard_False);

        // DetectedOwner() + StdSelect_BRepOwner rather than the simpler
        // DetectedShape()/HasDetectedShape() pair: those are tagged
        // deprecated (leftover "Local Context" naming) even though picking
        // itself is the ordinary, non-local kind used everywhere else here.
        const Handle(StdSelect_BRepOwner) owner =
            Handle(StdSelect_BRepOwner)::DownCast(ctx->DetectedOwner());
        if (!owner.IsNull() && owner->HasShape()) {
            const TopoDS_Shape& shape = owner->Shape();
            if (!shape.IsNull() && shape.ShapeType() == TopAbs_VERTEX) {
                thePoint   = BRep_Tool::Pnt(TopoDS::Vertex(shape));
                theSnapped = true;
                return;
            }
        }

        Standard_Real x = 0.0, y = 0.0, z = 0.0;
        view->Convert(thePos.x(), thePos.y(), x, y, z);
        thePoint = gp_Pnt(x, y, z);
    }

    void ClearPreviewLine()
    {
        const Handle(AIS_InteractiveContext) ctx = myContext.AisContext();
        if (!ctx.IsNull() && !myConnector.IsNull()) {
            ctx->Remove(myConnector, Standard_False);
        }
        myConnector.Nullify();
    }

    void ClearMarkers()
    {
        const Handle(AIS_InteractiveContext) ctx = myContext.AisContext();
        if (!ctx.IsNull() && !myLabel.IsNull()) {
            ctx->Remove(myLabel, Standard_False);
        }
        myLabel.Nullify();
        ClearPreviewLine();
    }

    void UpdatePreview(const gp_Pnt& theA, const gp_Pnt& theB)
    {
        const Handle(AIS_InteractiveContext) ctx = myContext.AisContext();
        if (ctx.IsNull()) {
            return;
        }
        ClearPreviewLine();
        myConnector = new AIS_Line(new Geom_CartesianPoint(theA), new Geom_CartesianPoint(theB));
        myConnector->SetColor(MeasureColor());
        myConnector->SetWidth(1.5);
        ctx->Display(myConnector, Standard_False);
    }

    void ShowResult(const gp_Pnt& theA, const gp_Pnt& theB)
    {
        const Handle(AIS_InteractiveContext) ctx = myContext.AisContext();
        if (ctx.IsNull()) {
            return;
        }
        ClearMarkers();

        const double distance = theA.Distance(theB);

        myConnector = new AIS_Line(new Geom_CartesianPoint(theA), new Geom_CartesianPoint(theB));
        myConnector->SetColor(MeasureColor());
        myConnector->SetWidth(2.0);
        ctx->Display(myConnector, Standard_False);

        const gp_Pnt mid(0.5 * (theA.X() + theB.X()), 0.5 * (theA.Y() + theB.Y()),
                         0.5 * (theA.Z() + theB.Z()));
        const QString text = FormatNumber(distance, "mm");

        myLabel = new AIS_TextLabel();
        myLabel->SetText(TCollection_ExtendedString(text.toUtf8().constData()));
        myLabel->SetPosition(mid);
        myLabel->SetColor(MeasureColor());
        ctx->Display(myLabel, Standard_False);

        ShowStatus(myContext, QString("Distance: %1").arg(text));
    }

    void ShowHint()
    {
        ShowStatus(myContext,
                   myHasStart
                       ? QString("Measure: click the second point (snaps to vertices). Esc cancels.")
                       : QString("Measure: click the first point (snaps to vertices). Esc exits."));
    }

    CommandContext myContext;
    bool           myIsRunning = false;
    bool           myHasStart  = false;
    gp_Pnt         myStart;

    Handle(AIS_Line)      myConnector;
    Handle(AIS_TextLabel) myLabel;
};

class MeasureDistanceCommand : public Command
{
public:
    std::string Id() const override { return "inspect.measure_distance"; }
    std::string Title() const override { return "Measure Distance"; }
    std::string Group() const override { return kInspectGroup; }
    std::string Section() const override { return "Measure"; }
    std::string Icon() const override { return "↔️"; }
    // I, as in Fusion. M belongs to Move, and a key bound twice fires neither.
    std::string Shortcut() const override { return "I"; }
    std::string Description() const override
    {
        return "Click two points (snaps to vertices) to measure the distance between them";
    }

    bool IsEnabled(const CommandContext& theContext) const override
    {
        return theContext.document != nullptr && !theContext.View().IsNull();
    }

    void Execute(CommandContext& theContext) override
    {
        MeasureDistanceTool::Instance().Start(theContext);
    }
};

// ---- Section View ----

// Owns the single clip plane a "Section View" toggle adds to the view.
class SectionPlaneController
{
public:
    static SectionPlaneController& Instance()
    {
        static SectionPlaneController theInstance;
        return theInstance;
    }

    bool IsActive() const { return myActive; }

    void Enable(const CommandContext& theContext, const gp_Pln& thePlane)
    {
        const Handle(V3d_View) view = theContext.View();
        if (view.IsNull()) {
            return;
        }
        Disable(theContext);   // drop any previous plane before adding the new one

        myPlane = new Graphic3d_ClipPlane(thePlane);
        myPlane->SetCapping(Standard_True);   // fills the cut face instead of leaving it open
        myPlane->SetOn(Standard_True);
        view->AddClipPlane(myPlane);
        myActive = true;
    }

    void Disable(const CommandContext& theContext)
    {
        const Handle(V3d_View) view = theContext.View();
        if (!view.IsNull() && !myPlane.IsNull()) {
            view->RemoveClipPlane(myPlane);
        }
        myPlane.Nullify();
        myActive = false;
    }

private:
    SectionPlaneController() = default;

    Handle(Graphic3d_ClipPlane) myPlane;
    bool                        myActive = false;
};

// A plane choice plus a numeric offset along its normal. Unlike sketching
// -- where you pick the plane directly in the viewport -- a section cut is
// a transient inspection setting, so a small dialog is the lighter touch.
bool AskForSectionPlane(const CommandContext& theContext, gp_Pln& thePlane)
{
    QDialog dialog(theContext.parent);
    dialog.setWindowTitle("Section View");

    QFormLayout* form = new QFormLayout(&dialog);

    QComboBox* planeBox = new QComboBox(&dialog);
    planeBox->addItem("XY plane (cuts along Z)");
    planeBox->addItem("XZ plane (cuts along Y)");
    planeBox->addItem("YZ plane (cuts along X)");
    form->addRow("Plane", planeBox);

    QDoubleSpinBox* offsetBox = new QDoubleSpinBox(&dialog);
    offsetBox->setRange(-100000.0, 100000.0);
    offsetBox->setDecimals(3);
    offsetBox->setSingleStep(1.0);
    offsetBox->setSuffix(" mm");
    form->addRow("Offset", offsetBox);

    QLabel* hint = new QLabel("Everything on the plane's normal side is clipped away.", &dialog);
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

    const double offset = offsetBox->value();
    switch (planeBox->currentIndex()) {
        case 0: thePlane = gp_Pln(gp_Pnt(0.0, 0.0, offset), gp_Dir(0.0, 0.0, 1.0)); break;
        case 1: thePlane = gp_Pln(gp_Pnt(0.0, offset, 0.0), gp_Dir(0.0, 1.0, 0.0)); break;
        default: thePlane = gp_Pln(gp_Pnt(offset, 0.0, 0.0), gp_Dir(1.0, 0.0, 0.0)); break;
    }
    return true;
}

class SectionViewCommand : public Command
{
public:
    std::string Id() const override { return "inspect.section_view"; }
    std::string Title() const override { return "Section View"; }
    std::string Group() const override { return kInspectGroup; }
    std::string Section() const override { return "Analyze"; }
    std::string Icon() const override { return "🔪"; }
    std::string Shortcut() const override { return "X"; }
    std::string Description() const override
    {
        return "Cut into the model with a clip plane (pick a plane and offset when turning it on)";
    }

    bool IsCheckable() const override { return true; }

    bool IsEnabled(const CommandContext& theContext) const override
    {
        return !theContext.View().IsNull();
    }

    bool IsChecked(const CommandContext& theContext) const override
    {
        (void)theContext;
        return SectionPlaneController::Instance().IsActive();
    }

    void Execute(CommandContext& theContext) override
    {
        SectionPlaneController& section = SectionPlaneController::Instance();
        if (section.IsActive()) {
            section.Disable(theContext);
            theContext.Redraw();
            return;
        }

        gp_Pln plane;
        if (!AskForSectionPlane(theContext, plane)) {
            return;
        }
        section.Enable(theContext, plane);
        theContext.Redraw();
    }
};

} // namespace

void RegisterInspectCommands(CommandRegistry& theRegistry)
{
    theRegistry.Add(std::make_unique<ModelPropertiesCommand>());
    theRegistry.Add(std::make_unique<MeasureDistanceCommand>());
    theRegistry.Add(std::make_unique<SectionViewCommand>());
}

} // namespace lcad
