#include "view/ViewOrientation.h"

#include <AIS_InteractiveObject.hxx>
#include <AIS_Shape.hxx>
#include <BRepBndLib.hxx>
#include <Precision.hxx>
#include <V3d.hxx>
#include <gp.hxx>
#include <gp_Ax1.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <cmath>

namespace lcad {

ViewPose StandardViewPose(V3d_TypeOfOrientation theOrientation)
{
    // GetProjAxis points from the model back towards the eye.
    const gp_Dir towardsEye = V3d::GetProjAxis(theOrientation);
    const gp_Dir direction = towardsEye.Reversed();

    // Looking straight down or up, Z cannot be "up", so the up direction
    // has to be chosen -- and Fusion chooses the one that keeps FRONT at
    // the bottom of the screen looking down and at the top looking up,
    // i.e. the view you get by tipping the front view over its X axis.
    if (theOrientation == V3d_TypeOfOrientation_Zup_Top) {
        return ViewPose{direction, gp::DY()};
    }
    if (theOrientation == V3d_TypeOfOrientation_Zup_Bottom) {
        return ViewPose{direction, gp::DY().Reversed()};
    }

    // Everything else keeps Z up. Projecting it square to the view
    // direction here, rather than leaving it to the camera, keeps the pose
    // honest for the corner views, where raw Z is not perpendicular.
    const gp_Vec z(gp::DZ());
    const gp_Vec d(direction);
    const gp_Vec up = z - d * z.Dot(d);
    return ViewPose{direction, gp_Dir(up)};
}

ViewPose CubeClickPose(V3d_TypeOfOrientation theTarget, const ViewPose& theCurrent)
{
    const ViewPose upright = StandardViewPose(theTarget);
    const gp_Dir&  newDirection = upright.direction;
    if (!newDirection.IsParallel(gp::DZ(), Precision::Angular())) {
        return upright;
    }

    // Tip the current up over by the shortest rotation that carries the
    // current direction onto the new one. Already looking this way, the
    // roll is kept; looking exactly the other way there is no shortest
    // swing, and the current up is as good a guess as any.
    gp_Vec swung(theCurrent.up);
    const gp_Vec axis = gp_Vec(theCurrent.direction).Crossed(gp_Vec(newDirection));
    if (axis.Magnitude() > Precision::Confusion()) {
        swung.Rotate(gp_Ax1(gp::Origin(), gp_Dir(axis)), theCurrent.direction.Angle(newDirection));
    }

    // Squared to the side it points most nearly at. Fusion's own up is
    // tried first and only a clearly better side displaces it, so the
    // exact tie from the home corner -- and the rounding either side of
    // it -- always lands on the top view a user expects.
    constexpr double kTieTolerance = 1.0e-3;
    ViewPose best = upright;
    double   bestDot = gp_Vec(upright.up).Dot(swung);
    gp_Dir   candidate = upright.up;
    for (int quarter = 1; quarter < 4; ++quarter) {
        candidate = candidate.Rotated(gp_Ax1(gp::Origin(), newDirection), M_PI_2);
        const double dot = gp_Vec(candidate).Dot(swung);
        if (dot > bestDot + kTieTolerance) {
            bestDot = dot;
            best.up = candidate;
        }
    }
    return best;
}

void AimCamera(const Handle(Graphic3d_Camera)& theCamera,
               const ViewPose&                 thePose,
               const gp_Pnt&                   theCenter)
{
    if (theCamera.IsNull()) {
        return;
    }
    const Standard_Real distance = theCamera->Distance() > 0.0 ? theCamera->Distance() : 500.0;
    const gp_Pnt eye = theCenter.Translated(gp_Vec(thePose.direction) * -distance);
    theCamera->SetEyeAndCenter(eye, theCenter);
    theCamera->SetUp(thePose.up);
    theCamera->OrthogonalizeUp();
}

Bnd_Box ModelBounds(const AIS_ListOfInteractive& theObjects)
{
    Bnd_Box bounds;
    for (AIS_ListOfInteractive::Iterator it(theObjects); it.More(); it.Next()) {
        const Handle(AIS_Shape) shape = Handle(AIS_Shape)::DownCast(it.Value());
        if (shape.IsNull() || shape->Shape().IsNull()) {
            continue;
        }
        if (!shape->TransformPersistence().IsNull()) {
            continue;
        }

        Bnd_Box own;
        BRepBndLib::Add(shape->Shape(), own);
        // An unbounded shape (an infinite construction plane, say) would
        // fit the view to infinity.
        if (own.IsVoid() || own.IsOpen()) {
            continue;
        }
        // A gizmo drag moves a body by its presentation transform before
        // the feature lands; the fit should frame where it is drawn.
        if (shape->Transformation().Form() != gp_Identity) {
            own = own.Transformed(shape->Transformation());
        }
        bounds.Add(own);
    }
    return bounds;
}

double EaseInOut(double theT)
{
    const double t = std::clamp(theT, 0.0, 1.0);
    if (t < 0.5) {
        return 4.0 * t * t * t;
    }
    const double u = -2.0 * t + 2.0;
    return 1.0 - u * u * u / 2.0;
}

} // namespace lcad
