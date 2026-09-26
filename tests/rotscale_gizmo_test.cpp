// The rotate and scale manipulators, driven the way a mouse drives them:
// pixels in, a grabbed handle and an angle or a factor out.
//
// The screen is the app's own home view (V3d_XposYnegZpos, orthographic)
// stood up as a bare Graphic3d_Camera on a viewport the size of the real
// one -- which is all V3d_View itself uses to turn a pixel into a ray --
// so every drag below runs through exactly the code RotateScaleGizmoTool
// runs, with no window.
//
// Nothing is read back out of the implementation to be compared with
// itself: where a drag should end up is worked out here by projecting the
// point that SHOULD be under the cursor, and the claim tested is the one a
// user feels -- the bit of ring or handle they grabbed stays under the
// hand, at every zoom.
#include "gizmos/RotateScaleGizmo.h"

#include <Graphic3d_Camera.hxx>
#include <gp.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

using namespace lcad;

static int failures = 0;

static void check(bool theOk, const std::string& theWhat)
{
    std::cout << (theOk ? "  PASS  " : "  FAIL  ") << theWhat << std::endl;
    if (!theOk) {
        ++failures;
    }
}

static void checkNear(double theValue, double theExpected, double theTolerance,
                      const std::string& theWhat)
{
    const bool ok = std::fabs(theValue - theExpected) <= theTolerance;
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << theWhat << "  (got " << theValue
              << ", expect " << theExpected << ")" << std::endl;
    if (!ok) {
        ++failures;
    }
}

namespace {

constexpr double kPi = 3.14159265358979323846;

// The real viewport, in device pixels, on the 2x display this was
// measured on: 1071 x 577 logical.
constexpr int    kWidth  = 2142;
constexpr int    kHeight = 1154;
constexpr double kRatio  = 2.0;

// A 20 mm box at the origin: its centroid is the pivot.
const gp_Pnt kPivot(10.0, 10.0, 10.0);

// theViewHeight is how many millimetres the viewport shows top to bottom
// -- the zoom. The eye sits up the (1,-1,1) diagonal, as V3d_XposYnegZpos
// puts it.
Handle(Graphic3d_Camera) HomeCamera(double theViewHeight, bool thePerspective = false)
{
    Handle(Graphic3d_Camera) camera = new Graphic3d_Camera();
    camera->SetProjectionType(thePerspective ? Graphic3d_Camera::Projection_Perspective
                                             : Graphic3d_Camera::Projection_Orthographic);
    camera->SetEyeAndCenter(gp_Pnt(10.0 + 300.0, 10.0 - 300.0, 10.0 + 300.0), kPivot);
    camera->SetUp(gp::DZ());
    camera->OrthogonalizeUp();
    camera->SetAspect(static_cast<double>(kWidth) / kHeight);
    camera->SetZRange(1.0, 5000.0);
    if (thePerspective) {
        camera->SetFOVy(45.0);
        // Pull the eye back until the view shows theViewHeight at the pivot.
        const double distance = 0.5 * theViewHeight / std::tan(0.5 * 45.0 * kPi / 180.0);
        camera->SetDistance(distance);
    } else {
        camera->SetScale(theViewHeight);
    }
    return camera;
}

// A world point at theAngle round the ring about axis theIndex, in the
// ring's own frame -- the frame RingDrag reads its angle in.
gp_Pnt RingPoint(int theIndex, double theRadius, double theAngleDegrees)
{
    const gp_Ax2 frame = RotationFrame(GizmoAxis(theIndex, kPivot));
    const double angle = theAngleDegrees * kPi / 180.0;
    const gp_Vec spoke = gp_Vec(frame.XDirection()) * (theRadius * std::cos(angle))
                       + gp_Vec(frame.YDirection()) * (theRadius * std::sin(angle));
    return kPivot.Translated(spoke);
}

bool RayAt(const GizmoScreen& theScreen, const gp_Pnt& thePoint, gp_Lin& theRay,
           double& theX, double& theY)
{
    return theScreen.Project(thePoint, theX, theY) && theScreen.RayThrough(theX, theY, theRay);
}

// How far a pixel is from the nearest drawn point of ring theIndex.
double PixelsFromRing(const GizmoScreen& theScreen, int theIndex, double theRadius,
                      double theX, double theY)
{
    double best = 1.0e300;
    for (const gp_Pnt& point : SampleRing(GizmoAxis(theIndex, kPivot), theRadius, 720)) {
        double x = 0.0, y = 0.0;
        if (theScreen.Project(point, x, y)) {
            best = std::min(best, std::hypot(x - theX, y - theY));
        }
    }
    return best;
}

} // namespace

// ---- 1. the screen: pixels and rays agree with each other ----
static void TestScreenRoundTrip()
{
    std::cout << "\n[1] a pixel's ray projects back onto that pixel" << std::endl;
    const GizmoScreen screen(HomeCamera(200.0), kWidth, kHeight);
    check(screen.IsValid(), "the home view makes a valid screen");
    check(!GizmoScreen().IsValid(), "a screen with no camera is refused, not divided by");

    double x = 0.0, y = 0.0;
    check(screen.Project(kPivot, x, y), "the pivot projects");
    // The camera looks straight at the pivot, so it lands dead centre --
    // V3d_View::Convert's convention: y down, the last row is height - 1.
    checkNear(x, kWidth * 0.5, 1.0e-6, "pivot x is the middle column");
    checkNear(y, kHeight - 1 - kHeight * 0.5, 1.0e-6, "pivot y is the middle row");

    gp_Lin ray;
    check(screen.RayThrough(700.25, 300.5, ray), "a ray through a pixel");
    const gp_Pnt far = ray.Location().Translated(gp_Vec(ray.Direction()) * 250.0);
    double fx = 0.0, fy = 0.0;
    screen.Project(far, fx, fy);
    checkNear(fx, 700.25, 1.0e-6, "a point far down that ray is still on that pixel (x)");
    checkNear(fy, 300.5, 1.0e-6, "... (y)");
}

// ---- 2. a fixed size on screen at every zoom ----
static void TestScreenSize()
{
    std::cout << "\n[2] a ring of N pixels is N pixels at every zoom" << std::endl;
    // Fusion's manipulators hold their size while the wheel turns. The
    // ring is drawn zoom-persistent at RotateRingRadiusPixels() and hit-
    // tested at that many pixels times ModelPerPixel -- so the radius the
    // hit test uses must project to exactly that many pixels, zoomed in or
    // out, or the user grabs at a ring that is not where it is drawn.
    const double pixels = RotateRingRadiusPixels() * kRatio;
    check(RotateRingRadiusPixels() >= 60.0 && RotateRingRadiusPixels() <= 150.0,
          "the ring radius is a comfortable 60..150 logical pixels");
    check(ScaleHandleLengthPixels() >= 60.0 && ScaleHandleLengthPixels() <= 150.0,
          "the scale handle is a comfortable 60..150 logical pixels long");

    for (const bool perspective : {false, true}) {
        for (const double viewHeight : {40.0, 200.0, 2000.0}) {
            const GizmoScreen screen(HomeCamera(viewHeight, perspective), kWidth, kHeight);
            const double perPixel = screen.ModelPerPixel(kPivot);
            const std::string zoom = std::string(perspective ? "perspective" : "orthographic")
                                   + ", " + std::to_string(static_cast<int>(viewHeight))
                                   + " mm tall: ";
            if (!perspective) {
                checkNear(perPixel, viewHeight / kHeight, 1.0e-12,
                          zoom + "a pixel is the view height over the pixel height");
            }

            // The Z ring lies flat, and its (1,1,0) diameter is square to
            // the line of sight and level on screen: at the pivot's depth,
            // so perspective does not stretch it either.
            double cx = 0.0, cy = 0.0, x = 0.0, y = 0.0;
            screen.Project(kPivot, cx, cy);
            const gp_Vec level = gp_Vec(1.0, 1.0, 0.0).Normalized() * (pixels * perPixel);
            screen.Project(kPivot.Translated(level), x, y);
            checkNear(x - cx, pixels, 1.0e-6, zoom + "the ring's radius on screen, device px");
            checkNear(y - cy, 0.0, 1.0e-6, zoom + "... measured level, so it is all radius");

            if (!perspective) {
                // Orthographic: no point of the ring reaches further out.
                double widest = 0.0;
                for (const gp_Pnt& point :
                     SampleRing(GizmoAxis(2, kPivot), pixels * perPixel, 3600)) {
                    screen.Project(point, x, y);
                    widest = std::max(widest, std::fabs(x - cx));
                }
                checkNear(widest, pixels, 0.05, zoom + "and it is the widest the ring gets");
            }
        }
    }
}

// ---- 3. which ring is under the cursor ----
static void TestPickRing()
{
    std::cout << "\n[3] a press on a ring grabs that ring and no other" << std::endl;
    const double pick = GizmoPickRadiusPixels() * kRatio;

    for (const double viewHeight : {60.0, 600.0}) {
        const GizmoScreen screen(HomeCamera(viewHeight), kWidth, kHeight);
        const double radius = RotateRingRadiusPixels() * kRatio * screen.ModelPerPixel(kPivot);
        const std::string zoom = std::to_string(static_cast<int>(viewHeight)) + " mm view: ";

        for (int axis = 0; axis < 3; ++axis) {
            // The rings cross each other on screen; press on a stretch of
            // this one well clear of the other two, as a user aiming at it
            // would.
            double bestClearance = 0.0, x = 0.0, y = 0.0;
            for (int degrees = 0; degrees < 360; degrees += 5) {
                double px = 0.0, py = 0.0;
                screen.Project(RingPoint(axis, radius, degrees), px, py);
                double clearance = 1.0e300;
                for (int other = 0; other < 3; ++other) {
                    if (other != axis) {
                        clearance = std::min(clearance,
                                             PixelsFromRing(screen, other, radius, px, py));
                    }
                }
                if (clearance > bestClearance) {
                    bestClearance = clearance;
                    x = px;
                    y = py;
                }
            }
            check(bestClearance > 2.0 * pick, zoom + "ring " + std::to_string(axis)
                                                  + " has a stretch clear of the others");
            check(PickRotateRing(screen, kPivot, radius, x, y, pick) == axis,
                  zoom + "a press ON ring " + std::to_string(axis) + " grabs it");
            check(PickRotateRing(screen, kPivot, radius, x + 0.6 * pick, y, pick) == axis,
                  zoom + "... and still does a little off it");
        }

        double cx = 0.0, cy = 0.0;
        screen.Project(kPivot, cx, cy);
        check(PickRotateRing(screen, kPivot, radius, cx, cy, pick) == -1,
              zoom + "a press on the pivot, inside every ring, grabs nothing");
        const double outside = RotateRingRadiusPixels() * kRatio + 3.0 * pick;
        check(PickRotateRing(screen, kPivot, radius, cx + outside, cy, pick) == -1,
              zoom + "a press well outside the rings grabs nothing");
    }
}

// ---- 4. a ring drag turns the body the way the hand went ----
static void TestRingDrag()
{
    std::cout << "\n[4] dragging a ring: the grabbed point stays under the cursor" << std::endl;
    for (const bool perspective : {false, true}) {
        const GizmoScreen screen(HomeCamera(150.0, perspective), kWidth, kHeight);
        const double radius = RotateRingRadiusPixels() * kRatio * screen.ModelPerPixel(kPivot);
        const std::string view = perspective ? "perspective: " : "orthographic: ";

        for (int axis = 0; axis < 3; ++axis) {
            const std::string name = view + "ring " + std::to_string(axis) + ": ";
            const gp_Pnt grabbed = RingPoint(axis, radius, 45.0);

            gp_Lin press, ray;
            double pressX = 0.0, pressY = 0.0, x = 0.0, y = 0.0;
            RayAt(screen, grabbed, press, pressX, pressY);

            RingDrag drag;
            check(drag.Begin(GizmoAxis(axis, kPivot), press), name + "the drag starts");

            // Out along the ring to 75 degrees, in small steps, the way a
            // mouse reports it.
            for (int degrees = 50; degrees <= 75; degrees += 5) {
                RayAt(screen, RingPoint(axis, radius, degrees), ray, x, y);
                drag.Update(ray);
            }
            checkNear(drag.TotalDegrees(), 30.0, 1.0e-6, name + "a 30 degree sweep reads 30");

            // The claim a user feels: turn the grabbed point by the angle the
            // drag produced, about the axis the feature will turn about, and
            // it lands on the pixel the cursor let go at.
            gp_Trsf turn;
            turn.SetRotation(GizmoAxis(axis, kPivot), drag.TotalRadians());
            double tx = 0.0, ty = 0.0;
            screen.Project(grabbed.Transformed(turn), tx, ty);
            check(std::hypot(tx - x, ty - y) < 1.0e-6,
                  name + "the turned body puts the grabbed point under the cursor");

            // And back the other way, past where it started.
            for (int degrees = 70; degrees >= 15; degrees -= 5) {
                RayAt(screen, RingPoint(axis, radius, degrees), ray, x, y);
                drag.Update(ray);
            }
            checkNear(drag.TotalDegrees(), -30.0, 1.0e-6, name + "back past the start reads -30");
        }
    }

    // A drag round through the frame's +-180 seam must keep counting, not
    // jump by a whole turn.
    const GizmoScreen screen(HomeCamera(150.0), kWidth, kHeight);
    const double radius = RotateRingRadiusPixels() * kRatio * screen.ModelPerPixel(kPivot);
    gp_Lin ray;
    double x = 0.0, y = 0.0;
    RayAt(screen, RingPoint(2, radius, 160.0), ray, x, y);
    RingDrag drag;
    drag.Begin(GizmoAxis(2, kPivot), ray);
    for (int degrees = 170; degrees <= 540; degrees += 10) {
        RayAt(screen, RingPoint(2, radius, degrees), ray, x, y);
        drag.Update(ray);
    }
    checkNear(drag.TotalDegrees(), 380.0, 1.0e-6,
              "more than a whole turn, across the seam, counts every degree");
}

// ---- 5. the scale handle ----
static void TestScaleDrag()
{
    std::cout << "\n[5] dragging the scale handle: the tip follows the cursor" << std::endl;
    const double pick = GizmoPickRadiusPixels() * kRatio;

    for (const double viewHeight : {80.0, 800.0}) {
        const GizmoScreen screen(HomeCamera(viewHeight), kWidth, kHeight);
        const double rest = ScaleHandleLengthPixels() * kRatio * screen.ModelPerPixel(kPivot);
        const gp_Ax1 axis(kPivot, ScaleHandleDirection());
        const gp_Vec along(axis.Direction());
        const std::string zoom = std::to_string(static_cast<int>(viewHeight)) + " mm view: ";

        const auto on = [&](double theShare, double& theX, double& theY) {
            screen.Project(kPivot.Translated(along * (rest * theShare)), theX, theY);
        };

        // Where on the handle a press grabs it.
        double x = 0.0, y = 0.0;
        on(0.8, x, y);
        check(PickScaleHandle(screen, axis, rest, x, y, pick), zoom + "the outer handle grabs");
        on(1.0, x, y);
        check(PickScaleHandle(screen, axis, rest, x, y, pick), zoom + "the cube on the tip grabs");
        on(0.2, x, y);
        check(!PickScaleHandle(screen, axis, rest, x, y, pick),
              zoom + "the inner half, inside the body, is left for picking faces");
        on(0.8, x, y);
        check(!PickScaleHandle(screen, axis, rest, x, y + 3.0 * pick, pick),
              zoom + "well to one side of it grabs nothing");

        // Press 80% of the way out and drag one whole rest length further:
        // the tip goes from 1x to 2x out, so the body doubles.
        gp_Lin press, ray;
        on(0.8, x, y);
        screen.RayThrough(x, y, press);
        ScaleDrag drag;
        check(drag.Begin(axis, rest, press), zoom + "the drag starts");
        for (int step = 1; step <= 10; ++step) {
            on(0.8 + 0.1 * step, x, y);
            screen.RayThrough(x, y, ray);
            drag.Update(ray);
        }
        checkNear(drag.Factor(), 2.0, 1.0e-9, zoom + "one rest length further out is 2x");

        // A sideways wobble on the way does not change the size: half the
        // pick radius straight across the handle as it lies on screen.
        double baseX = 0.0, baseY = 0.0;
        on(0.0, baseX, baseY);
        on(1.8, x, y);
        const double runX = x - baseX, runY = y - baseY;
        const double run = std::hypot(runX, runY);
        screen.RayThrough(x - 0.5 * pick * runY / run, y + 0.5 * pick * runX / run, ray);
        drag.Update(ray);
        checkNear(drag.Factor(), 2.0, 1.0e-9, zoom + "motion across the handle is ignored");

        // Back in to half way to the pivot: 0.5x. Then through the pivot:
        // refused, and the last good factor held rather than a collapsed
        // or inside-out body being previewed and committed.
        on(0.3, x, y);
        screen.RayThrough(x, y, ray);
        check(drag.Update(ray), zoom + "dragging in shrinks");
        checkNear(drag.Factor(), 0.5, 1.0e-9, zoom + "half a rest length in from the start is 0.5x");
        on(-0.5, x, y);
        screen.RayThrough(x, y, ray);
        check(!drag.Update(ray), zoom + "dragging through the pivot is refused");
        checkNear(drag.Factor(), 0.5, 1.0e-12, zoom + "... and the last good factor is held");
    }
}

int main()
{
    TestScreenRoundTrip();
    TestScreenSize();
    TestPickRing();
    TestRingDrag();
    TestScaleDrag();

    std::cout << std::endl
              << (failures == 0 ? "All rotate/scale gizmo tests passed."
                                : std::to_string(failures) + " rotate/scale gizmo test(s) FAILED.")
              << std::endl;
    return failures == 0 ? 0 : 1;
}
