// The Fusion object model: the entity taxonomy, bodies split out of a
// document shape with names that survive a rebuild, and durable
// references to individual faces, edges and vertices. All headless.
#include "core/Body.h"
#include "core/Document.h"
#include "core/Entity.h"
#include "core/GeometryRef.h"
#include "features/PrimitiveFeatures.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Pnt.hxx>

#include <cmath>
#include <iostream>
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

static TopoDS_Shape Box(double x, double y, double z, const gp_Pnt& at)
{
    return BRepPrimAPI_MakeBox(at, x, y, z).Shape();
}

static TopoDS_Shape CompoundOf(const std::vector<TopoDS_Shape>& theShapes)
{
    TopoDS_Compound compound;
    BRep_Builder builder;
    builder.MakeCompound(compound);
    for (const TopoDS_Shape& shape : theShapes) {
        builder.Add(compound, shape);
    }
    return compound;
}

int main()
{
    // ---- 1. the taxonomy reports Fusion's own names ----
    {
        check(EntityTypeName(EntityType::BRepFace) == "BRepFace", "BRepFace keeps its API name");
        check(EntityTypeName(EntityType::SketchEllipticalArc) == "SketchEllipticalArc",
              "SketchEllipticalArc keeps its API name");
        check(EntityDisplayName(EntityType::BRepFace) == "Face", "a BRepFace shows as 'Face'");
        check(EntityDisplayName(EntityType::SketchFittedSpline) == "Fit Point Spline",
              "a fitted spline shows as Fusion words it");

        check(FolderOf(EntityType::BRepBody) == BrowserFolder::Bodies, "bodies go in Bodies");
        check(FolderOf(EntityType::Sketch) == BrowserFolder::Sketches, "sketches go in Sketches");
        check(FolderOf(EntityType::ConstructionPlane) == BrowserFolder::Construction,
              "construction planes go in Construction");
        check(FolderOf(EntityType::Feature) == BrowserFolder::None,
              "features sit directly under the component, in no folder");

        // Fusion draws Origin above Bodies above Sketches above Construction.
        const std::vector<BrowserFolder>& order = BrowserFolderOrder();
        const auto indexOf = [&order](BrowserFolder theFolder) {
            for (std::size_t i = 0; i < order.size(); ++i) {
                if (order[i] == theFolder) {
                    return static_cast<int>(i);
                }
            }
            return -1;
        };
        check(indexOf(BrowserFolder::Origin) < indexOf(BrowserFolder::Bodies),
              "Origin is drawn above Bodies");
        check(indexOf(BrowserFolder::Bodies) < indexOf(BrowserFolder::Sketches),
              "Bodies is drawn above Sketches");
        check(indexOf(BrowserFolder::Sketches) < indexOf(BrowserFolder::Construction),
              "Sketches is drawn above Construction");

        check(IsSketchCurve(EntityType::SketchConicCurve), "a conic curve is a sketch curve");
        check(!IsSketchCurve(EntityType::SketchPoint), "a sketch point is not a curve");
        check(!IsSketchCurve(EntityType::Profile), "a profile is not a curve");

        check(!IsSelectable(EntityType::BRepLoop), "loops are structural, never picked");
        check(!IsSelectable(EntityType::BRepCoEdge), "co-edges are structural, never picked");
        check(IsSelectable(EntityType::BRepFace), "faces are picked");
        check(IsSelectable(EntityType::BRepVertex), "vertices are picked");
    }

    // ---- 2. a box body reports Fusion's counts, not OCCT's use counts ----
    {
        Body body(Box(10.0, 20.0, 30.0, gp_Pnt(0, 0, 0)), "Body1");

        check(body.Type() == EntityType::BRepBody, "a body is a BRepBody");
        check(body.IsSolid(), "a box is a solid body");
        check(body.BodyKind() == Body::Kind::Solid, "and reports Kind::Solid");

        // A cube shares each vertex between three edges and each edge
        // between two faces; the counts are 6/12/8, not 24/48.
        check(body.FaceCount() == 6, "a box has 6 faces");
        check(body.EdgeCount() == 12, "a box has 12 edges, not 24 edge-uses");
        check(body.VertexCount() == 8, "a box has 8 vertices, not 48 vertex-uses");
        check(body.Lumps().size() == 1, "a box is one lump");
        check(body.Shells().size() == 1, "a box has one shell");

        checkNear(body.Volume(), 6000.0, 1.0e-6, "volume is 10 x 20 x 30");
        // 2*(10*20 + 20*30 + 10*30) = 2*(200 + 600 + 300) = 2200
        checkNear(body.Area(), 2200.0, 1.0e-6, "surface area is 2200");
        checkNear(body.Centroid().X(), 5.0, 1.0e-9, "centroid x is half the width");
        checkNear(body.Centroid().Z(), 15.0, 1.0e-9, "centroid z is half the height");
    }

    // ---- 3. a surface body is not a solid ----
    {
        Body solid(Box(10.0, 10.0, 10.0, gp_Pnt(0, 0, 0)), "Body1");
        const std::vector<TopoDS_Face> faces = solid.Faces();
        check(!faces.empty(), "the box has faces to lift");

        Body surface(faces.front(), "Body2");
        check(!surface.IsSolid(), "a lone face is not a solid body");
        check(surface.BodyKind() == Body::Kind::Surface, "it is a surface body");
        checkNear(surface.Volume(), 0.0, 1.0e-12, "a surface body has no volume");
        check(surface.FaceCount() == 1, "and exactly one face");
    }

    // ---- 4. a compound splits into one body per child ----
    {
        const TopoDS_Shape two = CompoundOf({Box(10, 10, 10, gp_Pnt(0, 0, 0)),
                                             Box(4, 4, 4, gp_Pnt(50, 0, 0))});
        check(SplitIntoBodies(two).size() == 2, "a compound of two solids is two bodies");
        check(SplitIntoBodies(Box(1, 1, 1, gp_Pnt(0, 0, 0))).size() == 1,
              "a bare solid is one body");
        check(SplitIntoBodies(TopoDS_Shape()).empty(), "a null shape is no bodies");

        // Nested compounds are what MakeCompoundOf produces when a New Body
        // operation lands on top of an existing compound.
        const TopoDS_Shape nested = CompoundOf({two, Box(2, 2, 2, gp_Pnt(-50, 0, 0))});
        check(SplitIntoBodies(nested).size() == 3, "nested compounds flatten to three bodies");
    }

    // ---- 5. body names survive a rebuild ----
    {
        BodyTable table;
        table.Update(CompoundOf({Box(10, 10, 10, gp_Pnt(0, 0, 0)),
                                 Box(4, 4, 4, gp_Pnt(50, 0, 0))}));
        check(table.Count() == 2, "two bodies");
        check(table.Bodies()[0]->Name() == "Body1", "the first is Body1");
        check(table.Bodies()[1]->Name() == "Body2", "the second is Body2");

        table.Bodies()[1]->SetVisible(false);

        // Rebuild with the SAME geometry in the OPPOSITE order: names must
        // follow the geometry, not the position in the list.
        table.Update(CompoundOf({Box(4, 4, 4, gp_Pnt(50, 0, 0)),
                                 Box(10, 10, 10, gp_Pnt(0, 0, 0))}));
        check(table.Count() == 2, "still two bodies");
        check(table.Bodies()[0]->Name() == "Body2", "the small box kept the name Body2");
        check(table.Bodies()[1]->Name() == "Body1", "the big box kept the name Body1");
        check(!table.Bodies()[0]->IsVisible(), "and Body2 is still hidden");

        // A genuinely new body gets a new name, never a recycled one.
        table.Update(CompoundOf({Box(10, 10, 10, gp_Pnt(0, 0, 0)),
                                 Box(4, 4, 4, gp_Pnt(50, 0, 0)),
                                 Box(3, 3, 3, gp_Pnt(0, 60, 0))}));
        check(table.Count() == 3, "three bodies now");
        check(table.Find("Body3") != nullptr, "the newcomer is Body3");
        check(table.Find("Body1") != nullptr && table.Find("Body2") != nullptr,
              "and the other two kept their names");

        // Removing a body must not hand its name to a survivor.
        table.Update(Box(10, 10, 10, gp_Pnt(0, 0, 0)));
        check(table.Count() == 1, "one body left");
        check(table.Bodies()[0]->Name() == "Body1", "the survivor is still Body1");
    }

    // ---- 6. a resized body keeps its name ----
    {
        BodyTable table;
        table.Update(Box(10, 10, 10, gp_Pnt(0, 0, 0)));
        check(table.Bodies()[0]->Name() == "Body1", "starts as Body1");

        // Same place, a hair bigger: the user edited a dimension.
        table.Update(Box(10.05, 10.0, 10.0, gp_Pnt(0, 0, 0)));
        check(table.Bodies()[0]->Name() == "Body1", "a small edit keeps the name");

        // Somewhere else entirely and much bigger: a different body.
        table.Update(Box(80, 80, 80, gp_Pnt(500, 500, 500)));
        check(table.Bodies()[0]->Name() != "Body1",
              "a body that shares nothing with the old one gets a new name");
    }

    // ---- 7. the document exposes its bodies ----
    {
        Document doc;
        check(doc.Bodies().empty(), "an empty document has no bodies");

        auto box = std::make_shared<BoxFeature>();
        box->SetName("Box1");
        doc.AddFeature(box);
        check(doc.Bodies().size() == 1, "one primitive is one body");
        check(doc.FindBody("Body1") != nullptr, "and it is reachable by name");
        check(doc.FindBody("Nope") == nullptr, "a name nothing carries finds nothing");
        check(doc.Bodies()[0]->FaceCount() == 6, "the body has a box's six faces");

        doc.Undo();
        check(doc.Bodies().empty(), "undo takes the body away again");
    }

    // ---- 8. references to a face survive, and refuse when ambiguous ----
    {
        // Deliberately unequal sides: every face has a distinct area, so a
        // reference to one is unambiguous.
        Body body(Box(10.0, 20.0, 30.0, gp_Pnt(0, 0, 0)), "Body1");
        const std::vector<GeometryRef> faces = CollectGeometryRefs(body, EntityType::BRepFace);
        check(faces.size() == 6, "six face references");

        TopoDS_Shape found;
        check(ResolveGeometryRef(body, faces[0], found), "the first face resolves");
        check(!found.IsNull() && found.ShapeType() == TopAbs_FACE, "and it really is a face");

        GeometryRef decoded;
        check(GeometryRef::Decode(faces[0].Encode(), decoded), "a face reference round-trips");
        check(decoded.type == faces[0].type && decoded.body == faces[0].body,
              "type and body survive encoding");
        check(ResolveGeometryRef(body, decoded, found), "and the decoded one still resolves");
        check(DecodeGeometryRefs(EncodeGeometryRefs(faces)).size() == 6, "so does a list of them");
        check(DecodeGeometryRefs("rubbish").empty(), "rubbish decodes to nothing");

        // A reference into a body that is not this one must not resolve.
        GeometryRef foreign = faces[0];
        foreign.body = "Body7";
        check(!ResolveGeometryRef(body, foreign, found),
              "a reference naming another body does not resolve here");

        // The same face on a body rebuilt identically still resolves.
        Body rebuilt(Box(10.0, 20.0, 30.0, gp_Pnt(0, 0, 0)), "Body1");
        check(ResolveGeometryRef(rebuilt, faces[0], found),
              "the reference survives the body being rebuilt");

        // And on a body that has moved far away, it does not.
        Body moved(Box(10.0, 20.0, 30.0, gp_Pnt(900, 900, 900)), "Body1");
        check(!ResolveGeometryRef(moved, faces[0], found),
              "a face that is no longer there does not resolve");
    }

    // ---- 9. a cube's identical faces are refused, not guessed ----
    {
        // Every face of a cube has the same area, and opposite faces are
        // mirror images. A reference must not silently pick one.
        Body cube(Box(10.0, 10.0, 10.0, gp_Pnt(0, 0, 0)), "Body1");
        const std::vector<GeometryRef> faces = CollectGeometryRefs(cube, EntityType::BRepFace);
        check(faces.size() == 6, "six faces on a cube");

        TopoDS_Shape found;
        check(ResolveGeometryRef(cube, faces[0], found),
              "a cube face still resolves -- its CENTRE is unique even though its area is not");

        // Now ask with the right area but a centre exactly between two
        // faces: nothing should win.
        GeometryRef between = faces[0];
        between.point = cube.Centroid();
        check(!ResolveGeometryRef(cube, between, found),
              "a reference that does not single out one face is refused");
    }

    // ---- 10. edges and vertices ----
    {
        Body body(Box(10.0, 20.0, 30.0, gp_Pnt(0, 0, 0)), "Body1");
        const std::vector<GeometryRef> edges = CollectGeometryRefs(body, EntityType::BRepEdge);
        const std::vector<GeometryRef> vertices = CollectGeometryRefs(body, EntityType::BRepVertex);
        check(edges.size() == 12, "twelve edge references");
        check(vertices.size() == 8, "eight vertex references");

        TopoDS_Shape found;
        check(ResolveGeometryRef(body, edges[0], found) && found.ShapeType() == TopAbs_EDGE,
              "an edge reference resolves to an edge");
        check(ResolveGeometryRef(body, vertices[0], found) && found.ShapeType() == TopAbs_VERTEX,
              "a vertex reference resolves to a vertex");

        // Asking for a face with an edge's reference must not work.
        GeometryRef confused = edges[0];
        confused.type = EntityType::BRepFace;
        check(!ResolveGeometryRef(body, confused, found),
              "an edge's measurements do not match any face");
    }

    std::cout << std::endl;
    if (failures == 0) {
        std::cout << "ALL ENTITY TESTS PASSED" << std::endl;
        return 0;
    }
    std::cout << failures << " ENTITY TEST(S) FAILED" << std::endl;
    return 1;
}
