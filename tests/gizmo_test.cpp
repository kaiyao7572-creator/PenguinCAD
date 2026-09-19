// The Press/Pull drag gizmo: where its arrow sits, which way it points,
// and how far a drag has gone along it.
//
// Every expected number here is computed by hand in the comment beside
// it, from a 10 x 20 x 30 box sitting with one corner on the origin, so
// nothing is read back out of the implementation being tested.
//
// What is NOT here, deliberately: turning a mouse pixel into a ray and a
// 3D point back into a pixel needs a live V3d_View. Those two calls are
// the only untested lines of the drag; everything they feed is below.
#include "core/Body.h"
#include "core/Document.h"
#include "core/GeometryRef.h"
#include "features/PressPullFeature.h"
#include "features/PrimitiveFeatures.h"
#include "gizmos/PressPullGizmo.h"

#include <BRepGProp.hxx>
#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <GProp_GProps.hxx>
#include <TopoDS.hxx>

#include <cmath>
#include <iostream>
#include <memory>
#include <string>

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

static void checkPoint(const gp_Pnt& theValue, double theX, double theY, double theZ,
                       const std::string& theWhat)
{
    const bool ok = theValue.Distance(gp_Pnt(theX, theY, theZ)) <= 1.0e-7;
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << theWhat << "  (got " << theValue.X() << ","
              << theValue.Y() << "," << theValue.Z() << "; expect " << theX << "," << theY << ","
              << theZ << ")" << std::endl;
    if (!ok) {
        ++failures;
    }
}

static double VolumeOf(const TopoDS_Shape& theShape)
{
    if (theShape.IsNull()) {
        return 0.0;
    }
    GProp_GProps props;
    BRepGProp::VolumeProperties(theShape, props);
    return props.Mass();
}

// A 10 x 20 x 30 box with a corner on the origin: every face has a
// distinct area, so a reference to one of them is never ambiguous.
static void MakeBox(Document& theDocument)
{
    auto box = std::make_shared<BoxFeature>(10.0, 20.0, 30.0);
    box->SetName("Box1");
    box->SetOrigin(gp_Pnt(0.0, 0.0, 0.0));
    theDocument.AddFeature(box);
}

// The reference to the face whose centre is furthest along theDirection.
static GeometryRef ExtremeFace(const Document& theDocument, const gp_Dir& theDirection)
{
    GeometryRef best;
    double bestDot = 0.0;
    if (theDocument.Bodies().empty()) {
        return best;
    }
    for (const GeometryRef& ref :
         CollectGeometryRefs(*theDocument.Bodies().front(), EntityType::BRepFace)) {
        const double dot = ref.point.XYZ().Dot(theDirection.XYZ());
        if (best.IsNull() || dot > bestDot) {
            best = ref;
            bestDot = dot;
        }
    }
    return best;
}

static TopoDS_Face FaceOf(const Document& theDocument, const GeometryRef& theRef)
{
    TopoDS_Shape resolved;
    if (!ResolveGeometryRefInShape(theDocument.Shape(), theRef, resolved)) {
        return TopoDS_Face();
    }
    return TopoDS::Face(resolved);
}

int main()
{
    Document document;
    MakeBox(document);

    // ---- 1. where the arrow sits on the top face ----
    std::cout << "1. Arrow placement on the top face" << std::endl;

    const GeometryRef topRef = ExtremeFace(document, gp_Dir(0.0, 0.0, 1.0));
    const TopoDS_Face topFace = FaceOf(document, topRef);
    check(!topFace.IsNull(), "the box's top face resolves");

    PressPullArrow topArrow;
    std::string error;
    check(ComputePressPullArrow(topFace, topArrow, error),
          "a flat face gets an arrow (" + error + ")");

    // The top face spans x 0..10, y 0..20 at z = 30, so its centroid is
    // (5, 10, 30).
    checkPoint(topArrow.axis.Location(), 5.0, 10.0, 30.0, "arrow sits at the top face's centre");
    // Out of the solid is +Z.
    checkNear(topArrow.axis.Direction().X(), 0.0, 1.0e-9, "arrow direction X");
    checkNear(topArrow.axis.Direction().Y(), 0.0, 1.0e-9, "arrow direction Y");
    checkNear(topArrow.axis.Direction().Z(), 1.0, 1.0e-9, "arrow points +Z, out of the box");

    // Area 10 * 20 = 200; sqrt(200) = 14.14213562; half of that.
    checkNear(topArrow.length, 7.07106781, 1.0e-6, "arrow length is half the face's sqrt-area");

    // ---- 2. the outward normal follows the face's orientation ----
    std::cout << "\n2. Every face points out of the solid, not out of its surface"
              << std::endl;

    const GeometryRef bottomRef = ExtremeFace(document, gp_Dir(0.0, 0.0, -1.0));
    PressPullArrow bottomArrow;
    check(ComputePressPullArrow(FaceOf(document, bottomRef), bottomArrow, error),
          "the bottom face gets an arrow (" + error + ")");
    checkPoint(bottomArrow.axis.Location(), 5.0, 10.0, 0.0, "bottom arrow at (5,10,0)");
    checkNear(bottomArrow.axis.Direction().Z(), -1.0, 1.0e-9,
              "bottom arrow points -Z, out of the box");

    const GeometryRef rightRef = ExtremeFace(document, gp_Dir(1.0, 0.0, 0.0));
    PressPullArrow rightArrow;
    check(ComputePressPullArrow(FaceOf(document, rightRef), rightArrow, error),
          "the x = 10 face gets an arrow (" + error + ")");
    checkPoint(rightArrow.axis.Location(), 10.0, 10.0, 15.0, "right arrow at (10,10,15)");
    checkNear(rightArrow.axis.Direction().X(), 1.0, 1.0e-9, "right arrow points +X");
    // Area 20 * 30 = 600; sqrt(600) = 24.49489743; half of that.
    checkNear(rightArrow.length, 12.24744871, 1.0e-6, "right arrow length");

    // ---- 3. how far the drag has gone ----
    std::cout << "\n3. Drag distance is measured ALONG the axis" << std::endl;

    const gp_Ax1 axis = topArrow.axis;  // (5,10,30), +Z
    const gp_Pnt start(5.0, 10.0, 30.0);

    // 37 - 30 = +7, pulling out.
    checkNear(DragDistanceAlongAxis(axis, start, gp_Pnt(5.0, 10.0, 37.0)), 7.0, 1.0e-9,
              "dragging to z = 37 is +7");
    // 26 - 30 = -4, pushing in.
    checkNear(DragDistanceAlongAxis(axis, start, gp_Pnt(5.0, 10.0, 26.0)), -4.0, 1.0e-9,
              "dragging to z = 26 is -4");
    // The assertion most worth having: (100, -50, 0) dotted with (0,0,1)
    // is 0. Taking the raw distance between the points would report
    // 111.8 and shove the face out on a sideways mouse sweep.
    checkNear(DragDistanceAlongAxis(axis, start, gp_Pnt(105.0, -40.0, 30.0)), 0.0, 1.0e-9,
              "motion perpendicular to the axis contributes nothing");
    // Same sideways motion, plus 7 up: still exactly +7.
    checkNear(DragDistanceAlongAxis(axis, start, gp_Pnt(105.0, -40.0, 37.0)), 7.0, 1.0e-9,
              "only the along-axis component of a diagonal drag counts");
    // Zero drag: a click that never moved.
    checkNear(DragDistanceAlongAxis(axis, start, start), 0.0, 1.0e-12, "a click that never moved is 0");

    // On the bottom face, whose axis points -Z, dragging DOWN is a pull
    // OUT: (0,0,-3) dotted with (0,0,-1) = +3.
    checkNear(DragDistanceAlongAxis(bottomArrow.axis, gp_Pnt(5.0, 10.0, 0.0),
                                    gp_Pnt(5.0, 10.0, -3.0)),
              3.0, 1.0e-9, "dragging the bottom face down is a positive pull");
    // And dragging it up pushes in: (0,0,4) dot (0,0,-1) = -4.
    checkNear(DragDistanceAlongAxis(bottomArrow.axis, gp_Pnt(5.0, 10.0, 0.0),
                                    gp_Pnt(5.0, 10.0, 4.0)),
              -4.0, 1.0e-9, "dragging the bottom face up is a negative push");

    // ---- 4. where the cursor is on the drag axis ----
    std::cout << "\n4. The cursor ray's closest point on the axis" << std::endl;

    gp_Pnt onAxis;
    // Ray along -X at y = 10, z = 37 crosses the axis line x=5,y=10 at
    // exactly (5,10,37).
    check(ClosestPointOnAxis(axis, gp_Lin(gp_Pnt(105.0, 10.0, 37.0), gp_Dir(-1.0, 0.0, 0.0)),
                             onAxis),
          "a ray crossing the axis has a closest point");
    checkPoint(onAxis, 5.0, 10.0, 37.0, "an intersecting ray lands on the crossing point");
    checkNear(DragDistanceAlongAxis(axis, start, onAxis), 7.0, 1.0e-9,
              "which is a +7 drag");

    // Skew case: the axis is the world Z line, the ray runs along +Y
    // through (4, 0, 9). The common perpendicular is along X, so the foot
    // on the axis is (0, 0, 9) -- the ray's own height.
    const gp_Ax1 worldZ(gp_Pnt(0.0, 0.0, 0.0), gp_Dir(0.0, 0.0, 1.0));
    check(ClosestPointOnAxis(worldZ, gp_Lin(gp_Pnt(4.0, 0.0, 9.0), gp_Dir(0.0, 1.0, 0.0)), onAxis),
          "a skew ray has a closest point");
    checkPoint(onAxis, 0.0, 0.0, 9.0, "skew ray's foot on the axis is (0,0,9)");

    // Looking straight down the axis: no unique answer, and refusing is
    // the only honest response.
    check(!ClosestPointOnAxis(worldZ, gp_Lin(gp_Pnt(3.0, 4.0, 0.0), gp_Dir(0.0, 0.0, 1.0)), onAxis),
          "a ray parallel to the axis is refused");
    check(!ClosestPointOnAxis(worldZ, gp_Lin(gp_Pnt(3.0, 4.0, 0.0), gp_Dir(0.0, 0.0, -1.0)), onAxis),
          "an antiparallel ray is refused too");

    // ---- 5. the arrow's pixel hit test ----
    std::cout << "\n5. Hit-testing the arrow in screen pixels" << std::endl;

    // Segment from (10,0) to (10,20) on screen.
    checkNear(DistanceToSegment2d(0.0, 0.0, 10.0, 0.0, 10.0, 20.0), 10.0, 1.0e-9,
              "a point level with the tail is its horizontal distance away");
    checkNear(DistanceToSegment2d(13.0, 10.0, 10.0, 0.0, 10.0, 20.0), 3.0, 1.0e-9,
              "a point beside the shaft is 3 px away");
    // Past the tail: clamped to the endpoint, so 5 px, not 0.
    checkNear(DistanceToSegment2d(10.0, -5.0, 10.0, 0.0, 10.0, 20.0), 5.0, 1.0e-9,
              "a point beyond the tail clamps to the end, not the infinite line");
    checkNear(DistanceToSegment2d(10.0, 26.0, 10.0, 0.0, 10.0, 20.0), 6.0, 1.0e-9,
              "a point beyond the tip clamps to the end too");
    // 3-4-5 triangle onto a degenerate (zero-length) segment.
    checkNear(DistanceToSegment2d(0.0, 0.0, 3.0, 4.0, 3.0, 4.0), 5.0, 1.0e-9,
              "a zero-length segment is just its point");

    // ---- 6. the arrow solid ----
    std::cout << "\n6. The arrow solid spans the axis" << std::endl;

    const TopoDS_Shape arrowShape = MakePressPullArrowShape(topArrow.axis, topArrow.length);
    check(!arrowShape.IsNull(), "an arrow shape is built");

    Bnd_Box bounds;
    BRepBndLib::Add(arrowShape, bounds);
    double xMin = 0.0, yMin = 0.0, zMin = 0.0, xMax = 0.0, yMax = 0.0, zMax = 0.0;
    bounds.Get(xMin, yMin, zMin, xMax, yMax, zMax);
    // Tail at z = 30, tip at 30 + 7.07106781. Bnd_Box adds a small gap
    // around a shape, hence the loose tolerance.
    checkNear(zMin, 30.0, 0.05, "the arrow starts at the face");
    checkNear(zMax, 37.07106781, 0.05, "the arrow ends one arrow-length out");
    // The head is the widest part: radius 0.10 * 7.07106781 = 0.70710678,
    // centred on x = 5, so the box spans 4.29 .. 5.71.
    checkNear(xMax - 5.0, 0.70710678, 0.05, "the arrow is as wide as its head and no wider");

    // ---- 7. the gizmo and the feature agree on direction ----
    //
    // The one thing that cannot be checked by looking at the arrow alone:
    // a drag that LOOKS like it pulls material out must produce a feature
    // that actually adds it. If either side's outward-normal handling
    // flipped, these volumes would come out the other way round.
    std::cout << "\n7. A drag along the arrow builds the feature it looks like" << std::endl;

    ComputeContext computeContext;
    computeContext.document = &document;

    {
        // Top face, dragged from (5,10,30) to (5,10,37): +7.
        const double distance = DragDistanceAlongAxis(topArrow.axis, start, gp_Pnt(5.0, 10.0, 37.0));
        PressPullFeature feature(topRef, distance);
        TopoDS_Shape result;
        std::string featureError;
        check(feature.Compute(computeContext, document.Shape(), result, featureError),
              "press/pull of +7 on the top face computes (" + featureError + ")");
        // 10 * 20 * 30 = 6000, plus 10 * 20 * 7 = 1400.
        checkNear(VolumeOf(result), 7400.0, 1.0e-6, "pulling the top face out adds 1400");
    }

    {
        // Top face, dragged down to z = 26: -4.
        const double distance = DragDistanceAlongAxis(topArrow.axis, start, gp_Pnt(5.0, 10.0, 26.0));
        PressPullFeature feature(topRef, distance);
        TopoDS_Shape result;
        std::string featureError;
        check(feature.Compute(computeContext, document.Shape(), result, featureError),
              "press/pull of -4 on the top face computes (" + featureError + ")");
        // 6000 minus 10 * 20 * 4 = 800.
        checkNear(VolumeOf(result), 5200.0, 1.0e-6, "pushing the top face in removes 800");
    }

    {
        // Bottom face, dragged DOWN to z = -3. Along its own outward
        // normal that is +3, so it must ADD material below the box.
        const double distance = DragDistanceAlongAxis(bottomArrow.axis, gp_Pnt(5.0, 10.0, 0.0),
                                                      gp_Pnt(5.0, 10.0, -3.0));
        PressPullFeature feature(bottomRef, distance);
        TopoDS_Shape result;
        std::string featureError;
        check(feature.Compute(computeContext, document.Shape(), result, featureError),
              "press/pull of +3 on the bottom face computes (" + featureError + ")");
        // 6000 plus 10 * 20 * 3 = 600.
        checkNear(VolumeOf(result), 6600.0, 1.0e-6,
                  "dragging the bottom face downward adds 600, it does not cut");
    }

    // A drag that never moved must not become a feature: the gizmo checks
    // this before committing, and the feature refuses it too.
    {
        PressPullFeature feature(topRef, 0.0);
        TopoDS_Shape result;
        std::string featureError;
        check(!feature.Compute(computeContext, document.Shape(), result, featureError),
              "a zero-distance press/pull is refused, not silently applied");
    }

    // ---- 8. a curved face is refused, not guessed at ----
    std::cout << "\n8. A curved face is refused rather than guessed at" << std::endl;

    Document cylinderDocument;
    auto cylinder = std::make_shared<CylinderFeature>(5.0, 12.0);
    cylinder->SetName("Cylinder1");
    cylinderDocument.AddFeature(cylinder);

    // The side wall is the largest face: 2*pi*5*12 = 376.99 against
    // pi*5*5 = 78.54 for each cap. (Its centroid is on the axis, like the
    // caps', so position cannot tell them apart.)
    GeometryRef sideRef;
    for (const GeometryRef& ref :
         CollectGeometryRefs(*cylinderDocument.Bodies().front(), EntityType::BRepFace)) {
        if (sideRef.IsNull() || ref.measure > sideRef.measure) {
            sideRef = ref;
        }
    }
    checkNear(sideRef.measure, 376.99111843, 1.0e-6, "the curved wall's area is 2*pi*5*12");
    check(!sideRef.IsNull(), "the cylinder's curved wall is found");

    PressPullArrow curvedArrow;
    std::string curvedError;
    check(!ComputePressPullArrow(FaceOf(cylinderDocument, sideRef), curvedArrow, curvedError),
          "a cylindrical face gets no arrow");
    // The arrow's own wording, not the feature's: PressPullFeature can
    // move a cylindrical face by changing its radius, so borrowing its
    // message here would tell the user something untrue about the tool.
    check(curvedError == "the press/pull arrow needs a flat face",
          "and says why in its own words (" + curvedError + ")");

    // A null face is refused rather than crashing.
    check(!ComputePressPullArrow(TopoDS_Face(), curvedArrow, curvedError),
          "a null face is refused");

    std::cout << std::endl;
    if (failures == 0) {
        std::cout << "gizmo_test: all assertions passed." << std::endl;
        return 0;
    }
    std::cout << "gizmo_test: " << failures << " assertion(s) FAILED." << std::endl;
    return 1;
}
