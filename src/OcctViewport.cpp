#include "OcctViewport.h"

#include <AIS_Line.hxx>
#include <AIS_RubberBand.hxx>
#include <Aspect_DisplayConnection.hxx>
#include <Aspect_Grid.hxx>
#include <Font_FontMgr.hxx>
#include <Font_NameOfFont.hxx>
#include <Font_SystemFont.hxx>
#include <Geom_CartesianPoint.hxx>
#include <OpenGl_GraphicDriver.hxx>
#include <gp_Pnt.hxx>
#include <SelectMgr_ViewerSelector.hxx>
#include <V3d_AmbientLight.hxx>
#include <V3d_DirectionalLight.hxx>
#include <Xw_Window.hxx>

#include <algorithm>
#include <fstream>
#include <initializer_list>

#include <QKeyEvent>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QVBoxLayout>
#include <QWheelEvent>

namespace {

// OCCT resolves all 3D text through a handful of generic font names
// ("sans-serif", "monospace", "Courier", "Times-Roman"). None of those
// exist on a modern Linux box, so its Western fallback lookup fails
// outright and "Courier" degrades to OpenSymbol -- a symbol font. The
// result is unreadable or missing view-cube labels, dimensions and
// annotations. Point those names at real files and all of it works.
void EnsureTextFontAvailable()
{
    Handle(Font_FontMgr) manager = Font_FontMgr::GetInstance();
    if (manager.IsNull()) {
        return;
    }

    // First existing file wins, so this survives a distro that ships only
    // one of these families.
    auto firstExisting = [](std::initializer_list<const char*> theCandidates) -> const char* {
        for (const char* path : theCandidates) {
            if (std::ifstream(path).good()) {
                return path;
            }
        }
        return nullptr;
    };

    const char* sans = firstExisting({
        "/usr/share/fonts/liberation-sans-fonts/LiberationSans-Regular.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
        "/usr/share/fonts/google-carlito-fonts/Carlito-Regular.ttf",
        "/usr/share/fonts/adwaita-sans-fonts/AdwaitaSans-Regular.ttf",
    });
    const char* mono = firstExisting({
        "/usr/share/fonts/liberation-mono-fonts/LiberationMono-Regular.ttf",
        "/usr/share/fonts/dejavu-sans-mono-fonts/DejaVuSansMono.ttf",
        "/usr/share/fonts/adwaita-mono-fonts/AdwaitaMono-Regular.ttf",
    });
    const char* serif = firstExisting({
        "/usr/share/fonts/liberation-serif-fonts/LiberationSerif-Regular.ttf",
        "/usr/share/fonts/dejavu-serif-fonts/DejaVuSerif.ttf",
        "/usr/share/fonts/google-crosextra-caladea-fonts/Caladea-Regular.ttf",
    });

    if (sans == nullptr) {
        return;   // nothing usable installed; OCCT's own warning still applies
    }
    if (mono == nullptr) {
        mono = sans;
    }
    if (serif == nullptr) {
        serif = sans;
    }

    auto registerAlias = [&manager](const char* theAlias, const char* thePath) {
        Handle(Font_SystemFont) font = new Font_SystemFont(TCollection_AsciiString(theAlias));
        font->SetFontPath(Font_FontAspect_Regular, TCollection_AsciiString(thePath));
        manager->RegisterFont(font, Standard_True);
    };

    registerAlias(Font_NOF_SANS_SERIF, sans);
    registerAlias(Font_NOF_MONOSPACE, mono);
    registerAlias(Font_NOF_SERIF, serif);
    registerAlias(Font_NOF_ASCII_MONO, mono);      // "Courier"
    registerAlias("Times-Roman", serif);
    registerAlias("Arial", sans);
}

Aspect_VKeyMouse ToAspectMouseButton(Qt::MouseButton button)
{
    switch (button) {
        case Qt::LeftButton:   return Aspect_VKeyMouse_LeftButton;
        case Qt::RightButton:  return Aspect_VKeyMouse_RightButton;
        case Qt::MiddleButton: return Aspect_VKeyMouse_MiddleButton;
        default:                return Aspect_VKeyMouse_NONE;
    }
}

Aspect_VKeyMouse ToAspectMouseButtons(Qt::MouseButtons buttons)
{
    Aspect_VKeyMouse result = Aspect_VKeyMouse_NONE;
    if (buttons & Qt::LeftButton)   result |= Aspect_VKeyMouse_LeftButton;
    if (buttons & Qt::RightButton)  result |= Aspect_VKeyMouse_RightButton;
    if (buttons & Qt::MiddleButton) result |= Aspect_VKeyMouse_MiddleButton;
    return result;
}

Aspect_VKeyFlags ToAspectFlags(Qt::KeyboardModifiers modifiers)
{
    Aspect_VKeyFlags result = Aspect_VKeyFlags_NONE;
    if (modifiers & Qt::ShiftModifier)   result |= Aspect_VKeyFlags_SHIFT;
    if (modifiers & Qt::ControlModifier) result |= Aspect_VKeyFlags_CTRL;
    if (modifiers & Qt::AltModifier)     result |= Aspect_VKeyFlags_ALT;
    return result;
}

} // namespace

// ---- OcctNativeWindow ----

OcctNativeWindow::OcctNativeWindow(QWindow* parent)
    : QWindow(parent)
{
    // OCCT manages its own GLX context on top of this window; we just
    // need Qt/xcb to give it a plain native window with an OpenGL-capable
    // visual to draw into. Depth/stencil buffers must be requested here,
    // before the native window is created, or OCCT gets a visual it
    // can't properly z-buffer or render into.
    QSurfaceFormat format;
    format.setDepthBufferSize(24);
    format.setStencilBufferSize(8);
    format.setRenderableType(QSurfaceFormat::OpenGL);
    setFormat(format);

    setSurfaceType(QWindow::OpenGLSurface);
}

void OcctNativeWindow::initializeOcctViewer()
{
    if (m_initialized) {
        return;
    }

    EnsureTextFontAvailable();

    Handle(Aspect_DisplayConnection) displayConnection = new Aspect_DisplayConnection();
    Handle(OpenGl_GraphicDriver) graphicDriver = new OpenGl_GraphicDriver(displayConnection, false);

    m_viewer = new V3d_Viewer(graphicDriver);

    // Shading a box needs a light that is NOT coaxial with the camera.
    // SetDefaultLights() installs a HEADLIGHT, pointing straight down the
    // view direction, and in an isometric view the three visible faces of
    // a cube make EQUAL angles with that direction. All three then come
    // back at the same brightness and a box renders as a flat orange
    // hexagon with no edge between its faces. (A sphere still shaded
    // correctly, which is what narrowed this to the symmetry rather than
    // to the lighting being off.)
    //
    // Offsetting the key light up and to the left breaks the tie, the way
    // every CAD package's default studio light does. It stays a headlight
    // so it travels with the camera and the shading does not swim about
    // as the model is orbited; the ambient term keeps the faces turned
    // away from it off black.
    Handle(V3d_DirectionalLight) keyLight =
        new V3d_DirectionalLight(gp_Dir(-0.45, -0.60, -1.0), Quantity_NOC_WHITE, Standard_True);
    m_viewer->AddLight(keyLight);

    // Intensities left at OCCT's defaults deliberately. Dropping the key
    // to 0.85 and pinning ambient to 0.55 was tried and made every face
    // darker, not softer -- the stock ambient is already stronger than
    // that -- so the defaults are what the three shades below were
    // measured from.
    Handle(V3d_AmbientLight) fillLight = new V3d_AmbientLight(Quantity_NOC_WHITE);
    m_viewer->AddLight(fillLight);

    m_viewer->SetLightOn();

    m_context = new AIS_InteractiveContext(m_viewer);

    m_view = m_viewer->CreateView();

    Handle(Xw_Window) window = new Xw_Window(displayConnection, (Aspect_Drawable)winId());
    m_view->SetWindow(window);
    if (!window->IsMapped()) {
        window->Map();
    }

    m_view->SetBackgroundColor(Quantity_NOC_GRAY30);
    m_view->TriedronDisplay(Aspect_TOTP_LEFT_LOWER, Quantity_NOC_WHITE, 0.08);

    // Z-up world, like Fusion and every mechanical CAD package. OCCT's
    // stock camera is Y-up, which also drags the construction grid onto
    // the XZ plane -- so the ground plane ends up vertical and sketches
    // on "XY" read as walls. Pin both the privileged plane and the camera
    // to Z-up before anything is drawn.
    m_viewer->SetPrivilegedPlane(gp_Ax3(gp_Pnt(0.0, 0.0, 0.0),
                                         gp_Dir(0.0, 0.0, 1.0),
                                         gp_Dir(1.0, 0.0, 0.0)));
    m_view->SetProj(V3d_XposYnegZpos);
    m_view->SetUp(0.0, 0.0, 1.0);

    // Frame roughly the working area rather than OCCT's very wide default,
    // which left the grid and the sketch-plane pickers as a speck in the
    // middle of the screen.
    m_view->SetSize(260.0);

    m_view->MustBeResized();

    // Fusion-style ground grid on the XY plane: a 10 mm step with a
    // brighter line every 10th one. The extent is deliberately modest --
    // a grid large enough to fill the screen at every zoom level collapses
    // into moire noise, which reads as a dirty viewport rather than a
    // reference.
    m_viewer->SetRectangularGridValues(0.0, 0.0, 10.0, 10.0, 0.0);
    m_viewer->SetRectangularGridGraphicValues(150.0, 150.0, 0.0);
    m_viewer->ActivateGrid(Aspect_GT_Rectangular, Aspect_GDM_Lines);
    // Minor lines barely above the background; every tenth line clearly
    // brighter, so scale is readable at a glance without the grid
    // competing with the model.
    m_viewer->Grid()->SetColors(Quantity_Color(0.29, 0.29, 0.29, Quantity_TOC_RGB),
                                 Quantity_Color(0.46, 0.46, 0.46, Quantity_TOC_RGB));

    // Origin axis lines, Fusion-style: red = X, green = Y, blue = Z,
    // running the length of the grid in both directions. Decorative only
    // -- displayed with selection mode -1 so they never get in the way of
    // picking real geometry.
    //
    // That -1 is load-bearing, and was missing: the two-argument Display
    // overload activates the DEFAULT selection mode, so the axes were
    // pickable and, running through the world origin, sat between the
    // cursor and any body built there. Clicking a small part near the
    // origin selected an axis instead of the face under the pointer.
    // Just past the grid's edge: long enough to read as "these are the
    // world axes", short enough not to trail off to the horizon.
    const Standard_Real kAxisLength = 180.0;
    auto displayAxis = [this, kAxisLength](const gp_Pnt& theDir, const Quantity_Color& theColor) {
        Handle(Geom_CartesianPoint) start = new Geom_CartesianPoint(-kAxisLength * theDir.X(),
                                                                     -kAxisLength * theDir.Y(),
                                                                     -kAxisLength * theDir.Z());
        Handle(Geom_CartesianPoint) end = new Geom_CartesianPoint(kAxisLength * theDir.X(),
                                                                   kAxisLength * theDir.Y(),
                                                                   kAxisLength * theDir.Z());
        Handle(AIS_Line) axis = new AIS_Line(start, end);
        axis->SetColor(theColor);
        axis->SetWidth(1.5);
        m_context->Display(axis, AIS_WireFrame, -1, Standard_False);
    };

    displayAxis(gp_Pnt(1, 0, 0), Quantity_Color(0.85, 0.20, 0.20, Quantity_TOC_RGB)); // X, red
    displayAxis(gp_Pnt(0, 1, 0), Quantity_Color(0.25, 0.80, 0.25, Quantity_TOC_RGB)); // Y, green
    displayAxis(gp_Pnt(0, 0, 1), Quantity_Color(0.25, 0.45, 0.95, Quantity_TOC_RGB)); // Z, blue

    // Fusion-360-style bindings: left click/drag selects (with a
    // rubber-band box for multi-select), right-drag orbits the camera,
    // middle-drag pans. Scroll-to-zoom is hand-rolled in wheelEvent.
    // Ctrl+Left is bound too so a Ctrl-held drag still starts the select
    // gesture instead of silently doing nothing.
    AIS_MouseGestureMap& gestures = ChangeMouseGestureMap();
    gestures.Clear();
    gestures.Bind(Aspect_VKeyMouse_LeftButton, AIS_MouseGesture_SelectRectangle);
    gestures.Bind(Aspect_VKeyMouse_LeftButton | Aspect_VKeyFlags_CTRL, AIS_MouseGesture_SelectRectangle);
    gestures.Bind(Aspect_VKeyMouse_RightButton, AIS_MouseGesture_RotateOrbit);
    gestures.Bind(Aspect_VKeyMouse_MiddleButton, AIS_MouseGesture_Pan);

    // Plain left click/drag replaces the selection; Ctrl+left click/drag
    // toggles objects in/out of it instead, like Fusion's multi-select.
    AIS_MouseSelectionSchemeMap& schemes = ChangeMouseSelectionSchemes();
    schemes.Clear();
    schemes.Bind(Aspect_VKeyMouse_LeftButton, AIS_SelectionScheme_Replace);
    schemes.Bind(Aspect_VKeyMouse_LeftButton | Aspect_VKeyFlags_CTRL, AIS_SelectionScheme_XOR);

    m_initialized = true;
}

void OcctNativeWindow::FitAll()
{
    if (m_view.IsNull()) {
        return;
    }
    m_view->FitAll();
    m_view->ZFitAll();
    m_view->Redraw();
}

Graphic3d_Vec2i OcctNativeWindow::toDevicePixels(const QPointF& point) const
{
    const qreal dpr = devicePixelRatio();
    return Graphic3d_Vec2i(static_cast<int>(point.x() * dpr), static_cast<int>(point.y() * dpr));
}

void OcctNativeWindow::updateView()
{
    if (m_context.IsNull() || m_view.IsNull()) {
        return;
    }
    FlushViewEvents(m_context, m_view, Standard_True);
}

void OcctNativeWindow::exposeEvent(QExposeEvent*)
{
    if (!isExposed()) {
        return;
    }
    if (!m_initialized) {
        initializeOcctViewer();
    }
    if (!m_view.IsNull()) {
        m_view->Redraw();
    }
}

void OcctNativeWindow::resizeEvent(QResizeEvent*)
{
    if (m_initialized && !m_view.IsNull()) {
        m_view->MustBeResized();
    }
}

void OcctNativeWindow::PushInteraction(lcad::ViewportInteraction* theInteraction)
{
    if (theInteraction != nullptr) {
        m_interactions.push_back(theInteraction);
    }
}

void OcctNativeWindow::PopInteraction()
{
    if (m_interactions.empty()) {
        return;
    }
    lcad::ViewportInteraction* removed = m_interactions.back();
    m_interactions.pop_back();
    removed->OnDeactivated();
}

lcad::ViewportInteraction* OcctNativeWindow::CurrentInteraction() const
{
    return m_interactions.empty() ? nullptr : m_interactions.back();
}

lcad::ViewportInteraction* OcctNativeWindow::ExclusiveInteraction() const
{
    for (auto it = m_interactions.rbegin(); it != m_interactions.rend(); ++it) {
        if (*it != nullptr && (*it)->IsExclusive()) {
            return *it;
        }
    }
    return nullptr;
}

bool OcctNativeWindow::dispatchToInteractions(
    const std::function<bool(lcad::ViewportInteraction*)>& theDeliver)
{
    // Snapshotted because a handler may pop itself while being called --
    // Escape in a sketch tool does exactly that -- which would leave the
    // loop walking a vector that moved under it.
    const std::vector<lcad::ViewportInteraction*> stack = m_interactions;
    for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
        if (*it == nullptr || !theDeliver(*it)) {
            continue;
        }
        if (!m_view.IsNull()) {
            m_view->Redraw();
        }
        return true;
    }
    return false;
}

void OcctNativeWindow::mousePressEvent(QMouseEvent* event)
{
    const Graphic3d_Vec2i pos = toDevicePixels(event->position());

    if (dispatchToInteractions([&](lcad::ViewportInteraction* theHandler) {
            return theHandler->OnMousePress(pos, event->button(), event->modifiers());
        })) {
        return;
    }

    if (event->button() == Qt::LeftButton) {
        m_selectionStartX = pos.x();
        m_lastMoveX = m_selectionStartX;
    }

    // Detect what is under the press BEFORE handing the click on.
    //
    // OCCT selects whatever the last MoveTo detected, and a press is not a
    // move: a click that arrives without the cursor having travelled there
    // first -- which is every click from the input-script harness, and any
    // click that lands in the same frame as the move -- had nothing
    // detected and so selected nothing. For a real cursor this is
    // redundant work at the position it already sits on.
    if (!m_context.IsNull() && !m_view.IsNull() && event->button() == Qt::LeftButton) {
        m_context->MoveTo(pos.x(), pos.y(), m_view, Standard_False);
    }

    PressMouseButton(pos,
                      ToAspectMouseButton(event->button()),
                      ToAspectFlags(event->modifiers()),
                      false);
    updateView();
}

void OcctNativeWindow::mouseReleaseEvent(QMouseEvent* event)
{
    const Graphic3d_Vec2i pos = toDevicePixels(event->position());

    if (dispatchToInteractions([&](lcad::ViewportInteraction* theHandler) {
            return theHandler->OnMouseRelease(pos, event->button(), event->modifiers());
        })) {
        return;
    }

    if (event->button() == Qt::LeftButton) {
        m_lastMoveX = pos.x();
    }

    ReleaseMouseButton(pos,
                        ToAspectMouseButton(event->button()),
                        ToAspectFlags(event->modifiers()),
                        false);
    updateView();
}

void OcctNativeWindow::OnSelectionChanged(const Handle(AIS_InteractiveContext)& theCtx,
                                          const Handle(V3d_View)& theView)
{
    AIS_ViewController::OnSelectionChanged(theCtx, theView);
    if (m_onSelectionChanged) {
        m_onSelectionChanged();
    }
}

void OcctNativeWindow::mouseMoveEvent(QMouseEvent* event)
{
    const Graphic3d_Vec2i pos = toDevicePixels(event->position());

    if (dispatchToInteractions([&](lcad::ViewportInteraction* theHandler) {
            return theHandler->OnMouseMove(pos, event->buttons(), event->modifiers());
        })) {
        return;
    }

    m_lastMoveX = pos.x();

    if (event->buttons() & Qt::LeftButton) {
        updateRubberBandStyle();
    }

    UpdateMousePosition(pos,
                         ToAspectMouseButtons(event->buttons()),
                         ToAspectFlags(event->modifiers()),
                         false);
    updateView();
}

void OcctNativeWindow::mouseDoubleClickEvent(QMouseEvent* event)
{
    const Graphic3d_Vec2i pos = toDevicePixels(event->position());

    if (dispatchToInteractions([&](lcad::ViewportInteraction* theHandler) {
            return theHandler->OnMouseDoubleClick(pos, event->button(), event->modifiers());
        })) {
        return;
    }

    QWindow::mouseDoubleClickEvent(event);
}

void OcctNativeWindow::keyPressEvent(QKeyEvent* event)
{
    if (dispatchToInteractions([&](lcad::ViewportInteraction* theHandler) {
            return theHandler->OnKeyPress(event->key(), event->modifiers());
        })) {
        return;
    }

    QWindow::keyPressEvent(event);
}

void OcctNativeWindow::updateRubberBandStyle()
{
    if (myRubberBand.IsNull()) {
        return;
    }

    // Fusion-style visual cue matching the window-vs-crossing behavior in
    // SelectInViewer(): solid blue while dragging left-to-right (window
    // select), dashed green while dragging right-to-left (crossing
    // select).
    const bool draggedRight = m_lastMoveX >= m_selectionStartX;
    if (draggedRight) {
        const Quantity_Color blue(0.20, 0.55, 1.0, Quantity_TOC_RGB);
        myRubberBand->SetLineType(Aspect_TOL_SOLID);
        myRubberBand->SetLineColor(blue);
        myRubberBand->SetFilling(blue, 0.85);
    } else {
        const Quantity_Color green(0.25, 0.85, 0.35, Quantity_TOC_RGB);
        myRubberBand->SetLineType(Aspect_TOL_DASH);
        myRubberBand->SetLineColor(green);
        myRubberBand->SetFilling(green, 0.85);
    }
}

void OcctNativeWindow::SelectInViewer(const NCollection_Sequence<Graphic3d_Vec2i>& thePnts,
                                       const AIS_SelectionScheme theScheme)
{
    if (thePnts.IsEmpty() || m_context.IsNull() || m_view.IsNull()) {
        AIS_ViewController::SelectInViewer(thePnts, theScheme);
        return;
    }

    Graphic3d_Vec2i pMin = thePnts.First();
    Graphic3d_Vec2i pMax = thePnts.First();
    for (NCollection_Sequence<Graphic3d_Vec2i>::Iterator it(thePnts); it.More(); it.Next()) {
        const Graphic3d_Vec2i& p = it.Value();
        pMin.x() = std::min(pMin.x(), p.x());
        pMin.y() = std::min(pMin.y(), p.y());
        pMax.x() = std::max(pMax.x(), p.x());
        pMax.y() = std::max(pMax.y(), p.y());
    }

    // A plain CLICK arrives here as a one-point "rectangle", and
    // SelectRectangle on a zero-area box matches nothing -- with window
    // semantics, which require full enclosure, it can never match
    // anything. Left-click selection was silently dead because of it.
    // A point pick goes through OCCT's own detect-then-select path.
    //
    // The slop is there because a click with a pixel or two of jitter is
    // still a click, not a drag of a two-pixel box.
    constexpr Standard_Integer kClickSlopPixels = 2;
    if (pMax.x() - pMin.x() <= kClickSlopPixels && pMax.y() - pMin.y() <= kClickSlopPixels) {
        m_context->MoveTo(pMax.x(), pMax.y(), m_view, Standard_False);
        m_context->SelectDetected(theScheme);
        m_view->Redraw();
        return;
    }

    // Fusion-style direction-sensitive box select: dragging left-to-right
    // is a "window" box (only fully-enclosed objects match); dragging
    // right-to-left is a "crossing" box (anything merely touched matches
    // too).
    const bool draggedRight = m_lastMoveX >= m_selectionStartX;
    m_context->MainSelector()->AllowOverlapDetection(!draggedRight);

    m_context->SelectRectangle(pMin, pMax, m_view, theScheme);
    m_view->Redraw();
}

void OcctNativeWindow::wheelEvent(QWheelEvent* event)
{
    if (m_view.IsNull()) {
        event->accept();
        return;
    }

    // Hand-rolled instead of routed through AIS_ViewController's built-in
    // scroll handling: that path gave no control over sensitivity (way
    // too strong a zoom per notch) and didn't anchor to the cursor.
    // V3d_View::StartZoomAtPoint/ZoomAtPoint takes a synthetic pixel drag
    // distance, which we get to scale ourselves, and keeps the zoom
    // centered on wherever the mouse is -- like Fusion.
    //
    // Prefer pixelDelta() when available (trackpads / high-res mice
    // report fine-grained continuous values there) for a smooth feel;
    // fall back to angleDelta() (~120 units per notch on a normal mouse
    // wheel) scaled down to a gentle per-notch step.
    double rawDelta = 0.0;
    if (!event->pixelDelta().isNull()) {
        constexpr double kPixelSensitivity = 0.6;
        rawDelta = event->pixelDelta().y() * kPixelSensitivity;
    } else {
        constexpr double kNotchToPixels = 0.22;
        rawDelta = event->angleDelta().y() * kNotchToPixels;
    }

    const Graphic3d_Vec2i pos = toDevicePixels(event->position());
    const int dragDistance = static_cast<int>(rawDelta * devicePixelRatio());
    if (dragDistance == 0) {
        event->accept();
        return;
    }

    m_view->StartZoomAtPoint(pos.x(), pos.y());
    m_view->ZoomAtPoint(pos.x(), pos.y(), pos.x(), pos.y() - dragDistance);
    m_view->Redraw();

    event->accept();
}

// ---- OcctViewport ----

OcctViewport::OcctViewport(QWidget* parent)
    : QWidget(parent)
{
    m_window = new OcctNativeWindow();
    QWidget* container = QWidget::createWindowContainer(m_window, this);

    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(container);
}
