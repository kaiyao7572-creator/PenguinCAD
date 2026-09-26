// Standard views, view cube clicks and Fit: does the camera land where the
// name on the button says, the way Fusion puts it there?
//
// None of this can be told apart by compiling it. A front view that is
// really the back view, a top view with FRONT at the top, a corner click
// that leaves the part lying on its side, or a fit that frames the origin
// axes instead of the part all build cleanly and all look "roughly right"
// in a screenshot of a symmetric box. So the conventions are written down
// here, as numbers.
//
// The world is Fusion's default: Z up, and FRONT is the -Y side (a user
// looking at the front view looks along +Y, with X running to the right).
#include "view/ViewOrientation.h"

#include <AIS_Line.hxx>
#include <AIS_Shape.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <Geom_CartesianPoint.hxx>
#include <Graphic3d_TransformPers.hxx>
#include <V3d.hxx>
#include <gp_Pln.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <cmath>
#include <iostream>
#include <string>

using namespace lcad;

static int failures = 0;

static void check(bool ok, const std::string& what)
{
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << what << std::endl;
    if (!ok) ++failures;
}

static bool Same(const gp_Dir& a, const gp_XYZ& b)
{
    return a.IsEqual(gp_Dir(b), 1.0e-9);
}

static bool Near(double a, double b, double tol = 1.0e-6)
{
    return std::fabs(a - b) <= tol;
}

// What is on the right of the screen.
static gp_Dir ScreenRight(const ViewPose& thePose)
{
    return thePose.direction.Crossed(thePose.up);
}

static void CheckPose(const std::string& theName, const ViewPose& thePose,
                      const gp_XYZ& theDirection, const gp_XYZ& theUp, const gp_XYZ& theRight)
{
    check(Same(thePose.direction, theDirection), theName + ": looks the right way");
    check(Same(thePose.up, theUp), theName + ": the right way is up");
    check(Same(ScreenRight(thePose), theRight), theName + ": the right way is to the right");
    check(Near(gp_Vec(thePose.direction).Dot(gp_Vec(thePose.up)), 0.0),
          theName + ": up is square to the view");
}

int main()
{
    const double r2 = 1.0 / std::sqrt(2.0);

    std::cout << "--- standard views (Ctrl+1..6, Home) ---" << std::endl;
    CheckPose("front", StandardViewPose(V3d_TypeOfOrientation_Zup_Front),
              gp_XYZ(0, 1, 0), gp_XYZ(0, 0, 1), gp_XYZ(1, 0, 0));
    CheckPose("back", StandardViewPose(V3d_TypeOfOrientation_Zup_Back),
              gp_XYZ(0, -1, 0), gp_XYZ(0, 0, 1), gp_XYZ(-1, 0, 0));
    CheckPose("left", StandardViewPose(V3d_TypeOfOrientation_Zup_Left),
              gp_XYZ(1, 0, 0), gp_XYZ(0, 0, 1), gp_XYZ(0, -1, 0));
    CheckPose("right", StandardViewPose(V3d_TypeOfOrientation_Zup_Right),
              gp_XYZ(-1, 0, 0), gp_XYZ(0, 0, 1), gp_XYZ(0, 1, 0));
    // Tipped over the front: FRONT at the bottom looking down, at the top
    // looking up, X to the right both times.
    CheckPose("top", StandardViewPose(V3d_TypeOfOrientation_Zup_Top),
              gp_XYZ(0, 0, -1), gp_XYZ(0, 1, 0), gp_XYZ(1, 0, 0));
    CheckPose("bottom", StandardViewPose(V3d_TypeOfOrientation_Zup_Bottom),
              gp_XYZ(0, 0, 1), gp_XYZ(0, -1, 0), gp_XYZ(1, 0, 0));

    // Home: in from the front-right-top corner, so FRONT, RIGHT and TOP are
    // the three faces on show, upright.
    {
        const ViewPose home = StandardViewPose(V3d_TypeOfOrientation_Zup_AxoRight);
        const gp_Vec towardsEye = -gp_Vec(home.direction);
        check(towardsEye.Dot(gp_Vec(0, -1, 0)) > 0.5, "home: FRONT (-Y) faces the eye");
        check(towardsEye.Dot(gp_Vec(1, 0, 0)) > 0.5, "home: RIGHT (+X) faces the eye");
        check(towardsEye.Dot(gp_Vec(0, 0, 1)) > 0.5, "home: TOP (+Z) faces the eye");
        check(Near(towardsEye.X(), -towardsEye.Y()) && Near(towardsEye.X(), towardsEye.Z()),
              "home: exactly isometric");
        check(Same(home.up, gp_XYZ(-1, 1, 2)), "home: Z is up on screen");
    }

    std::cout << "--- view cube clicks from the home view ---" << std::endl;
    const ViewPose home = StandardViewPose(V3d_TypeOfOrientation_Zup_AxoRight);
    CheckPose("cube FRONT", CubeClickPose(V3d_TypeOfOrientation_Zup_Front, home),
              gp_XYZ(0, 1, 0), gp_XYZ(0, 0, 1), gp_XYZ(1, 0, 0));
    CheckPose("cube RIGHT", CubeClickPose(V3d_TypeOfOrientation_Zup_Right, home),
              gp_XYZ(-1, 0, 0), gp_XYZ(0, 0, 1), gp_XYZ(0, 1, 0));
    // The swing from the home corner lands exactly between FRONT and RIGHT;
    // the tie must go to Fusion's own top view, not to whichever side the
    // rounding favours.
    CheckPose("cube TOP", CubeClickPose(V3d_TypeOfOrientation_Zup_Top, home),
              gp_XYZ(0, 0, -1), gp_XYZ(0, 1, 0), gp_XYZ(1, 0, 0));
    CheckPose("cube front-right edge", CubeClickPose(V3d_XposYneg, home),
              gp_XYZ(-r2, r2, 0), gp_XYZ(0, 0, 1), gp_XYZ(r2, r2, 0));
    CheckPose("cube top-front edge", CubeClickPose(V3d_YnegZpos, home),
              gp_XYZ(0, r2, -r2), gp_XYZ(0, r2, r2), gp_XYZ(1, 0, 0));
    CheckPose("cube front-right-top corner", CubeClickPose(V3d_XposYnegZpos, home),
              home.direction.XYZ(), home.up.XYZ(), ScreenRight(home).XYZ());
    // A far corner of the cube reached by orbiting: still upright.
    {
        const ViewPose corner = CubeClickPose(V3d_XnegYposZneg, home);
        check(Same(corner.direction, gp_XYZ(1, -1, 1)), "cube back-left-bottom corner: looks up at it");
        check(Same(corner.up, gp_XYZ(-1, 1, 2)), "cube back-left-bottom corner: Z still up on screen");
    }

    std::cout << "--- view cube clicks from a top view ---" << std::endl;
    // AIS_ViewCube's own rule keeps the quarter-turn nearest the current
    // up, and from a top view that is a horizontal up: the part ends up on
    // its side in every edge and corner view.
    const ViewPose top = StandardViewPose(V3d_TypeOfOrientation_Zup_Top);
    CheckPose("from top, front-right-top corner", CubeClickPose(V3d_XposYnegZpos, top),
              home.direction.XYZ(), home.up.XYZ(), ScreenRight(home).XYZ());
    CheckPose("from top, top-right edge", CubeClickPose(V3d_XposZpos, top),
              gp_XYZ(-r2, 0, -r2), gp_XYZ(-r2, 0, r2), gp_XYZ(0, 1, 0));
    CheckPose("from top, TOP again keeps the view", CubeClickPose(V3d_TypeOfOrientation_Zup_Top, top),
              gp_XYZ(0, 0, -1), gp_XYZ(0, 1, 0), gp_XYZ(1, 0, 0));
    {
        const ViewPose rolled{gp_Dir(0, 0, -1), gp_Dir(1, 0, 0)};
        const ViewPose again = CubeClickPose(V3d_TypeOfOrientation_Zup_Top, rolled);
        check(Same(again.up, gp_XYZ(1, 0, 0)), "from a rolled top, TOP keeps the roll");
    }

    std::cout << "--- tipping over onto the top and bottom ---" << std::endl;
    // The side the camera came from ends up at the bottom of the top view.
    const struct
    {
        const char*           name;
        V3d_TypeOfOrientation from;
        gp_XYZ                up;
    } tips[] = {
        {"from FRONT", V3d_TypeOfOrientation_Zup_Front, gp_XYZ(0, 1, 0)},
        {"from BACK", V3d_TypeOfOrientation_Zup_Back, gp_XYZ(0, -1, 0)},
        {"from LEFT", V3d_TypeOfOrientation_Zup_Left, gp_XYZ(1, 0, 0)},
        {"from RIGHT", V3d_TypeOfOrientation_Zup_Right, gp_XYZ(-1, 0, 0)},
    };
    for (const auto& tip : tips) {
        const ViewPose landed =
            CubeClickPose(V3d_TypeOfOrientation_Zup_Top, StandardViewPose(tip.from));
        check(Same(landed.direction, gp_XYZ(0, 0, -1)) && Same(landed.up, tip.up),
              std::string("TOP ") + tip.name + ": that side at the bottom");
    }
    {
        const ViewPose landed = CubeClickPose(V3d_TypeOfOrientation_Zup_Bottom,
                                              StandardViewPose(V3d_TypeOfOrientation_Zup_Front));
        check(Same(landed.direction, gp_XYZ(0, 0, 1)) && Same(landed.up, gp_XYZ(0, -1, 0)),
              "BOTTOM from FRONT: FRONT at the top, as the Bottom command has it");
    }
    // Just off the home corner towards the right side, the tie is broken
    // for real -- by more than rounding -- and RIGHT goes to the bottom.
    {
        ViewPose nearlyHome = home;
        const gp_Ax1 zAxis(gp::Origin(), gp::DZ());
        nearlyHome.direction = home.direction.Rotated(zAxis, 0.2);
        nearlyHome.up = home.up.Rotated(zAxis, 0.2);
        const ViewPose landed = CubeClickPose(V3d_TypeOfOrientation_Zup_Top, nearlyHome);
        check(Same(landed.up, gp_XYZ(-1, 0, 0)), "TOP from nearer the right: RIGHT at the bottom");
    }

    std::cout << "--- aiming keeps the zoom ---" << std::endl;
    {
        Handle(Graphic3d_Camera) camera = new Graphic3d_Camera();
        camera->SetProjectionType(Graphic3d_Camera::Projection_Orthographic);
        camera->SetEyeAndCenter(gp_Pnt(100, -100, 100), gp_Pnt(0, 0, 0));
        camera->SetUp(gp_Dir(0, 0, 1));
        camera->OrthogonalizeUp();
        camera->SetScale(123.0);
        const double distance = camera->Distance();

        AimCamera(camera, StandardViewPose(V3d_TypeOfOrientation_Zup_Front), gp_Pnt(5, 10, 15));
        check(Same(camera->Direction(), gp_XYZ(0, 1, 0)), "aim: now looking along +Y");
        check(Same(camera->Up(), gp_XYZ(0, 0, 1)), "aim: Z up");
        check(camera->Center().Distance(gp_Pnt(5, 10, 15)) < 1.0e-9, "aim: centred where asked");
        check(Near(camera->Distance(), distance, 1.0e-9), "aim: same distance");
        check(Near(camera->Scale(), 123.0, 1.0e-9), "aim: same zoom");
    }

    std::cout << "--- what Fit frames ---" << std::endl;
    {
        AIS_ListOfInteractive objects;
        check(ModelBounds(objects).IsVoid(), "empty design: nothing to frame");

        // The origin axes, exactly as OcctViewport draws them.
        for (const gp_XYZ axis : {gp_XYZ(1, 0, 0), gp_XYZ(0, 1, 0), gp_XYZ(0, 0, 1)}) {
            objects.Append(new AIS_Line(new Geom_CartesianPoint(gp_Pnt(axis * -180.0)),
                                        new Geom_CartesianPoint(gp_Pnt(axis * 180.0))));
        }
        check(ModelBounds(objects).IsVoid(), "origin axes alone: nothing to frame");

        objects.Append(new AIS_Shape(BRepPrimAPI_MakeBox(10.0, 20.0, 30.0).Shape()));
        Bnd_Box bounds = ModelBounds(objects);
        double xMin = 0, yMin = 0, zMin = 0, xMax = 0, yMax = 0, zMax = 0;
        if (!bounds.IsVoid()) {
            bounds.Get(xMin, yMin, zMin, xMax, yMax, zMax);
        }
        check(!bounds.IsVoid() && Near(xMin, 0.0, 1e-3) && Near(yMin, 0.0, 1e-3)
                  && Near(zMin, 0.0, 1e-3) && Near(xMax, 10.0, 1e-3) && Near(yMax, 20.0, 1e-3)
                  && Near(zMax, 30.0, 1e-3),
              "a 10x20x30 box beside the axes: exactly the box");

        // Screen furniture drawn with transform persistence is not geometry.
        Handle(AIS_Shape) furniture = new AIS_Shape(BRepPrimAPI_MakeBox(500.0, 500.0, 500.0).Shape());
        furniture->SetTransformPersistence(
            new Graphic3d_TransformPers(Graphic3d_TMF_TriedronPers, Aspect_TOTP_RIGHT_UPPER,
                                        Graphic3d_Vec2i(100, 100)));
        objects.Append(furniture);
        // Nor is an unbounded construction plane.
        objects.Append(new AIS_Shape(BRepBuilderAPI_MakeFace(gp_Pln(gp::XOY())).Shape()));
        bounds = ModelBounds(objects);
        if (!bounds.IsVoid()) {
            bounds.Get(xMin, yMin, zMin, xMax, yMax, zMax);
        }
        check(!bounds.IsVoid() && Near(xMax, 10.0, 1e-3) && Near(zMax, 30.0, 1e-3),
              "screen furniture and infinite planes are left out");

        // A body a gizmo drag has moved by its presentation transform is
        // framed where it is drawn.
        Handle(AIS_Shape) moved = new AIS_Shape(BRepPrimAPI_MakeBox(1.0, 1.0, 1.0).Shape());
        gp_Trsf shift;
        shift.SetTranslation(gp_Vec(100.0, 0.0, 0.0));
        moved->SetLocalTransformation(shift);
        objects.Append(moved);
        bounds = ModelBounds(objects);
        if (!bounds.IsVoid()) {
            bounds.Get(xMin, yMin, zMin, xMax, yMax, zMax);
        }
        check(Near(xMax, 101.0, 1e-3), "a moved body is framed where it is drawn");
    }

    std::cout << "--- the swing's pacing ---" << std::endl;
    check(Near(EaseInOut(0.0), 0.0) && Near(EaseInOut(1.0), 1.0), "starts and ends in place");
    check(Near(EaseInOut(0.5), 0.5), "halfway at half time");
    check(EaseInOut(0.1) < 0.1 && EaseInOut(0.9) > 0.9, "slow away, slow in");
    check(Near(EaseInOut(-1.0), 0.0) && Near(EaseInOut(2.0), 1.0), "clamped outside [0, 1]");
    bool monotonic = true;
    for (int i = 1; i <= 100; ++i) {
        monotonic = monotonic && EaseInOut(i / 100.0) >= EaseInOut((i - 1) / 100.0);
    }
    check(monotonic, "never swings back");

    std::cout << (failures == 0 ? "ALL PASS" : "FAILURES: " + std::to_string(failures))
              << std::endl;
    return failures == 0 ? 0 : 1;
}
