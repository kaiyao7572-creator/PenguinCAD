#include "core/Body.h"
#include "core/Command.h"
#include "core/Document.h"
#include "core/GeometrySelection.h"
#include "core/Registration.h"
#include "core/ViewportInteraction.h"
#include "inspect/InspectGeometry.h"
#include "widgets/UnitLineEdit.h"

#include "OcctViewport.h"

#include <AIS_InteractiveContext.hxx>
#include <AIS_Line.hxx>
#include <AIS_ListOfInteractive.hxx>
#include <AIS_Point.hxx>
#include <AIS_Shape.hxx>
#include <AIS_TextLabel.hxx>
#include <AIS_ViewCube.hxx>
#include <Geom_CartesianPoint.hxx>
#include <Graphic3d_ClipPlane.hxx>
#include <Graphic3d_Texture2D.hxx>
#include <Graphic3d_TextureParams.hxx>
#include <Graphic3d_Vec2.hxx>
#include <Graphic3d_ZLayerId.hxx>
#include <Image_PixMap.hxx>
#include <Prs3d_Drawer.hxx>
#include <Prs3d_PointAspect.hxx>
#include <SelectMgr_EntityOwner.hxx>
#include <Standard_Failure.hxx>
#include <StdSelect_BRepOwner.hxx>
#include <TCollection_ExtendedString.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS_Shape.hxx>
#include <V3d_View.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>

#include <algorithm>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <QButtonGroup>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QStatusBar>
#include <QString>
#include <QVBoxLayout>

namespace lcad {

namespace {

const char* const kInspectGroup = "Inspect";

// Measure graphics: an orange that stands off the model's own colour.
const Quantity_Color kMeasureColor(1.0, 0.55, 0.0, Quantity_TOC_sRGB);
// The result label is a light plate with dark text -- the same look as a
// sketch's value boxes, so a number the app is TELLING you reads the same
// wherever it appears.
const Quantity_Color kLabelPlateColor(0.91, 0.93, 0.95, Quantity_TOC_sRGB);
const Quantity_Color kLabelTextColor(0.08, 0.10, 0.13, Quantity_TOC_sRGB);

// Sizes in LOGICAL pixels, scaled by the device pixel ratio: AIS takes
// framebuffer pixels, and on this 2x display an unscaled label came out as
// unreadable 8-point text.
constexpr double kLabelHeight = 14.0;
constexpr double kLineWidth   = 2.0;
constexpr double kMarkerScale = 3.0;

constexpr Standard_Integer kNoSelectionMode = -1;

void ShowStatus(QWidget* theParent, const QString& theText)
{
    if (QMainWindow* window = qobject_cast<QMainWindow*>(theParent)) {
        if (theText.isEmpty()) {
            window->statusBar()->clearMessage();
        } else {
            window->statusBar()->showMessage(theText);
        }
    }
}

double DevicePixelRatio(const CommandContext& theContext)
{
    QWidget* widget = theContext.viewport != nullptr ? static_cast<QWidget*>(theContext.viewport)
                                                     : theContext.parent;
    const double ratio = widget != nullptr ? widget->devicePixelRatioF() : 1.0;
    return ratio > 0.0 ? ratio : 1.0;
}

QString Q(const std::string& theText)
{
    return QString::fromStdString(theText);
}

// The AIS objects MainWindow displays for the document's bodies. Found by
// matching shapes rather than held on to, because MainWindow rebuilds
// them on every document change and a list kept here would go stale.
std::vector<Handle(AIS_Shape)> BodyObjects(const CommandContext& theContext)
{
    std::vector<Handle(AIS_Shape)> result;
    const Handle(AIS_InteractiveContext) ctx = theContext.AisContext();
    if (ctx.IsNull() || theContext.document == nullptr) {
        return result;
    }
    AIS_ListOfInteractive displayed;
    ctx->DisplayedObjects(displayed);
    for (AIS_ListOfInteractive::Iterator it(displayed); it.More(); it.Next()) {
        const Handle(AIS_Shape) shape = Handle(AIS_Shape)::DownCast(it.Value());
        if (shape.IsNull() || shape->Shape().IsNull()) {
            continue;
        }
        for (const BodyPtr& body : theContext.document->Bodies()) {
            if (body && !body->Shape().IsNull() && body->Shape().IsSame(shape->Shape())) {
                result.push_back(shape);
                break;
            }
        }
    }
    return result;
}

// True for a body, or a face/edge/vertex of one. Anything else under the
// cursor -- a sketch curve, a gizmo -- is not something Measure answers.
bool IsBodyGeometry(const Document* theDocument, const TopoDS_Shape& theShape)
{
    if (theDocument == nullptr || theShape.IsNull()) {
        return false;
    }
    const TopAbs_ShapeEnum type = theShape.ShapeType();
    for (const BodyPtr& body : theDocument->Bodies()) {
        if (!body || body->Shape().IsNull()) {
            continue;
        }
        if (body->Shape().IsSame(theShape)) {
            return true;
        }
        if (type != TopAbs_FACE && type != TopAbs_EDGE && type != TopAbs_VERTEX) {
            continue;
        }
        TopTools_IndexedMapOfShape map;
        TopExp::MapShapes(body->Shape(), type, map);
        if (map.Contains(theShape)) {
            return true;
        }
    }
    return false;
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
        if (theContext.document == nullptr || theContext.document->Shape().IsNull()) {
            return;
        }
        const ModelProperties props = ComputeModelProperties(theContext.document->Shape());
        if (!props.isValid) {
            QMessageBox::warning(theContext.parent, "Model Properties",
                                 QString("Could not compute properties:\n%1").arg(Q(props.error)));
            return;
        }

        QDialog dialog(theContext.parent);
        dialog.setWindowTitle("Model Properties");
        dialog.setMinimumWidth(360);

        QFormLayout* form = new QFormLayout(&dialog);

        // Captions are made here rather than by addRow(QString, ...): that
        // overload makes each caption the value's buddy, and the script
        // harness's dump then prints the numbers with nothing saying which
        // is which.
        auto addRow = [form, &dialog](const QString& theLabel, const QString& theValue) {
            QLabel* caption = new QLabel(theLabel, &dialog);
            QLabel* value = new QLabel(theValue, &dialog);
            // Selectable so the user can copy a number out of the dialog.
            value->setTextInteractionFlags(Qt::TextSelectableByMouse);
            form->addRow(caption, value);
        };
        auto point = [](const gp_Pnt& thePoint) {
            return Q(FormatMeasureLength(thePoint.X()) + ", " + FormatMeasureLength(thePoint.Y())
                     + ", " + FormatMeasureLength(thePoint.Z()));
        };

        // The same formatters Measure uses, so both follow the document
        // unit and agree to the last digit.
        addRow("Volume", Q(FormatMeasureVolume(props.volume)));
        addRow("Area", Q(FormatMeasureArea(props.area)));
        addRow("Center of Mass", point(props.centreOfMass));

        if (!props.hasBounds) {
            addRow("Bounding Box", "(empty)");
        } else {
            addRow("Bounding Box Min", point(props.boundsMin));
            addRow("Bounding Box Max", point(props.boundsMax));
            addRow("Size (X x Y x Z)",
                   Q(FormatMeasureLength(props.boundsMax.X() - props.boundsMin.X()) + " x "
                     + FormatMeasureLength(props.boundsMax.Y() - props.boundsMin.Y()) + " x "
                     + FormatMeasureLength(props.boundsMax.Z() - props.boundsMin.Z())));
        }

        QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Ok, &dialog);
        QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        form->addRow(buttons);

        dialog.exec();
    }
};

// ---- Measure ----
//
// Fusion's Measure: click two things -- vertices, edges, faces, or whole
// bodies under Select Body Priority -- and read the shortest distance
// between them, with the deltas and, where it means something, the angle,
// in a small panel over the canvas. A third click starts a new
// measurement; clicking a picked item again un-picks it.
//
// Picks are held in OCCT's own selection so they highlight exactly the
// way any selection does, and whatever was selected when the tool started
// becomes the first picks, as it does in Fusion.

class MeasureTool;

// The results panel. A plain widget over the viewport rather than a dock
// or a dialog: a dock would shove the canvas sideways every time Measure
// opened, and a modal dialog would stop the viewport taking clicks.
//
// It is made a NATIVE child and raised. The canvas is a native GL window,
// and an ordinary child widget -- painted into the parent's backing store
// -- is drawn underneath it, where nobody can see it.
class MeasurePanel
{
public:
    void Show(QWidget* theHost, const std::function<void()>& theOnRestart,
              const std::function<void()>& theOnClose)
    {
        if (theHost == nullptr) {
            return;
        }
        if (m_frame.isNull() || m_frame->parentWidget() != theHost) {
            Build(theHost, theOnRestart, theOnClose);
        }
        Place();
        m_frame->show();
        m_frame->raise();
    }

    void Hide()
    {
        if (!m_frame.isNull()) {
            m_frame->hide();
        }
    }

    // theRows are (caption, value) pairs under the RESULTS heading; empty
    // rows are hidden rather than shown blank.
    void Update(const QString& theFirst, const QString& theSecond,
                const std::vector<std::pair<QString, QString>>& theRows, const QString& theHint)
    {
        if (m_frame.isNull()) {
            return;
        }
        m_first->setText(theFirst.isEmpty() ? QString("-") : theFirst);
        m_second->setText(theSecond.isEmpty() ? QString("-") : theSecond);
        for (std::size_t i = 0; i < m_resultCaptions.size(); ++i) {
            const bool used = i < theRows.size();
            m_resultCaptions[i]->setVisible(used);
            m_resultValues[i]->setVisible(used);
            if (used) {
                m_resultCaptions[i]->setText(theRows[i].first);
                m_resultValues[i]->setText(theRows[i].second);
            }
        }
        m_hint->setText(theHint);
        m_hint->setVisible(!theHint.isEmpty());
        m_frame->adjustSize();
        Place();
    }

private:
    static constexpr int kResultRows = 6;

    void Build(QWidget* theHost, const std::function<void()>& theOnRestart,
               const std::function<void()>& theOnClose)
    {
        delete m_frame.data();
        m_resultCaptions.clear();
        m_resultValues.clear();

        m_frame = new QFrame(theHost);
        m_frame->setObjectName("measurePanel");
        m_frame->setAttribute(Qt::WA_NativeWindow);
        m_frame->setAutoFillBackground(true);
        m_frame->setStyleSheet(
            "QFrame#measurePanel { background: #2b2d30; border: 1px solid #5a5d63;"
            " border-radius: 3px; }"
            " QLabel { color: #e6e6e6; }"
            " QLabel[role=\"heading\"] { color: #9aa0a6; font-weight: bold; }"
            " QLabel[role=\"value\"] { font-weight: bold; }");

        QVBoxLayout* outer = new QVBoxLayout(m_frame);
        outer->setContentsMargins(12, 10, 12, 10);
        outer->setSpacing(6);

        QLabel* title = new QLabel("MEASURE", m_frame);
        title->setProperty("role", "heading");
        outer->addWidget(title);

        QGridLayout* grid = new QGridLayout();
        grid->setHorizontalSpacing(14);
        grid->setVerticalSpacing(3);
        int row = 0;
        grid->addWidget(new QLabel("Selection 1", m_frame), row, 0);
        m_first = new QLabel("-", m_frame);
        grid->addWidget(m_first, row++, 1);
        grid->addWidget(new QLabel("Selection 2", m_frame), row, 0);
        m_second = new QLabel("-", m_frame);
        grid->addWidget(m_second, row++, 1);

        QLabel* results = new QLabel("RESULTS", m_frame);
        results->setProperty("role", "heading");
        grid->addWidget(results, row++, 0, 1, 2);
        for (int i = 0; i < kResultRows; ++i) {
            QLabel* caption = new QLabel(m_frame);
            QLabel* value = new QLabel(m_frame);
            value->setProperty("role", "value");
            value->setTextInteractionFlags(Qt::TextSelectableByMouse);
            grid->addWidget(caption, row, 0);
            grid->addWidget(value, row++, 1);
            m_resultCaptions.push_back(caption);
            m_resultValues.push_back(value);
        }
        outer->addLayout(grid);

        m_hint = new QLabel(m_frame);
        m_hint->setWordWrap(true);
        m_hint->setMaximumWidth(260);
        outer->addWidget(m_hint);

        QHBoxLayout* buttons = new QHBoxLayout();
        buttons->addStretch();
        QPushButton* restart = new QPushButton("Restart", m_frame);
        QPushButton* close = new QPushButton("Close", m_frame);
        // Never the default button: Return in the canvas must not close
        // the panel behind the user's back.
        restart->setAutoDefault(false);
        close->setAutoDefault(false);
        QObject::connect(restart, &QPushButton::clicked, m_frame, [theOnRestart]() {
            if (theOnRestart) {
                theOnRestart();
            }
        });
        QObject::connect(close, &QPushButton::clicked, m_frame, [theOnClose]() {
            if (theOnClose) {
                theOnClose();
            }
        });
        buttons->addWidget(restart);
        buttons->addWidget(close);
        outer->addLayout(buttons);
    }

    // Right-hand side of the canvas, where Fusion's command dialogs sit,
    // but BELOW the ViewCube. Pinned to the very top it covered the cube
    // completely (panel 2072..2640 x 406..1030 in a window grab, cube
    // 2370..2495 x 405..550), so nobody could turn the view while
    // measuring. The cube is drawn by OCCT in device pixels -- measured
    // on screen, it and its axis letters end 175 device pixels down -- so
    // the reserve is in device pixels too.
    void Place()
    {
        if (m_frame.isNull() || m_frame->parentWidget() == nullptr) {
            return;
        }
        constexpr int    kMargin = 12;
        constexpr double kViewCubeDevicePixels = 190.0;
        m_frame->adjustSize();
        const QWidget* host = m_frame->parentWidget();
        const double   ratio = host->devicePixelRatioF() > 0.0 ? host->devicePixelRatioF() : 1.0;
        const int      top = static_cast<int>(kViewCubeDevicePixels / ratio) + kMargin;
        m_frame->move(host->width() - m_frame->width() - kMargin, top);
    }

    QPointer<QFrame>     m_frame;
    QLabel*              m_first = nullptr;
    QLabel*              m_second = nullptr;
    QLabel*              m_hint = nullptr;
    std::vector<QLabel*> m_resultCaptions;
    std::vector<QLabel*> m_resultValues;
};

class MeasureTool : public ViewportInteraction, public DocumentObserver
{
public:
    static MeasureTool& Instance()
    {
        // Deliberately leaked: the AIS handles held here must not be
        // released during static destruction, after the viewer that owns
        // the GL context is already gone.
        static MeasureTool* theInstance = new MeasureTool();
        return *theInstance;
    }

    bool IsRunning() const { return myIsRunning; }

    void Start(const CommandContext& theContext)
    {
        myContext = theContext;

        if (myIsRunning) {
            // Pressing I again inside Measure starts the measurement over.
            ClearPicks();
            Refresh();
            return;
        }

        myIsRunning = true;
        if (myContext.document != nullptr) {
            myContext.document->AddObserver(this);
        }
        ArmPicking();
        SeedFromSelection();
        if (myContext.viewport != nullptr) {
            myContext.viewport->PushInteraction(this);
        }
        myPanel.Show(myContext.viewport, [this]() { Restart(); }, [this]() { Stop(); });
        Refresh();
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
        // Middle and right stay with the viewport, so pan (middle), orbit
        // (Shift+middle) and the marking menu (right) keep working while
        // the tool is armed.
        if (!myIsRunning || theButton != Qt::LeftButton) {
            return false;
        }

        ArmPicking();
        Handle(SelectMgr_EntityOwner) owner;
        TopoDS_Shape shape;
        myPressConsumed = false;
        if (!Detect(thePos, owner, shape)) {
            // The ViewCube sits below every tool on the interaction stack
            // and only gets the clicks a tool lets through. Swallowing
            // them left the cube dead for as long as Measure was open.
            const Handle(AIS_InteractiveContext) ctx = myContext.AisContext();
            if (!ctx.IsNull() && !Handle(AIS_ViewCubeOwner)::DownCast(ctx->DetectedOwner()).IsNull()) {
                return false;
            }
            // A click on empty canvas picks nothing and keeps what is
            // there: Fusion only reacts to geometry.
            myPressConsumed = true;
            return true;
        }
        myPressConsumed = true;

        const Handle(AIS_InteractiveContext) ctx = myContext.AisContext();
        // A click after a finished measurement starts the next one with
        // it -- even on a corner already used. Tested the other way round
        // first, a user re-clicking the first corner to measure from it
        // again silently UN-picked it instead, and got the distance from
        // the second corner: 20 where the face diagonal is 28.284.
        if (myPicks.size() >= 2) {
            ClearPicks();
        }
        // Mid-measurement, clicking the picked item again un-picks it, as
        // in any selection.
        for (std::size_t i = 0; i < myPicks.size(); ++i) {
            if (myPicks[i].shape.IsSame(shape)) {
                ctx->AddOrRemoveSelected(myPicks[i].owner, Standard_False);
                myPicks.erase(myPicks.begin() + static_cast<std::ptrdiff_t>(i));
                Refresh();
                return true;
            }
        }
        ctx->AddOrRemoveSelected(owner, Standard_False);
        myPicks.push_back(Pick{owner, shape});
        Refresh();
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
            return false;   // a pan, an orbit or a marking-menu gesture in progress
        }
        // Hover-highlight what a click would pick, with our pick modes.
        ArmPicking();
        const Handle(AIS_InteractiveContext) ctx = myContext.AisContext();
        const Handle(V3d_View) view = myContext.View();
        if (!ctx.IsNull() && !view.IsNull()) {
            ctx->MoveTo(thePos.x(), thePos.y(), view, Standard_False);
        }
        return true;
    }

    bool OnMouseRelease(const Graphic3d_Vec2i& thePos, Qt::MouseButton theButton,
                        Qt::KeyboardModifiers theModifiers) override
    {
        (void)thePos;
        (void)theModifiers;
        // A consumed press never reached the viewport's controller, so
        // handing it the release would leave it in a state it can't make
        // sense of (see sketch/SketchTools.cpp). A press let through to
        // the ViewCube needs its release to go the same way.
        if (!myIsRunning || theButton != Qt::LeftButton) {
            return false;
        }
        const bool consumed = myPressConsumed;
        myPressConsumed = false;
        return consumed;
    }

    bool OnKeyPress(int theKey, Qt::KeyboardModifiers theModifiers) override
    {
        (void)theModifiers;
        if (!myIsRunning) {
            return false;
        }
        if (theKey == Qt::Key_Return || theKey == Qt::Key_Enter) {
            Stop();   // Fusion's OK
            return true;
        }
        if (theKey != Qt::Key_Escape) {
            return false;
        }
        // Half a measurement: Escape drops that pick, the way the sketch
        // tools drop a half-drawn line. Otherwise it closes Measure.
        if (myPicks.size() == 1) {
            ClearPicks();
            Refresh();
            return true;
        }
        Stop();
        return true;
    }

    void OnDeactivated() override
    {
        myIsRunning = false;
        if (myContext.document != nullptr) {
            myContext.document->RemoveObserver(this);
        }
        ClearPicks();
        ClearMarkers();
        RestorePicking();
        const Handle(AIS_InteractiveContext) ctx = myContext.AisContext();
        if (!ctx.IsNull()) {
            // Otherwise the last hovered vertex stays lit after the tool
            // has gone, looking like a pick nobody made.
            ctx->ClearDetected(Standard_False);
        }
        myPanel.Hide();
        ShowStatus(myContext.parent, QString());
        myContext.Redraw();
    }

    void OnDocumentChanged(Document& theDocument) override
    {
        (void)theDocument;
        // The bodies were rebuilt: the picked owners belong to AIS objects
        // that no longer exist, and the geometry may have moved. Measuring
        // stale shapes would report a number for a model that is gone.
        if (!myIsRunning) {
            return;
        }
        myPicks.clear();
        ClearMarkers();
        Refresh();
    }

private:
    struct Pick
    {
        Handle(SelectMgr_EntityOwner) owner;
        TopoDS_Shape                  shape;
    };

    MeasureTool() = default;

    // What a click can pick while measuring. Vertices, edges and faces
    // together, whatever the selection filter says -- Fusion's Measure
    // picks all three -- except under Select Body Priority, where the
    // user has asked for whole bodies and gets them.
    std::vector<Standard_Integer> PickModes() const
    {
        const GeometrySelection& selection = GeometrySelection::Instance();
        if (selection.HasPriority() && selection.Priority() == EntityType::BRepBody) {
            return {AIS_Shape::SelectionMode(TopAbs_SHAPE)};
        }
        return {AIS_Shape::SelectionMode(TopAbs_VERTEX), AIS_Shape::SelectionMode(TopAbs_EDGE),
                AIS_Shape::SelectionMode(TopAbs_FACE)};
    }

    // Per body object, not context-wide: activating a mode on EVERY
    // displayed object also armed the view cube, grid pickers and sketch
    // graphics. Re-run on every move and press, because MainWindow
    // rebuilds the body objects on each document change and re-applies
    // the filter, which would silently drop modes set once at start.
    void ArmPicking()
    {
        const Handle(AIS_InteractiveContext) ctx = myContext.AisContext();
        if (ctx.IsNull()) {
            return;
        }
        const std::vector<Standard_Integer> modes = PickModes();
        for (const Handle(AIS_Shape)& object : BodyObjects(myContext)) {
            for (const Standard_Integer mode : modes) {
                ctx->Activate(object, mode, Standard_False);
            }
        }
    }

    // Leave each body with exactly the modes the selection filter wants.
    // The old tool deactivated vertex picking context-wide on exit, which
    // also switched it off for a user whose filter had Vertices on.
    void RestorePicking()
    {
        const Handle(AIS_InteractiveContext) ctx = myContext.AisContext();
        if (ctx.IsNull()) {
            return;
        }
        std::vector<Standard_Integer> wanted;
        for (const EntityType type : GeometrySelection::Instance().PickableTypes()) {
            wanted.push_back(AIS_Shape::SelectionMode(TopAbsTypeOf(type)));
        }
        const std::vector<Standard_Integer> ours = {
            AIS_Shape::SelectionMode(TopAbs_SHAPE), AIS_Shape::SelectionMode(TopAbs_VERTEX),
            AIS_Shape::SelectionMode(TopAbs_EDGE), AIS_Shape::SelectionMode(TopAbs_FACE)};
        for (const Handle(AIS_Shape)& object : BodyObjects(myContext)) {
            for (const Standard_Integer mode : ours) {
                if (std::find(wanted.begin(), wanted.end(), mode) == wanted.end()) {
                    ctx->Deactivate(object, mode);
                } else {
                    ctx->Activate(object, mode, Standard_False);
                }
            }
        }
    }

    // What is under the cursor, if it is body geometry.
    bool Detect(const Graphic3d_Vec2i& thePos, Handle(SelectMgr_EntityOwner)& theOwner,
                TopoDS_Shape& theShape) const
    {
        const Handle(AIS_InteractiveContext) ctx = myContext.AisContext();
        const Handle(V3d_View) view = myContext.View();
        if (ctx.IsNull() || view.IsNull()) {
            return false;
        }
        // A press is not a move: detect at the press point first, or a
        // click that arrives without a preceding move picks nothing.
        ctx->MoveTo(thePos.x(), thePos.y(), view, Standard_False);
        const Handle(StdSelect_BRepOwner) owner =
            Handle(StdSelect_BRepOwner)::DownCast(ctx->DetectedOwner());
        if (owner.IsNull() || !owner->HasShape()) {
            return false;
        }
        const TopoDS_Shape& shape = owner->Shape();
        if (!IsBodyGeometry(myContext.document, shape)) {
            return false;
        }
        theOwner = owner;
        theShape = shape;
        return true;
    }

    // Fusion carries a selection made before Measure into it.
    void SeedFromSelection()
    {
        const Handle(AIS_InteractiveContext) ctx = myContext.AisContext();
        if (ctx.IsNull()) {
            return;
        }
        std::vector<Pick> seeded;
        for (ctx->InitSelected(); ctx->MoreSelected(); ctx->NextSelected()) {
            const Handle(StdSelect_BRepOwner) owner =
                Handle(StdSelect_BRepOwner)::DownCast(ctx->SelectedOwner());
            if (owner.IsNull() || !owner->HasShape()
                || !IsBodyGeometry(myContext.document, owner->Shape())) {
                continue;
            }
            seeded.push_back(Pick{owner, owner->Shape()});
        }
        if (seeded.size() > 2) {
            seeded.clear();   // more than a measurement can use: start clean
            ctx->ClearSelected(Standard_False);
        }
        myPicks = seeded;
        // The picks now live in OCCT's selection only. Left in the shared
        // selection too, a single face would bring the press/pull arrow
        // back the moment Measure closes, over a face nobody re-picked.
        GeometrySelection::Instance().Clear();
    }

    void ClearPicks()
    {
        const Handle(AIS_InteractiveContext) ctx = myContext.AisContext();
        if (!ctx.IsNull()) {
            ctx->ClearSelected(Standard_False);
        }
        myPicks.clear();
    }

    void ClearMarkers()
    {
        const Handle(AIS_InteractiveContext) ctx = myContext.AisContext();
        for (Handle(AIS_InteractiveObject)& object : myMarkers) {
            if (!ctx.IsNull() && !object.IsNull()) {
                ctx->Remove(object, Standard_False);
            }
        }
        myMarkers.clear();
    }

    // Measure graphics sit in the topmost layer: a body diagonal runs
    // THROUGH the part, and in the model's own layer the line and its
    // label were hidden inside it -- the result was simply invisible.
    // The label goes one layer higher still, where there is no depth
    // test: sharing Topmost with the line, the line was drawn straight
    // through the digits it was labelled with.
    void DisplayMarker(const Handle(AIS_InteractiveObject)& theObject,
                       Graphic3d_ZLayerId theLayer = Graphic3d_ZLayerId_Topmost)
    {
        const Handle(AIS_InteractiveContext) ctx = myContext.AisContext();
        if (ctx.IsNull()) {
            return;
        }
        ctx->Display(theObject, 0, kNoSelectionMode, Standard_False);
        ctx->SetZLayer(theObject, theLayer);
        myMarkers.push_back(theObject);
    }

    void DrawResult(const MeasureResult& theResult)
    {
        const double ratio = DevicePixelRatio(myContext);

        if (theResult.distance > 1.0e-9) {
            Handle(AIS_Line) line = new AIS_Line(new Geom_CartesianPoint(theResult.onFirst),
                                                 new Geom_CartesianPoint(theResult.onSecond));
            line->SetColor(kMeasureColor);
            line->SetWidth(kLineWidth * ratio);
            DisplayMarker(line);
        }
        for (const gp_Pnt& end : {theResult.onFirst, theResult.onSecond}) {
            Handle(AIS_Point) marker = new AIS_Point(new Geom_CartesianPoint(end));
            marker->Attributes()->SetPointAspect(
                new Prs3d_PointAspect(Aspect_TOM_BALL, kMeasureColor, kMarkerScale * ratio));
            DisplayMarker(marker);
        }

        const gp_Pnt mid((theResult.onFirst.XYZ() + theResult.onSecond.XYZ()) * 0.5);
        Handle(AIS_TextLabel) label = new AIS_TextLabel();
        label->SetText(TCollection_ExtendedString(
            (" " + FormatMeasureLength(theResult.distance) + " ").c_str(), Standard_True));
        label->SetPosition(mid);
        label->SetDisplayType(Aspect_TODT_SUBTITLE);
        label->SetColorSubTitle(kLabelPlateColor);
        label->SetColor(kLabelTextColor);
        label->SetHeight(kLabelHeight * ratio);
        label->SetHJustification(Graphic3d_HTA_CENTER);
        label->SetVJustification(Graphic3d_VTA_CENTER);
        DisplayMarker(label, Graphic3d_ZLayerId_TopOSD);
    }

    void Restart()
    {
        ClearPicks();
        Refresh();
    }

    // Re-derive every output -- markers, panel, status bar -- from the
    // picks. One place, so the status bar can no longer be overwritten by
    // a hint a line after the result was put there (which is how the old
    // tool's result never reached the status bar at all).
    void Refresh()
    {
        ClearMarkers();

        QString first, second, hint, status;
        std::vector<std::pair<QString, QString>> rows;

        if (!myPicks.empty()) {
            first = Q(KindOfPick(myPicks[0].shape));
        }
        if (myPicks.size() >= 2) {
            second = Q(KindOfPick(myPicks[1].shape));
        }

        if (myPicks.empty()) {
            hint = "Click a face, edge or vertex to measure from.";
            status = "Measure: click a face, edge or vertex. Esc closes.";
        } else if (myPicks.size() == 1) {
            const QString single = Q(DescribePick(myPicks[0].shape));
            const int split = single.indexOf(' ');
            rows.push_back({single.left(split), single.mid(split + 1)});
            hint = "Click a second face, edge or vertex.";
            status = QString("Measure: %1   %2 -- click a second face, edge or vertex.")
                         .arg(first, single);
        } else {
            const MeasureResult result = MeasureBetween(myPicks[0].shape, myPicks[1].shape);
            if (!result.isValid) {
                hint = Q(result.error);
                status = QString("Measure: %1").arg(Q(result.error));
            } else {
                const gp_Vec delta = result.Delta();
                const QString distance = Q(FormatMeasureLength(result.distance));
                rows.push_back({"Distance", distance});
                rows.push_back({"ΔX", Q(FormatMeasureLength(delta.X()))});
                rows.push_back({"ΔY", Q(FormatMeasureLength(delta.Y()))});
                rows.push_back({"ΔZ", Q(FormatMeasureLength(delta.Z()))});
                status = QString("Distance %1   ΔX %2  ΔY %3  ΔZ %4")
                             .arg(distance, Q(FormatMeasureLength(delta.X())),
                                  Q(FormatMeasureLength(delta.Y())),
                                  Q(FormatMeasureLength(delta.Z())));
                if (result.hasAngle) {
                    const QString angle = QString("%1°").arg(result.angleDegrees, 0, 'f', 1);
                    rows.push_back({"Angle", angle});
                    status += QString("   Angle %1").arg(angle);
                }
                DrawResult(result);
            }
        }

        myPanel.Update(first, second, rows, hint);
        ShowStatus(myContext.parent, status);
        myContext.Redraw();
    }

    CommandContext    myContext;
    bool              myIsRunning = false;
    bool              myPressConsumed = false;
    std::vector<Pick> myPicks;
    std::vector<Handle(AIS_InteractiveObject)> myMarkers;
    MeasurePanel      myPanel;
};

class MeasureDistanceCommand : public Command
{
public:
    std::string Id() const override { return "inspect.measure_distance"; }
    std::string Title() const override { return "Measure"; }
    std::string Group() const override { return kInspectGroup; }
    std::string Section() const override { return "Measure"; }
    std::string Icon() const override { return "↔️"; }
    // I, as in Fusion. M belongs to Move, and a key bound twice fires neither.
    std::string Shortcut() const override { return "I"; }
    std::string Description() const override
    {
        return "Click two faces, edges or vertices to measure the shortest distance between them";
    }

    bool IsEnabled(const CommandContext& theContext) const override
    {
        if (MeasureTool::Instance().IsRunning()) {
            return true;
        }
        return theContext.document != nullptr && !theContext.document->Shape().IsNull()
            && !theContext.View().IsNull();
    }

    void Execute(CommandContext& theContext) override
    {
        MeasureTool::Instance().Start(theContext);
    }
};

// ---- Section View ----
//
// Fusion's Section Analysis: cut the model with a plane so you see inside.
// The cut is display-only -- a clip plane, never a feature -- so the
// document, its volume and the timeline are untouched.

class SectionController : public DocumentObserver
{
public:
    static SectionController& Instance()
    {
        // Deliberately leaked, like the tools: it holds OCCT handles that
        // must outlive the viewer's teardown.
        static SectionController* theInstance = new SectionController();
        return *theInstance;
    }

    bool IsActive() const { return myIsActive; }

    // The last settings, so turning the section back on returns to the
    // cut the user had rather than starting over.
    SectionAxis Axis() const { return myAxis; }
    double Offset() const { return myOffset; }
    bool HasSettings() const { return myHasSettings; }
    bool Flipped() const { return myFlipped; }

    // Show (or move) the cut. Removes the half of the model facing the
    // camera -- so the cut face looks at the viewer -- unless flipped.
    void Apply(const CommandContext& theContext, SectionAxis theAxis, double theOffset,
               bool theFlipped)
    {
        myContext = theContext;
        myAxis = theAxis;
        myOffset = theOffset;
        myFlipped = theFlipped;
        myHasSettings = true;

        const gp_Pln plane = SectionClipPlane(theAxis, theOffset, RemovesPositive());
        if (myPlane.IsNull()) {
            myPlane = new Graphic3d_ClipPlane(plane);
            // Fill the cut with the body's own colour, instead of leaving
            // the part open like a hollow shell.
            myPlane->SetCapping(Standard_True);
            myPlane->SetUseObjectMaterial(Standard_True);
        } else {
            myPlane->SetEquation(plane);
        }
        // Hatch it the way Fusion marks a section face, with a TEXTURE of
        // dark stripes modulating the body colour. OCCT's own capping
        // hatch is a stipple that DISCARDS the pixels between its lines:
        // a solid box cut at Z = 5 showed its inner walls and the grid
        // through the cap and read as an open tray. Re-set on every apply
        // so the stripe spacing follows the model's size.
        const TopoDS_Shape model =
            myContext.document != nullptr ? myContext.document->Shape() : TopoDS_Shape();
        myPlane->SetCappingTexture(HatchTexture(SectionHatchSpacing(model)));
        myPlane->SetOn(Standard_True);
        myIsActive = true;
        if (theContext.document != nullptr) {
            theContext.document->AddObserver(this);   // de-duplicates
        }
        Attach();
        myContext.Redraw();
    }

    void Disable(const CommandContext& theContext)
    {
        myContext = theContext;
        if (!myPlane.IsNull()) {
            const Handle(AIS_InteractiveContext) ctx = myContext.AisContext();
            if (!ctx.IsNull()) {
                AIS_ListOfInteractive displayed;
                ctx->DisplayedObjects(displayed);
                for (AIS_ListOfInteractive::Iterator it(displayed); it.More(); it.Next()) {
                    Detach(it.Value());
                }
            }
        }
        if (myContext.document != nullptr) {
            myContext.document->RemoveObserver(this);
        }
        myIsActive = false;
        myContext.Redraw();
    }

    void OnDocumentChanged(Document& theDocument) override
    {
        (void)theDocument;
        // MainWindow has just replaced every body object (it observes the
        // document first), and the new ones carry no clip plane. Without
        // this the section silently vanished on the first edit.
        if (myIsActive) {
            Attach();
            myContext.Redraw();
        }
    }

private:
    SectionController() = default;

    // White with a dark diagonal stripe, repeated so the stripes are
    // theSpacing apart in model units. Modulated, so it tints whatever
    // colour the body is rather than painting over it.
    Handle(Graphic3d_Texture2D) HatchTexture(double theSpacing)
    {
        constexpr int kSize = 64;
        if (myHatch.IsNull()) {
            Handle(Image_PixMap) image = new Image_PixMap();
            if (image->InitZero(Image_Format_RGB, kSize, kSize)) {
                for (int row = 0; row < kSize; ++row) {
                    for (int col = 0; col < kSize; ++col) {
                        // One stripe per tile, a quarter of it wide and
                        // dark enough to read at 1:1 -- a sixth-wide stripe
                        // at half brightness was barely visible on a
                        // 20 mm part in the home view.
                        const bool stripe = (row + col) % kSize < kSize / 4;
                        const Standard_Byte value = stripe ? 90 : 255;
                        Standard_Byte* pixel = image->ChangeRawValue(row, col);
                        pixel[0] = pixel[1] = pixel[2] = value;
                    }
                }
            }
            myHatch = new Graphic3d_Texture2D(image);
            myHatch->EnableModulate();
            myHatch->EnableRepeat();
            myHatch->GetParams()->SetFilter(Graphic3d_TOTF_BILINEAR);
        }
        // Texture coordinates on the cap are model units times the scale,
        // and a tile holds one stripe.
        const float scale = static_cast<float>(1.0 / theSpacing);
        myHatch->GetParams()->SetScale(Graphic3d_Vec2(scale, scale));
        return myHatch;
    }

    bool RemovesPositive() const
    {
        gp_Vec towardEye(0.0, 0.0, 1.0);
        const Handle(V3d_View) view = myContext.View();
        if (!view.IsNull()) {
            Standard_Real x = 0.0, y = 0.0, z = 0.0;
            view->Proj(x, y, z);
            towardEye = gp_Vec(x, y, z);
        }
        const bool facingEye = SectionRemovesPositive(myAxis, towardEye);
        return myFlipped ? !facingEye : facingEye;
    }

    // Bodies only, not the whole view: a view-wide plane also cut the
    // grid, the origin axes, sketches and the measure markers, none of
    // which Fusion's section touches.
    void Attach()
    {
        for (const Handle(AIS_Shape)& object : BodyObjects(myContext)) {
            const Handle(Graphic3d_SequenceOfHClipPlane)& planes = object->ClipPlanes();
            bool has = false;
            if (!planes.IsNull()) {
                for (Graphic3d_SequenceOfHClipPlane::Iterator it(*planes); it.More(); it.Next()) {
                    has = has || it.Value() == myPlane;
                }
            }
            if (!has) {
                object->AddClipPlane(myPlane);
            }
        }
    }

    void Detach(const Handle(AIS_InteractiveObject)& theObject)
    {
        if (theObject.IsNull() || myPlane.IsNull()) {
            return;
        }
        const Handle(Graphic3d_SequenceOfHClipPlane)& planes = theObject->ClipPlanes();
        if (planes.IsNull()) {
            return;
        }
        for (Graphic3d_SequenceOfHClipPlane::Iterator it(*planes); it.More(); it.Next()) {
            if (it.Value() == myPlane) {
                theObject->RemoveClipPlane(myPlane);
                return;
            }
        }
    }

    CommandContext              myContext;
    Handle(Graphic3d_ClipPlane) myPlane;
    Handle(Graphic3d_Texture2D) myHatch;
    bool                        myIsActive = false;
    bool                        myHasSettings = false;
    SectionAxis                 myAxis = SectionAxis::Y;
    double                      myOffset = 0.0;
    bool                        myFlipped = false;
};

// The plane, where along it, and which side goes. Every change re-cuts
// the model immediately, so the dialog is a live preview; Cancel puts the
// model back whole.
bool AskForSection(CommandContext& theContext)
{
    SectionController& section = SectionController::Instance();
    const TopoDS_Shape model =
        theContext.document != nullptr ? theContext.document->Shape() : TopoDS_Shape();

    QDialog dialog(theContext.parent);
    dialog.setWindowTitle("Section Analysis");

    QFormLayout* form = new QFormLayout(&dialog);

    // Radio buttons rather than a combo: one click each, all three visible
    // at once, and the harness can press them by name.
    QWidget* planeRow = new QWidget(&dialog);
    QHBoxLayout* planeLayout = new QHBoxLayout(planeRow);
    planeLayout->setContentsMargins(0, 0, 0, 0);
    QButtonGroup* planes = new QButtonGroup(&dialog);
    const SectionAxis axes[] = {SectionAxis::Z, SectionAxis::Y, SectionAxis::X};
    for (const SectionAxis axis : axes) {
        QRadioButton* button = new QRadioButton(Q(SectionPlaneName(axis)), planeRow);
        button->setToolTip(QString("Cut across %1").arg(Q(SectionAxisName(axis))));
        planes->addButton(button, static_cast<int>(axis));
        planeLayout->addWidget(button);
    }
    planeLayout->addStretch();
    form->addRow("Plane", planeRow);

    UnitLineEdit* offset = new UnitLineEdit(UnitKind::Length, &dialog);
    form->addRow("Offset", offset);

    QCheckBox* flip = new QCheckBox("Flip", &dialog);
    flip->setToolTip("Keep the other half instead");
    form->addRow(QString(), flip);

    QDialogButtonBox* buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);

    // Start where the user left off; the first time, cut through the
    // middle of the model on the XZ (front) plane.
    SectionAxis axis = section.HasSettings() ? section.Axis() : SectionAxis::Y;
    offset->SetValue(section.HasSettings() ? section.Offset() : SectionMiddle(model, axis));
    flip->setChecked(section.HasSettings() && section.Flipped());
    planes->button(static_cast<int>(axis))->setChecked(true);

    auto apply = [&]() {
        section.Apply(theContext, static_cast<SectionAxis>(planes->checkedId()), offset->Value(),
                      flip->isChecked());
    };
    // A new plane recentres the cut on the model along ITS axis: an offset
    // that was the middle in Y means nothing in Z.
    QObject::connect(planes, &QButtonGroup::idClicked, &dialog, [&](int theId) {
        offset->SetValue(SectionMiddle(model, static_cast<SectionAxis>(theId)));
        apply();
    });
    QObject::connect(offset, &UnitLineEdit::ValueChanged, &dialog, [&](double) { apply(); });
    QObject::connect(flip, &QCheckBox::toggled, &dialog, [&](bool) { apply(); });

    apply();
    if (dialog.exec() != QDialog::Accepted) {
        section.Disable(theContext);
        return false;
    }
    apply();
    return true;
}

class SectionViewCommand : public Command
{
public:
    std::string Id() const override { return "inspect.section_view"; }
    std::string Title() const override { return "Section Analysis"; }
    std::string Group() const override { return kInspectGroup; }
    std::string Section() const override { return "Analyze"; }
    std::string Icon() const override { return "🔪"; }
    std::string Shortcut() const override { return "X"; }
    std::string Description() const override
    {
        return "Cut the model with a plane to see inside it (press again to turn it off)";
    }

    bool IsCheckable() const override { return true; }

    bool IsEnabled(const CommandContext& theContext) const override
    {
        // Always able to turn an active section OFF, even if the model has
        // since emptied; otherwise only with something to cut.
        if (SectionController::Instance().IsActive()) {
            return true;
        }
        return !theContext.View().IsNull() && theContext.document != nullptr
            && !theContext.document->Shape().IsNull();
    }

    bool IsChecked(const CommandContext& theContext) const override
    {
        (void)theContext;
        return SectionController::Instance().IsActive();
    }

    void Execute(CommandContext& theContext) override
    {
        SectionController& section = SectionController::Instance();
        if (section.IsActive()) {
            section.Disable(theContext);
            ShowStatus(theContext.parent, "Section analysis off.");
            return;
        }
        if (!AskForSection(theContext)) {
            ShowStatus(theContext.parent, QString());
            return;
        }
        ShowStatus(theContext.parent,
                   QString("Section: %1 plane at %2 = %3. Press X to turn it off.")
                       .arg(Q(SectionPlaneName(section.Axis())),
                            Q(SectionAxisName(section.Axis())),
                            Q(FormatMeasureLength(section.Offset()))));
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
