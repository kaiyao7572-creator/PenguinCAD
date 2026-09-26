#include "core/Command.h"
#include "core/Document.h"
#include "core/Registration.h"
#include "core/ViewportInteraction.h"

#include "OcctViewport.h"
#include "view/ViewOrientation.h"

#include <AIS_InteractiveContext.hxx>
#include <AIS_ListOfInteractive.hxx>
#include <AIS_Shape.hxx>
#include <AIS_ViewCube.hxx>
#include <AIS_AnimationCamera.hxx>
#include <AIS_DisplayMode.hxx>
#include <Aspect_Window.hxx>
#include <Bnd_Box.hxx>
#include <Graphic3d_ArrayOfPolylines.hxx>
#include <Graphic3d_AspectLine3d.hxx>
#include <Graphic3d_Group.hxx>
#include <Graphic3d_ZLayerId.hxx>
#include <Precision.hxx>
#include <Prs3d_Presentation.hxx>
#include <PrsMgr_PresentationManager.hxx>
#include <SelectMgr_Selection.hxx>
#include <gp.hxx>
#include <gp_Vec.hxx>
#include <Aspect_Grid.hxx>
#include <Aspect_TypeOfTriedronPosition.hxx>
#include <Graphic3d_Camera.hxx>
#include <Graphic3d_TransformPers.hxx>
#include <Graphic3d_TransModeFlags.hxx>
#include <Graphic3d_Vec2.hxx>
#include <Prs3d_DatumAspect.hxx>
#include <Prs3d_Drawer.hxx>
#include <Prs3d_ShadingAspect.hxx>
#include <Prs3d_TextAspect.hxx>
#include <V3d_TypeOfOrientation.hxx>
#include <V3d_View.hxx>
#include <V3d_Viewer.hxx>

#include <cstdlib>
#include <memory>
#include <string>
#include <utility>

#include <QElapsedTimer>
#include <QMainWindow>
#include <QObject>
#include <QTimer>
#include <QStatusBar>
#include <QString>

namespace lcad {

namespace {

const char* const kViewGroup = "View";

// ---- display mode (Shaded / Wireframe / Shaded+Edges) ----
//
// MainWindow::redisplayDocument() throws away the old AIS_Shape and builds
// a brand new one -- always AIS_Shaded -- on every document change. A
// chosen Wireframe/Shaded+Edges preference would silently revert the
// instant the user edited anything unless something re-asserts it, so this
// registers itself as a DocumentObserver (a public, intended extension
// point) and reapplies the current mode after every rebuild.
class DisplayModeController : public DocumentObserver
{
public:
    enum class Mode { Shaded, Wireframe, ShadedWithEdges };

    static DisplayModeController& Instance()
    {
        static DisplayModeController theInstance;
        return theInstance;
    }

    Mode Current() const { return myMode; }

    void SetMode(const CommandContext& theContext, Mode theMode)
    {
        EnsureObserving(theContext);
        myMode = theMode;
        Apply(theContext.AisContext());
    }

    void OnDocumentChanged(Document& /*theDocument*/) override { Apply(myContext.AisContext()); }

private:
    DisplayModeController() = default;

    void EnsureObserving(const CommandContext& theContext)
    {
        myContext = theContext;
        if (myObserving || theContext.document == nullptr) {
            return;
        }
        theContext.document->AddObserver(this);
        myObserving = true;
    }

    void Apply(const Handle(AIS_InteractiveContext)& theAisContext)
    {
        if (theAisContext.IsNull()) {
            return;
        }

        const Standard_Integer dispMode = (myMode == Mode::Wireframe) ? AIS_WireFrame : AIS_Shaded;
        const Standard_Boolean boundaries = (myMode == Mode::ShadedWithEdges);

        // The context-wide default only benefits objects displayed from now
        // on; walking what's already displayed makes the change visible
        // immediately too (and re-asserting both here covers a freshly
        // rebuilt AIS_Shape either way).
        if (!theAisContext->DefaultDrawer().IsNull()) {
            theAisContext->DefaultDrawer()->SetFaceBoundaryDraw(boundaries);
        }
        theAisContext->SetDisplayMode(dispMode, Standard_False);

        AIS_ListOfInteractive objects;
        theAisContext->DisplayedObjects(objects);
        for (AIS_ListOfInteractive::Iterator it(objects); it.More(); it.Next()) {
            const Handle(AIS_Shape) shape = Handle(AIS_Shape)::DownCast(it.Value());
            if (shape.IsNull()) {
                continue;   // leave the ground grid / origin axes alone
            }
            if (!shape->Attributes().IsNull()) {
                shape->Attributes()->SetFaceBoundaryDraw(boundaries);
            }
            theAisContext->SetDisplayMode(shape, dispMode, Standard_False);
            theAisContext->Redisplay(shape, Standard_False);
        }
        theAisContext->UpdateCurrentViewer();
    }

    CommandContext myContext;
    Mode           myMode       = Mode::Shaded;
    bool           myObserving  = false;
};

// ---- camera flights ----
//
// Every view change on this tab -- a cube click, the home button, a
// standard view, Fit -- swings the camera instead of cutting to the new
// view, because that is how Fusion does it and the swing is what tells the
// user where they have ended up. The app has no render loop (OCCT redraws
// only in response to events), so a timer pumps the frames.
//
// Seconds a swing takes. Long enough to read as a move, short enough not
// to be in the way; Fusion's own sits around half a second.
constexpr double kFlightSeconds = 0.45;
constexpr int    kFrameMilliseconds = 16;

// Border left around a fitted model, as a fraction of the view. OCCT's
// 1% default runs the model to the very edge of the window and parks its
// corners under the view cube; this leaves the part filling about five
// sixths of the view, as a Fusion fit does.
constexpr double kFitMargin = 0.2;

class CameraFlight
{
public:
    static CameraFlight& Instance()
    {
        // Leaked on purpose: static destruction runs after the viewer is
        // gone, and the OCCT handles below must not be released into it.
        static CameraFlight* theInstance = new CameraFlight();
        return *theInstance;
    }

    // Swings theView from wherever it is now to theTarget. A second request
    // mid-swing simply takes over from where the first one had got to.
    void Fly(const Handle(V3d_View)& theView, const Handle(Graphic3d_Camera)& theTarget)
    {
        if (theView.IsNull() || theView->Camera().IsNull() || theTarget.IsNull()) {
            return;
        }
        myAnimation->SetView(theView);
        myAnimation->SetCameraStart(new Graphic3d_Camera(theView->Camera()));
        myAnimation->SetCameraEnd(theTarget);
        // Update() does nothing to an animation that is not started, which
        // would leave the camera parked until the last frame copied the end
        // in: a cut dressed up as a swing.
        myAnimation->Start(Standard_False);
        myView = theView;
        myClock.start();
        myTimer->start();
    }

    // The user grabbed the camera mid-swing: leave it where it has got to
    // rather than dragging it on to a view they no longer want.
    void Stop()
    {
        if (myTimer->isActive()) {
            myTimer->stop();
            myAnimation->Stop();
        }
    }

private:
    CameraFlight()
        : myAnimation(new AIS_AnimationCamera("ViewFlight", Handle(V3d_View)())),
          myTimer(new QTimer())
    {
        myAnimation->SetOwnDuration(kFlightSeconds);
        myTimer->setInterval(kFrameMilliseconds);
        QObject::connect(myTimer, &QTimer::timeout, [this]() { Tick(); });
    }

    void Tick()
    {
        if (myView.IsNull()) {
            myTimer->stop();
            return;
        }
        // Paced by the clock rather than by counting ticks, so a slow frame
        // makes the swing choppier, never longer.
        const double t = static_cast<double>(myClock.elapsed()) / (1000.0 * kFlightSeconds);
        if (t >= 1.0) {
            // Land exactly on the named view. An almost-front view is worse
            // than no animation at all.
            myTimer->stop();
            myAnimation->Stop();
            myView->Camera()->Copy(myAnimation->CameraEnd());
        } else {
            myAnimation->Update(EaseInOut(t) * kFlightSeconds);
        }
        myView->Invalidate();
        myView->Redraw();
    }

    Handle(AIS_AnimationCamera) myAnimation;
    Handle(V3d_View)            myView;
    QTimer*                     myTimer;
    QElapsedTimer               myClock;
};

// Frames the model in theCamera without turning it. Returns false and
// leaves the camera alone when there is nothing to frame, so an empty
// design keeps its zoom instead of snapping out to the origin axes.
bool FitCameraToModel(const Handle(AIS_InteractiveContext)& theContext,
                      const Handle(V3d_View)&               theView,
                      const Handle(Graphic3d_Camera)&       theCamera)
{
    if (theContext.IsNull() || theView.IsNull() || theCamera.IsNull()) {
        return false;
    }
    AIS_ListOfInteractive objects;
    theContext->DisplayedObjects(objects);
    const Bnd_Box bounds = ModelBounds(objects);
    if (bounds.IsVoid()) {
        return false;
    }
    return theView->FitMinMax(theCamera, bounds, kFitMargin, 10.0 * Precision::Confusion())
           == Standard_True;
}

// Swings the camera to thePose, fitted to the model. Turned about the world
// origin, which for an empty design is the only thing worth looking at,
// and which the fit replaces with the model's own centre whenever there is
// a model.
void FlyToPose(const Handle(AIS_InteractiveContext)& theContext,
               const Handle(V3d_View)&               theView,
               const ViewPose&                       thePose)
{
    if (theView.IsNull() || theView->Camera().IsNull()) {
        return;
    }
    Handle(Graphic3d_Camera) target = new Graphic3d_Camera(theView->Camera());
    AimCamera(target, thePose, gp::Origin());
    FitCameraToModel(theContext, theView, target);
    CameraFlight::Instance().Fly(theView, target);
}

void FlyToStandardView(const CommandContext& theContext, V3d_TypeOfOrientation theOrientation)
{
    FlyToPose(theContext.AisContext(), theContext.View(), StandardViewPose(theOrientation));
}

// Fusion's home view: in from the front-right-top corner.
constexpr V3d_TypeOfOrientation kHomeOrientation = V3d_TypeOfOrientation_Zup_AxoRight;

// ---- view cube ----
//
// AIS_ViewCube's own click handling turns the camera with SetProj's up
// rule, then keeps whichever quarter-turn of it is nearest the current up
// -- which from a top view leaves every edge and corner view lying on its
// side -- and fits "the scene", origin axes and all, 360mm end to end:
// clicking FRONT on a 30mm part zoomed out until the part was a sliver.
// It also cut straight to the new view, having been handed an animation
// with no duration. HandleClick still lands here, so the cube keeps OCCT's
// detection and hover highlighting and takes Fusion's camera from us.
class FusionViewCube : public AIS_ViewCube
{
    DEFINE_STANDARD_RTTI_INLINE(FusionViewCube, AIS_ViewCube)

public:
    void StartAnimation(const Handle(AIS_ViewCubeOwner)& theOwner) override
    {
        const Handle(AIS_InteractiveContext) context = GetContext();
        if (theOwner.IsNull() || context.IsNull()) {
            return;
        }
        const Handle(V3d_View) view = context->LastActiveView();
        if (view.IsNull() || view->Camera().IsNull()) {
            return;
        }
        const ViewPose current{view->Camera()->Direction(), view->Camera()->Up()};
        FlyToPose(context, view, CubeClickPose(theOwner->MainOrientation(), current));
    }
};

// Fusion's home button: a small house by the cube's top-left corner that
// shows while the cursor is over the cube and flies the camera home.
//
// Drawn in 2D screen space, anchored to the same corner as the cube, and
// displayed with no selection mode: the click is hit-tested in pixels by
// ViewCubeInteraction, so the icon can never steal a pick from a body.
class HomeIcon : public AIS_InteractiveObject
{
    DEFINE_STANDARD_RTTI_INLINE(HomeIcon, AIS_InteractiveObject)

public:
    HomeIcon(const Graphic3d_Vec2i& theOffsetFromCorner, double theSize, double theLineWidth)
        : mySize(theSize),
          myLineWidth(theLineWidth)
    {
        SetTransformPersistence(new Graphic3d_TransformPers(
            Graphic3d_TMF_2d, Aspect_TOTP_RIGHT_UPPER, theOffsetFromCorner));
        SetZLayer(Graphic3d_ZLayerId_TopOSD);
        SetMutable(Standard_True);
    }

    bool IsHot() const { return myHot; }

    void SetHot(bool theHot)
    {
        if (myHot != theHot) {
            myHot = theHot;
            SetToUpdate();
        }
    }

    Standard_Boolean AcceptDisplayMode(const Standard_Integer theMode) const override
    {
        return theMode == 0;
    }

private:
    void Compute(const Handle(PrsMgr_PresentationManager)& /*theManager*/,
                 const Handle(Prs3d_Presentation)& thePrs,
                 const Standard_Integer            theMode) override
    {
        if (theMode != 0) {
            return;
        }
        // An outline house, in pixels about the anchor, +Y up: a roof
        // chevron, then walls with a door notched out of the floor line.
        const double s = mySize * 0.5;
        Handle(Graphic3d_ArrayOfPolylines) house = new Graphic3d_ArrayOfPolylines(11, 2);
        house->AddBound(3);
        house->AddVertex(-s, 0.05 * s, 0.0);
        house->AddVertex(0.0, s, 0.0);
        house->AddVertex(s, 0.05 * s, 0.0);
        house->AddBound(8);
        house->AddVertex(-0.65 * s, 0.38 * s, 0.0);
        house->AddVertex(-0.65 * s, -s, 0.0);
        house->AddVertex(-0.2 * s, -s, 0.0);
        house->AddVertex(-0.2 * s, -0.35 * s, 0.0);
        house->AddVertex(0.2 * s, -0.35 * s, 0.0);
        house->AddVertex(0.2 * s, -s, 0.0);
        house->AddVertex(0.65 * s, -s, 0.0);
        house->AddVertex(0.65 * s, 0.38 * s, 0.0);

        // Fusion's hover blue when the cursor is on it, a quiet grey
        // otherwise, so it reads as available without shouting.
        const Quantity_Color color = myHot ? Quantity_Color(0.33, 0.63, 1.00, Quantity_TOC_sRGB)
                                           : Quantity_Color(0.82, 0.82, 0.82, Quantity_TOC_sRGB);
        Handle(Graphic3d_Group) group = thePrs->NewGroup();
        group->SetGroupPrimitivesAspect(
            new Graphic3d_AspectLine3d(color, Aspect_TOL_SOLID, myLineWidth));
        group->AddPrimitiveArray(house);
    }

    void ComputeSelection(const Handle(SelectMgr_Selection)& /*theSelection*/,
                          const Standard_Integer /*theMode*/) override
    {
    }

    double mySize;
    double myLineWidth;
    bool   myHot = false;
};

// Layout of the cube corner, in LOGICAL pixels. OCCT draws these in device
// pixels, so they are scaled by the screen's pixel ratio when applied --
// on a 2x screen an unscaled cube and icon come out half the size a user
// sees in every other application.
//
// The cube's centre sits far enough in from the corner that nothing is
// cut off in any orientation: its bevelled half-diagonal is 56 and its
// axis labels reach about 70 from the centre.
constexpr double kCubeCenterFromCorner = 72.0;   // right and top edges to cube centre
constexpr double kCubeSize = 50.0;               // side of the cube proper
constexpr double kHomeFromRight = 132.0;         // home icon centre: off the cube's
constexpr double kHomeFromTop = 14.0;            // top-left, as in Fusion
constexpr double kHomeSize = 15.0;
constexpr double kHomeLineWidth = 1.5;
constexpr double kHomeHitSlop = 4.0;             // a near miss is still a click
// The patch of screen that counts as "over the cube" for showing the home
// button: from this far in from the right edge, down to this far from the
// top.
constexpr double kCubeZoneFromRight = 150.0;
constexpr double kCubeZoneFromTop = 150.0;

// AIS_ViewCube only detects clicks (it deliberately never enters the
// context's normal "selected" list -- see its header comment), so the app
// is expected to notice the click itself and hand the detected owner back.
// This is that noticing: a ViewportInteraction that sits at the BOTTOM of
// the interaction stack for the app's lifetime. Tools push above it, get
// first refusal on every event, and anything they do not consume falls
// through to here -- so the cube keeps working while a tool is up rather
// than going quiet.
//
// It reports IsExclusive() == false because it is not a tool the user
// started: it is always pushed. Tools asking "is the user in the middle of
// something?" must not be answered "yes, the view cube".
class ViewCubeInteraction : public ViewportInteraction
{
public:
    // Permanently pushed background handler, not a tool -- see above.
    bool IsExclusive() const override { return false; }

    static ViewCubeInteraction& Instance()
    {
        static ViewCubeInteraction theInstance;
        return theInstance;
    }

    bool IsVisible() const { return myVisible; }

    // There is no "on startup" hook src/view/ is allowed to reach (that's
    // MainWindow, off limits, and it builds the ribbon before the GL
    // window's first expose event anyway, so the AIS context is still null
    // at that point). IsEnabled()/IsChecked() are polled constantly once
    // the app is alive, so this is called from both of them to lazily
    // create and show the cube -- shown by default -- the moment a real
    // AIS context exists; it quietly no-ops and gets retried on the next
    // poll until then.
    void EnsureMaterialized(const CommandContext& theContext)
    {
        if (myMaterialized) {
            return;
        }
        const Handle(AIS_InteractiveContext) aisContext = theContext.AisContext();
        const Handle(V3d_View) view = theContext.View();
        if (aisContext.IsNull() || view.IsNull()) {
            return;
        }

        myContext = theContext;
        myPixelRatio = theContext.viewport != nullptr ? theContext.viewport->devicePixelRatioF()
                                                      : 1.0;

        myCube = new FusionViewCube();
        myCube->SetSize(kCubeSize * myPixelRatio);
        StyleLikeFusion(myCube);
        // HandleClick must not spin on the cube's own animation: the swing
        // is CameraFlight's, paced by a timer, and the cube's never starts.
        myCube->SetFixedAnimationLoop(false);
        const int centre = Pixels(kCubeCenterFromCorner);
        myCube->SetTransformPersistence(new Graphic3d_TransformPers(
            Graphic3d_TMF_TriedronPers, Aspect_TOTP_RIGHT_UPPER, Graphic3d_Vec2i(centre, centre)));

        myHome = new HomeIcon(Graphic3d_Vec2i(Pixels(kHomeFromRight), Pixels(kHomeFromTop)),
                              kHomeSize * myPixelRatio, kHomeLineWidth * myPixelRatio);

        if (myVisible) {
            aisContext->Display(myCube, Standard_False);
        }
        if (theContext.viewport != nullptr) {
            theContext.viewport->PushInteraction(this);
        }
        myMaterialized = true;
    }

    void SetVisible(const CommandContext& theContext, bool theVisible)
    {
        EnsureMaterialized(theContext);
        myContext = theContext;
        myVisible = theVisible;

        const Handle(AIS_InteractiveContext) aisContext = theContext.AisContext();
        if (aisContext.IsNull() || myCube.IsNull()) {
            return;
        }
        if (myVisible) {
            aisContext->Display(myCube, Standard_False);
        } else {
            aisContext->Erase(myCube, Standard_False);
            ShowHome(false, false);
        }
    }

    bool OnMousePress(const Graphic3d_Vec2i& thePos, Qt::MouseButton theButton,
                      Qt::KeyboardModifiers theModifiers) override
    {
        (void)theModifiers;
        myPressOnCube = false;
        myPressOnHome = false;

        if (myVisible && theButton == Qt::LeftButton) {
            if (myHomeShown && IsOnHome(thePos)) {
                myPressOnHome = true;
                return true;
            }
            // Detect at the press point rather than trusting the last hover:
            // a click that arrives without a move first (a tablet tap, the
            // script harness) would otherwise ask about wherever the cursor
            // last was.
            const Handle(AIS_InteractiveContext) ctx = myContext.AisContext();
            const Handle(V3d_View) view = myContext.View();
            if (!ctx.IsNull() && !view.IsNull()) {
                ctx->MoveTo(thePos.x(), thePos.y(), view, Standard_False);
            }
            if (!DetectedCubeOwner().IsNull()) {
                myPressOnCube = true;
                return true;
            }
        }

        // Any other press means the user is taking the camera themselves --
        // orbiting, panning, selecting -- and a swing still in flight would
        // wrestle it back.
        CameraFlight::Instance().Stop();
        return false;   // let the click fall through to normal select/orbit
    }

    bool OnMouseMove(const Graphic3d_Vec2i& thePos, Qt::MouseButtons theButtons,
                     Qt::KeyboardModifiers theModifiers) override
    {
        (void)theModifiers;
        if (myVisible && theButtons == Qt::NoButton) {
            const bool inZone = IsInCubeZone(thePos);
            ShowHome(inZone, inZone && IsOnHome(thePos));
        }
        // Never consumed: the cube's own hover highlight is the viewport's
        // normal MoveTo, which has to keep running underneath.
        return false;
    }

    bool OnMouseRelease(const Graphic3d_Vec2i& thePos, Qt::MouseButton theButton,
                        Qt::KeyboardModifiers theModifiers) override
    {
        (void)theModifiers;
        // The press was consumed above whenever this fires with a flag set,
        // so the viewport's own controller never saw the button go down --
        // handing it the release would leave it in a state it can't make
        // sense of (same reasoning the sketch tools' OnMouseRelease
        // documents).
        if (myPressOnHome) {
            myPressOnHome = false;
            // Like any button: sliding off before letting go cancels.
            if (theButton == Qt::LeftButton && IsOnHome(thePos)) {
                FlyToStandardView(myContext, kHomeOrientation);
            }
            return true;
        }
        if (!myPressOnCube) {
            return false;
        }
        myPressOnCube = false;
        if (theButton != Qt::LeftButton) {
            return true;
        }

        const Handle(AIS_ViewCubeOwner) owner = DetectedCubeOwner();
        if (!owner.IsNull() && !myCube.IsNull()) {
            // Straight out of AIS_ViewCube.hxx's own usage note: the app
            // detects the click and hands the owner back in. HandleClick
            // lands in FusionViewCube::StartAnimation, which swings the
            // camera to the face, edge or corner view that was clicked.
            myCube->HandleClick(owner);
        }
        return true;
    }

private:
    ViewCubeInteraction() = default;

    static void StyleLikeFusion(const Handle(AIS_ViewCube)& theCube)
    {
        // The cube's corner triad in the same red/green/blue as the origin
        // axes in the scene, so which way X runs -- and that Z, the blue
        // one, is up -- can be read straight off the cube. OCCT's default
        // is grey arrows with yellow letters, which match nothing else on
        // screen.
        //
        // The cube has no datum aspect of its own: it reads the one it
        // inherits from the context's default drawer once displayed, so
        // colouring that would recolour every trihedron in the app, and
        // colouring it before Display finds nothing at all. Hence a fresh
        // one, owned by the cube.
        Handle(Prs3d_DatumAspect) datum = new Prs3d_DatumAspect();
        const Prs3d_DatumParts axes[3] = {Prs3d_DatumParts_XAxis, Prs3d_DatumParts_YAxis,
                                          Prs3d_DatumParts_ZAxis};
        const Quantity_Color colors[3] = {Quantity_Color(0.85, 0.20, 0.20, Quantity_TOC_RGB),
                                          Quantity_Color(0.25, 0.80, 0.25, Quantity_TOC_RGB),
                                          Quantity_Color(0.25, 0.45, 0.95, Quantity_TOC_RGB)};
        for (int i = 0; i < 3; ++i) {
            if (!datum->ShadingAspect(axes[i]).IsNull()) {
                datum->ShadingAspect(axes[i])->SetColor(colors[i]);
            }
            if (!datum->TextAspect(axes[i]).IsNull()) {
                datum->TextAspect(axes[i])->SetColor(colors[i]);
            }
        }
        theCube->Attributes()->SetDatumAspect(datum);

        // Fusion's cube is one pale block with soft bevels; OCCT's near-black
        // bevels read as a frame around every face.
        const Quantity_Color bevel(0.62, 0.62, 0.62, Quantity_TOC_sRGB);
        theCube->BoxEdgeStyle()->SetColor(bevel);
        theCube->BoxCornerStyle()->SetColor(bevel);

        // Hovering a face, edge or corner lights it in Fusion's soft blue;
        // OCCT's stock cyan is a warning colour, not a hover. The cube
        // paints its highlight from the drawer's shading aspect, so that is
        // the one that has to change.
        const Handle(Prs3d_Drawer)& hover = theCube->DynamicHilightAttributes();
        if (!hover.IsNull()) {
            const Quantity_Color blue(0.47, 0.71, 1.00, Quantity_TOC_sRGB);
            hover->SetColor(blue);
            if (!hover->ShadingAspect().IsNull()) {
                hover->ShadingAspect()->SetColor(blue);
            }
        }
    }

    int Pixels(double theLogical) const
    {
        return static_cast<int>(theLogical * myPixelRatio + 0.5);
    }

    Handle(AIS_ViewCubeOwner) DetectedCubeOwner() const
    {
        const Handle(AIS_InteractiveContext) ctx = myContext.AisContext();
        if (ctx.IsNull()) {
            return Handle(AIS_ViewCubeOwner)();
        }
        return Handle(AIS_ViewCubeOwner)::DownCast(ctx->DetectedOwner());
    }

    // Window width in device pixels, which is what both OCCT's corner
    // anchoring and the positions handed to the interaction use.
    int ViewWidth() const
    {
        const Handle(V3d_View) view = myContext.View();
        if (view.IsNull() || view->Window().IsNull()) {
            return 0;
        }
        Standard_Integer width = 0;
        Standard_Integer height = 0;
        view->Window()->Size(width, height);
        return width;
    }

    bool IsInCubeZone(const Graphic3d_Vec2i& thePos) const
    {
        const int width = ViewWidth();
        return width > 0 && thePos.x() >= width - Pixels(kCubeZoneFromRight)
               && thePos.y() <= Pixels(kCubeZoneFromTop);
    }

    bool IsOnHome(const Graphic3d_Vec2i& thePos) const
    {
        const int width = ViewWidth();
        if (width <= 0) {
            return false;
        }
        const int centreX = width - Pixels(kHomeFromRight);
        const int centreY = Pixels(kHomeFromTop);
        const int reach = Pixels(kHomeSize * 0.5 + kHomeHitSlop);
        return std::abs(thePos.x() - centreX) <= reach && std::abs(thePos.y() - centreY) <= reach;
    }

    void ShowHome(bool theShown, bool theHot)
    {
        const Handle(AIS_InteractiveContext) ctx = myContext.AisContext();
        if (ctx.IsNull() || myHome.IsNull()) {
            return;
        }
        if (theShown == myHomeShown && theHot == myHome->IsHot()) {
            return;
        }
        myHome->SetHot(theHot);
        if (theShown) {
            // Mode -1: never pickable, see HomeIcon.
            if (myHomeShown) {
                ctx->Redisplay(myHome, Standard_False);
            } else {
                ctx->Display(myHome, 0, -1, Standard_False);
            }
        } else if (myHomeShown) {
            ctx->Erase(myHome, Standard_False);
        }
        myHomeShown = theShown;
        myContext.Redraw();
    }

    CommandContext       myContext;
    Handle(AIS_ViewCube) myCube;
    Handle(HomeIcon)     myHome;
    double               myPixelRatio   = 1.0;
    bool                 myVisible      = true;   // shown by default
    bool                 myMaterialized = false;
    bool                 myPressOnCube  = false;
    bool                 myPressOnHome  = false;
    bool                 myHomeShown    = false;
};

// ---- commands ----

// One command per standard orientation; they differ only in id/label/icon
// and which V3d_TypeOfOrientation they apply, so a single parameterized
// class stands in for all seven (same idea as SketchToolCommand).
class StandardViewCommand : public Command
{
public:
    StandardViewCommand(std::string theId, std::string theTitle, std::string theIcon,
                        std::string theShortcut, std::string theDescription,
                        V3d_TypeOfOrientation theOrientation)
        : myId(std::move(theId)),
          myTitle(std::move(theTitle)),
          myIcon(std::move(theIcon)),
          myShortcut(std::move(theShortcut)),
          myDescription(std::move(theDescription)),
          myOrientation(theOrientation)
    {
    }

    std::string Id() const override { return myId; }
    std::string Title() const override { return myTitle; }
    std::string Group() const override { return kViewGroup; }
    std::string Section() const override { return "Orientation"; }
    std::string Icon() const override { return myIcon; }
    std::string Shortcut() const override { return myShortcut; }
    std::string Description() const override { return myDescription; }

    bool IsEnabled(const CommandContext& theContext) const override
    {
        return !theContext.View().IsNull();
    }

    void Execute(CommandContext& theContext) override
    {
        // Not V3d_View::SetProj + OcctViewport::FitAll: that cut to the new
        // view instead of swinging there, and its fit framed the origin
        // axes rather than the model.
        FlyToStandardView(theContext, myOrientation);
    }

private:
    std::string           myId;
    std::string           myTitle;
    std::string           myIcon;
    std::string           myShortcut;
    std::string           myDescription;
    V3d_TypeOfOrientation myOrientation;
};

class FitAllCommand : public Command
{
public:
    std::string Id() const override { return "view.fit_all"; }
    std::string Title() const override { return "Fit All"; }
    std::string Group() const override { return kViewGroup; }
    std::string Section() const override { return "Orientation"; }
    std::string Icon() const override { return "🔍"; }
    // F6, as in Fusion. F belongs to Fillet, and a key bound twice fires neither.
    std::string Shortcut() const override { return "F6"; }
    std::string Description() const override { return "Frame the entire model in the viewport"; }

    bool IsEnabled(const CommandContext& theContext) const override
    {
        return !theContext.View().IsNull();
    }

    void Execute(CommandContext& theContext) override
    {
        const Handle(V3d_View) view = theContext.View();
        if (view.IsNull() || view->Camera().IsNull()) {
            return;
        }
        Handle(Graphic3d_Camera) target = new Graphic3d_Camera(view->Camera());
        if (!FitCameraToModel(theContext.AisContext(), view, target)) {
            // Nothing to frame. Bring the origin back to the middle instead,
            // at the current zoom -- a user lost in an empty design presses
            // Fit to find their way home, and framing nothing does nothing.
            // A pan, eye and centre together: SetCenter alone keeps the eye
            // where it is and so turns the camera to face the origin.
            const gp_Vec shift(target->Center(), gp::Origin());
            target->SetEyeAndCenter(target->Eye().Translated(shift), gp::Origin());
        }
        CameraFlight::Instance().Fly(view, target);
    }
};

class DisplayModeCommand : public Command
{
public:
    DisplayModeCommand(std::string theId, std::string theTitle, std::string theIcon,
                       std::string theDescription, DisplayModeController::Mode theMode)
        : myId(std::move(theId)),
          myTitle(std::move(theTitle)),
          myIcon(std::move(theIcon)),
          myDescription(std::move(theDescription)),
          myMode(theMode)
    {
    }

    std::string Id() const override { return myId; }
    std::string Title() const override { return myTitle; }
    std::string Group() const override { return kViewGroup; }
    std::string Section() const override { return "Display"; }
    std::string Icon() const override { return myIcon; }
    std::string Description() const override { return myDescription; }

    bool IsCheckable() const override { return true; }
    bool IsChecked(const CommandContext& theContext) const override
    {
        (void)theContext;
        return DisplayModeController::Instance().Current() == myMode;
    }

    void Execute(CommandContext& theContext) override
    {
        DisplayModeController::Instance().SetMode(theContext, myMode);
        theContext.Redraw();
    }

private:
    std::string                 myId;
    std::string                 myTitle;
    std::string                 myIcon;
    std::string                 myDescription;
    DisplayModeController::Mode myMode;
};

class GridToggleCommand : public Command
{
public:
    std::string Id() const override { return "view.grid_toggle"; }
    std::string Title() const override { return "Grid"; }
    std::string Group() const override { return kViewGroup; }
    std::string Section() const override { return "Show"; }
    std::string Icon() const override { return "🔲"; }
    std::string Shortcut() const override { return "G"; }
    std::string Description() const override { return "Show or hide the ground grid"; }

    bool IsCheckable() const override { return true; }

    bool IsEnabled(const CommandContext& theContext) const override
    {
        return !theContext.View().IsNull();
    }

    bool IsChecked(const CommandContext& theContext) const override
    {
        const Handle(V3d_View) view = theContext.View();
        if (view.IsNull() || view->Viewer().IsNull()) {
            return false;
        }
        return view->Viewer()->IsGridActive();
    }

    void Execute(CommandContext& theContext) override
    {
        const Handle(V3d_View) view = theContext.View();
        if (view.IsNull() || view->Viewer().IsNull()) {
            return;
        }
        const Handle(V3d_Viewer) viewer = view->Viewer();
        if (viewer->IsGridActive()) {
            viewer->DeactivateGrid();
        } else {
            // Type/draw mode chosen to match what OcctViewport activates at
            // startup -- this only ever flips the same grid on and off.
            viewer->ActivateGrid(Aspect_GT_Rectangular, Aspect_GDM_Lines);
        }
        theContext.Redraw();
    }
};

class ProjectionToggleCommand : public Command
{
public:
    std::string Id() const override { return "view.projection_toggle"; }
    std::string Title() const override { return "Perspective"; }
    std::string Group() const override { return kViewGroup; }
    std::string Section() const override { return "Display"; }
    std::string Icon() const override { return "🎥"; }
    std::string Shortcut() const override { return "P"; }
    std::string Description() const override
    {
        return "Toggle between perspective and orthographic projection";
    }

    bool IsCheckable() const override { return true; }

    bool IsEnabled(const CommandContext& theContext) const override
    {
        return !theContext.View().IsNull();
    }

    bool IsChecked(const CommandContext& theContext) const override
    {
        const Handle(V3d_View) view = theContext.View();
        if (view.IsNull() || view->Camera().IsNull()) {
            return false;
        }
        return !view->Camera()->IsOrthographic();
    }

    void Execute(CommandContext& theContext) override
    {
        const Handle(V3d_View) view = theContext.View();
        if (view.IsNull() || view->Camera().IsNull()) {
            return;
        }
        const bool isOrthographic = view->Camera()->IsOrthographic();
        view->Camera()->SetProjectionType(isOrthographic ? Graphic3d_Camera::Projection_Perspective
                                                          : Graphic3d_Camera::Projection_Orthographic);
        theContext.Redraw();
    }
};

class ViewCubeCommand : public Command
{
public:
    std::string Id() const override { return "view.view_cube"; }
    std::string Title() const override { return "View Cube"; }
    std::string Group() const override { return kViewGroup; }
    std::string Section() const override { return "Show"; }
    std::string Icon() const override { return "🎲"; }
    std::string Description() const override
    {
        return "Show or hide the corner navigation cube (click a face, edge or "
               "corner to orient the view)";
    }

    bool IsCheckable() const override { return true; }

    bool IsEnabled(const CommandContext& theContext) const override
    {
        ViewCubeInteraction::Instance().EnsureMaterialized(theContext);
        return theContext.viewport != nullptr;
    }

    bool IsChecked(const CommandContext& theContext) const override
    {
        ViewCubeInteraction::Instance().EnsureMaterialized(theContext);
        return ViewCubeInteraction::Instance().IsVisible();
    }

    void Execute(CommandContext& theContext) override
    {
        ViewCubeInteraction& cube = ViewCubeInteraction::Instance();
        cube.SetVisible(theContext, !cube.IsVisible());
        theContext.Redraw();
    }
};

} // namespace

void RegisterViewCommands(CommandRegistry& theRegistry)
{
    theRegistry.Add(std::make_unique<StandardViewCommand>(
        "view.front", "Front", "⬆️", "Ctrl+1", "Look straight at the front (XZ) of the model",
        V3d_TypeOfOrientation_Zup_Front));
    theRegistry.Add(std::make_unique<StandardViewCommand>(
        "view.back", "Back", "⬇️", "Ctrl+2", "Look straight at the back of the model",
        V3d_TypeOfOrientation_Zup_Back));
    theRegistry.Add(std::make_unique<StandardViewCommand>(
        "view.left", "Left", "⬅️", "Ctrl+3", "Look straight at the left side of the model",
        V3d_TypeOfOrientation_Zup_Left));
    theRegistry.Add(std::make_unique<StandardViewCommand>(
        "view.right", "Right", "➡️", "Ctrl+4", "Look straight at the right side (YZ) of the model",
        V3d_TypeOfOrientation_Zup_Right));
    theRegistry.Add(std::make_unique<StandardViewCommand>(
        "view.top", "Top", "🔼", "Ctrl+5", "Look straight down at the top (XY) of the model",
        V3d_TypeOfOrientation_Zup_Top));
    theRegistry.Add(std::make_unique<StandardViewCommand>(
        "view.bottom", "Bottom", "🔽", "Ctrl+6", "Look straight up at the bottom of the model",
        V3d_TypeOfOrientation_Zup_Bottom));
    theRegistry.Add(std::make_unique<StandardViewCommand>(
        "view.isometric", "Isometric", "🏠", "Home",
        "Return to the default home/isometric view", V3d_TypeOfOrientation_Zup_AxoRight));

    theRegistry.Add(std::make_unique<FitAllCommand>());

    theRegistry.Add(std::make_unique<DisplayModeCommand>(
        "view.display_shaded", "Shaded", "🧱", "Solid shaded display with no face boundaries",
        DisplayModeController::Mode::Shaded));
    theRegistry.Add(std::make_unique<DisplayModeCommand>(
        "view.display_wireframe", "Wireframe", "🕸", "Edges only, no shaded surfaces",
        DisplayModeController::Mode::Wireframe));
    theRegistry.Add(std::make_unique<DisplayModeCommand>(
        "view.display_shaded_edges", "Shaded + Edges", "🔷",
        "Shaded surfaces with face boundaries drawn on top",
        DisplayModeController::Mode::ShadedWithEdges));

    theRegistry.Add(std::make_unique<GridToggleCommand>());
    theRegistry.Add(std::make_unique<ProjectionToggleCommand>());
    theRegistry.Add(std::make_unique<ViewCubeCommand>());

    // Lives in its own file but belongs on the same tab.
    RegisterUnitsCommand(theRegistry);
}

} // namespace lcad
