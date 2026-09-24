// Selection: what the user can point at, what comes back when they do,
// and what happens to a pick when the model is rebuilt underneath it.
//
// ARCHITECTURE.md has only ever stated these rules in prose -- the
// indexed-map rule under core/Body.h, the structural/pickable split under
// core/Entity.h, and four viewport traps under "Viewport selection". This
// suite is the headless half of that written down as assertions.
//
// Three of the four traps need an AIS context and a window server and are
// NOT covered here. They are named, one by one, with the precise reason,
// in the block just above main() -- read it before believing this file
// covers "selection".
//
// Counts and areas are worked out by hand in the comment beside them,
// deliberately not read back from the implementation.
#include "core/Body.h"
#include "core/Document.h"
#include "core/Entity.h"
#include "core/GeometryRef.h"
#include "core/GeometrySelection.h"
#include "core/ProfileSelection.h"
#include "features/PrimitiveFeatures.h"

// AIS_Shape is included for AIS_Shape::SelectionMode alone, which is an
// inline static: no AIS object is ever built here, so this stays a
// headless binary and the harness still links no TKV3d.
#include <AIS_Shape.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <GProp_GProps.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Shape.hxx>

#include <cmath>
#include <iostream>
#include <memory>
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

static void checkCount(std::size_t theValue, std::size_t theExpected, const std::string& theWhat)
{
    const bool ok = theValue == theExpected;
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << theWhat << "  (got " << theValue
              << ", expect " << theExpected << ")" << std::endl;
    if (!ok) {
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

static void checkText(const std::string& theValue, const std::string& theExpected,
                      const std::string& theWhat)
{
    const bool ok = theValue == theExpected;
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << theWhat << "  (got \"" << theValue
              << "\", expect \"" << theExpected << "\")" << std::endl;
    if (!ok) {
        ++failures;
    }
}

// Sub-shapes the way TopExp_Explorer counts them: once per USE. This is
// the number the indexed map exists to avoid, so the suite states it
// rather than trusting the comment that says it.
static std::size_t ExplorerUses(const TopoDS_Shape& theShape, TopAbs_ShapeEnum theType)
{
    std::size_t uses = 0;
    for (TopExp_Explorer explorer(theShape, theType); explorer.More(); explorer.Next()) {
        ++uses;
    }
    return uses;
}

// How many of these are actually different shapes, by OCCT's own identity.
template <typename Shape>
static std::size_t DistinctCount(const std::vector<Shape>& theShapes)
{
    std::size_t distinct = 0;
    for (std::size_t i = 0; i < theShapes.size(); ++i) {
        bool seenBefore = false;
        for (std::size_t j = 0; j < i; ++j) {
            seenBefore = seenBefore || theShapes[i].IsSame(theShapes[j]);
        }
        if (!seenBefore) {
            ++distinct;
        }
    }
    return distinct;
}

static double FaceArea(const TopoDS_Shape& theFace)
{
    if (theFace.IsNull()) {
        return 0.0;
    }
    GProp_GProps props;
    BRepGProp::SurfaceProperties(theFace, props);
    return props.Mass();
}

static TopoDS_Shape Box(double x, double y, double z, const gp_Pnt& at)
{
    return BRepPrimAPI_MakeBox(at, x, y, z).Shape();
}

// A hand-built profile reference. The selection compares refs by boundary
// ids and seed, so a test can make them without a sketch -- and should,
// because what is under test here is the selection, not the arrangement.
static ProfileRef Region(std::vector<int> theBoundary, double x, double y)
{
    ProfileRef ref;
    ref.boundary = std::move(theBoundary);
    ref.seed     = gp_Pnt2d(x, y);
    return ref;
}

// What a prune of the viewport selection has to do after a rebuild: keep
// the picks that still name something, drop the ones that do not.
//
// This helper is the TEST'S own. Unlike ProfileSelection, GeometrySelection
// has no Prune() of its own -- the viewport hands over the whole picked set
// on every click, so nothing in core re-points one. What is asserted below
// is therefore ResolveGeometryRef's real answer for each pick, which is the
// rule any prune would have to be built on.
static std::vector<GeometryRef> SurvivorsIn(const Body&                     theBody,
                                            const std::vector<GeometryRef>& theItems)
{
    std::vector<GeometryRef> kept;
    for (const GeometryRef& item : theItems) {
        TopoDS_Shape found;
        if (ResolveGeometryRef(theBody, item, found)) {
            kept.push_back(item);
        }
    }
    return kept;
}

int main()
{
    // ---- 1. sub-entities come from an indexed map, never the explorer ----
    //
    // The thing ARCHITECTURE.md says hand-rolled OCCT wrappers get wrong,
    // and the one that would be invisible: a viewport armed from the
    // explorer's list arms the same corner six times and still looks
    // exactly right on screen.
    //
    // A box is 6 faces, 12 edges, 8 vertices. TopExp_Explorer visits once
    // per USE: each edge is used by two faces (12 x 2 = 24) and each
    // corner by three edges in two faces apiece (8 x 6 = 48).
    {
        const TopoDS_Shape shape = Box(20.0, 20.0, 20.0, gp_Pnt(0, 0, 0));
        Body               body(shape, "Body1");

        checkCount(body.FaceCount(), 6, "a box has 6 faces");
        checkCount(body.EdgeCount(), 12, "a box has 12 edges");
        checkCount(body.VertexCount(), 8, "a box has 8 vertices");

        // The numbers being avoided, on the record. If these ever change,
        // the three above stop meaning anything.
        checkCount(ExplorerUses(shape, TopAbs_FACE), 6, "the explorer agrees about faces");
        checkCount(ExplorerUses(shape, TopAbs_EDGE), 24,
                   "but reports 24 edge-USES for the same 12 edges");
        checkCount(ExplorerUses(shape, TopAbs_VERTEX), 48,
                   "and 48 vertex-USES for the same 8 corners");

        // The lists the viewport and the gizmos actually walk, not just the
        // counters: as long, and deduplicated rather than merely as long.
        checkCount(body.Faces().size(), 6, "Faces() hands back six");
        checkCount(body.Edges().size(), 12, "Edges() hands back twelve");
        checkCount(body.Vertices().size(), 8, "Vertices() hands back eight");
        checkCount(DistinctCount(body.Edges()), 12, "and they are twelve DIFFERENT edges");
        checkCount(DistinctCount(body.Vertices()), 8, "and eight DIFFERENT corners");

        checkCount(body.Lumps().size(), 1, "one lump: a box is one connected chunk");
        checkCount(body.Shells().size(), 1, "with one closed shell around it");

        // The selection path has to agree with what the browser shows, or
        // the viewport arms things the user cannot see the point of.
        checkCount(CollectGeometryRefs(body, EntityType::BRepFace).size(), 6,
                   "six face references to pick from");
        checkCount(CollectGeometryRefs(body, EntityType::BRepEdge).size(), 12,
                   "twelve edge references, not twenty-four");
        checkCount(CollectGeometryRefs(body, EntityType::BRepVertex).size(), 8,
                   "eight vertex references, not forty-eight");
    }

    // ---- 1b. the hollow cube ARCHITECTURE.md describes ----
    //
    // A 20mm cube with a 10mm cube taken out of its middle: one lump, two
    // shells -- the outer boundary and the cavity. 6 + 6 = 12 faces,
    // 12 + 12 = 24 edges, 8 + 8 = 16 vertices. Volume 8000 - 1000 = 7000,
    // area 2400 + 600 = 3000.
    {
        BRepAlgoAPI_Cut cut(Box(20.0, 20.0, 20.0, gp_Pnt(0, 0, 0)),
                            Box(10.0, 10.0, 10.0, gp_Pnt(5, 5, 5)));
        cut.Build();
        check(cut.IsDone(), "the cavity cuts cleanly");

        Body body(cut.Shape(), "Body1");
        checkCount(body.Lumps().size(), 1, "a hollow cube is ONE lump");
        checkCount(body.Shells().size(), 2, "with two shells -- outside and cavity");
        checkCount(body.FaceCount(), 12, "twelve faces");
        checkCount(body.EdgeCount(), 24, "twenty-four edges");
        checkCount(body.VertexCount(), 16, "sixteen vertices");
        checkCount(ExplorerUses(cut.Shape(), TopAbs_VERTEX), 96,
                   "where the explorer would offer ninety-six");
        checkNear(body.Volume(), 7000.0, 1.0e-6, "8000 outside less 1000 of cavity");
        checkNear(body.Area(), 3000.0, 1.0e-6, "2400 outside plus 600 of cavity wall");
    }

    // ---- 2. what the viewport is allowed to arm ----
    //
    // IsSelectable records the structural/pickable split once so that the
    // filter, the browser and the shell do not each guess at it.
    {
        check(!IsSelectable(EntityType::BRepLump), "a lump is structural, never picked");
        check(!IsSelectable(EntityType::BRepShell), "nor is a shell");
        check(!IsSelectable(EntityType::BRepLoop), "nor a loop");
        check(!IsSelectable(EntityType::BRepCoEdge), "nor a co-edge");
        check(!IsSelectable(EntityType::Unknown), "and nothing unknown is offered either");

        check(IsSelectable(EntityType::BRepBody), "bodies are pickable");
        check(IsSelectable(EntityType::BRepFace), "faces are pickable");
        check(IsSelectable(EntityType::BRepEdge), "edges are pickable");
        check(IsSelectable(EntityType::BRepVertex), "vertices are pickable");
        check(IsSelectable(EntityType::SketchLine), "so is a sketch curve, inside a sketch");

        // The filter list and the pickable split are two statements of the
        // same fact, in two files. A filter for something structural would
        // be a switch that can only ever arm nothing.
        const std::vector<EntityType>& filterable = GeometrySelection::FilterableTypes();
        checkCount(filterable.size(), 4, "four things a filter can stand for");
        bool allPickable = true;
        for (const EntityType type : filterable) {
            allPickable = allPickable && IsSelectable(type);
        }
        check(allPickable, "and every one of them is pickable");

        // The OCCT shape type each filter means, and the selection mode
        // MainWindow::applySelectionFilters hands to Activate for it.
        check(TopAbsTypeOf(EntityType::BRepFace) == TopAbs_FACE, "a face filter means TopAbs_FACE");
        check(TopAbsTypeOf(EntityType::BRepEdge) == TopAbs_EDGE, "an edge filter means TopAbs_EDGE");
        check(TopAbsTypeOf(EntityType::BRepVertex) == TopAbs_VERTEX,
              "a vertex filter means TopAbs_VERTEX");
        check(TopAbsTypeOf(EntityType::BRepBody) == TopAbs_SHAPE,
              "a body filter means the whole object");

        checkCount(static_cast<std::size_t>(AIS_Shape::SelectionMode(
                       TopAbsTypeOf(EntityType::BRepBody))), 0, "whole-object picking is mode 0");
        checkCount(static_cast<std::size_t>(AIS_Shape::SelectionMode(
                       TopAbsTypeOf(EntityType::BRepFace))), 4, "faces are mode 4");
        checkCount(static_cast<std::size_t>(AIS_Shape::SelectionMode(
                       TopAbsTypeOf(EntityType::BRepEdge))), 2, "edges are mode 2");
        checkCount(static_cast<std::size_t>(AIS_Shape::SelectionMode(
                       TopAbsTypeOf(EntityType::BRepVertex))), 1, "vertices are mode 1");

        // Mode 0 is also what the two-argument Display overload turns on by
        // itself, which is the fourth trap: decoration displayed without an
        // explicit -1 becomes a whole-object pick sitting in front of the
        // model. The call sites are not reachable from here (see the block
        // above main); the number they would arm is.
        check(TopAbsTypeOf(EntityType::BRepLoop) == TopAbs_SHAPE,
              "anything not a face, edge or vertex falls back to whole-object");
    }

    // ---- 3. GeometrySelection: the filter, and what is picked ----
    //
    // Note there is no Add or Toggle here, unlike ProfileSelection: the
    // viewport re-reads OCCT's whole selected set after every click and
    // hands it over in one Set(), so Ctrl+click accumulates inside OCCT
    // rather than in this list. Set REPLACING rather than appending is
    // therefore load-bearing, and is asserted below.
    {
        GeometrySelection& selection = GeometrySelection::Instance();

        check(selection.IsFilterOn(EntityType::BRepFace), "faces are pickable out of the box");
        check(selection.IsFilterOn(EntityType::BRepEdge), "so are edges");
        check(!selection.IsFilterOn(EntityType::BRepVertex), "vertices are not, until asked for");
        check(!selection.IsFilterOn(EntityType::BRepBody), "nor are whole bodies");

        // The generation counter is the shell's only cue to re-Activate
        // every displayed body, which is not free. It must move when the
        // filter really changed and stay put when it did not.
        const std::size_t start = selection.FilterGeneration();
        selection.SetFilter(EntityType::BRepVertex, true);
        check(selection.IsFilterOn(EntityType::BRepVertex), "vertices can be turned on");
        check(selection.FilterGeneration() != start, "and the viewport is told to re-arm");

        const std::size_t armed = selection.FilterGeneration();
        selection.SetFilter(EntityType::BRepVertex, true);
        checkCount(selection.FilterGeneration(), armed,
                   "turning an already-on filter on again re-arms nothing");

        selection.SetFilter(EntityType::BRepVertex, false);
        selection.SetFilter(EntityType::BRepEdge, false);
        checkCount(selection.Filters().size(), 1, "down to one filter");

        const std::size_t last = selection.FilterGeneration();
        selection.SetFilter(EntityType::BRepFace, false);
        checkCount(selection.Filters().size(), 1,
                   "the last filter cannot be turned off -- an inert viewport looks broken");
        checkCount(selection.FilterGeneration(), last,
                   "and refusing it does not re-arm anything either");

        selection.SetFilter(EntityType::BRepEdge, true);  // back to the default pair

        // Selection priority, Fusion's other tool, and the one the handoff
        // wanted when it called select.faces broken. The filters are
        // independent checkboxes; turning Faces off with the default pair
        // on leaves EDGES pickable, which is what that report saw.
        selection.SetFilter(EntityType::BRepFace, false);
        {
            const std::vector<EntityType> pickable = selection.PickableTypes();
            check(pickable.size() == 1 && pickable[0] == EntityType::BRepEdge,
                  "the report reproduced: Faces toggled OFF leaves edges alone pickable");
        }
        selection.SetFilter(EntityType::BRepFace, true);

        const std::size_t beforePriority = selection.FilterGeneration();
        selection.SetPriority(EntityType::BRepFace);
        {
            const std::vector<EntityType> pickable = selection.PickableTypes();
            check(pickable.size() == 1 && pickable[0] == EntityType::BRepFace,
                  "Face Priority: faces are the ONLY thing a click can pick");
        }
        check(selection.IsFilterOn(EntityType::BRepEdge),
              "without unticking the Edges filter underneath");
        check(selection.FilterGeneration() != beforePriority, "the viewport re-arms for it");

        selection.SetPriority(EntityType::BRepEdge);
        check(selection.HasPriority() && selection.Priority() == EntityType::BRepEdge,
              "only one priority at a time -- Edge replaces Face");

        selection.SetPriority(EntityType::BRepBody);
        check(selection.IsFilterOn(EntityType::BRepBody),
              "a priority ticks its own filter, as Fusion's does");
        selection.SetFilter(EntityType::BRepBody, false);  // restore the default pair
        check(!selection.HasPriority(), "changing a filter ends the priority");
        checkCount(selection.PickableTypes().size(), 2, "and the filters rule again");

        selection.SetPriority(EntityType::BRepFace);
        selection.ClearPriority();
        check(!selection.HasPriority(), "a priority turns off");
        checkCount(selection.PickableTypes().size(), 2, "leaving faces and edges, as before");

        Document doc;
        auto     box = std::make_shared<BoxFeature>(20.0, 20.0, 20.0);
        box->SetName("Box1");
        doc.AddFeature(box);
        check(doc.Errors().empty(), "a box builds");
        checkCount(doc.Bodies().size(), 1, "and is one body");

        const std::vector<GeometryRef> faces =
            CollectGeometryRefs(*doc.Bodies().front(), EntityType::BRepFace);
        const std::vector<GeometryRef> edges =
            CollectGeometryRefs(*doc.Bodies().front(), EntityType::BRepEdge);

        selection.Set({faces[0], faces[1]});
        checkCount(selection.Count(), 2, "two faces picked");
        selection.Set({faces[2]});
        checkCount(selection.Count(), 1,
                   "Set REPLACES -- the shell hands over the whole picked set, not a delta");

        check(!selection.SoleItem(EntityType::BRepFace).IsNull(), "one face is a sole item");
        check(selection.SoleItem(EntityType::BRepEdge).IsNull(), "no edge was picked");

        selection.Set({faces[0], edges[0], edges[1]});
        check(!selection.SoleItem(EntityType::BRepFace).IsNull(),
              "one face among three picks is still the sole face");
        check(selection.SoleItem(EntityType::BRepEdge).IsNull(),
              "two edges are not -- the caller must refuse rather than guess");

        selection.Clear();
        check(selection.IsEmpty(), "clearing empties it");
        check(selection.SoleItem(EntityType::BRepFace).IsNull(), "and leaves nothing sole");

        // A pick is traced back to the body that owns it. A shape from
        // outside the document belongs to no body, and saying so is what
        // stops a reference naming a body that never held it.
        GeometryRef traced;
        check(MakeGeometryRefIn(doc, doc.Bodies().front()->Faces().front(), traced),
              "a picked face is traced back to its body");
        checkText(traced.body, "Body1", "and names it");

        Body       stranger(Box(5.0, 5.0, 5.0, gp_Pnt(100, 100, 100)), "Elsewhere");
        check(!MakeGeometryRefIn(doc, stranger.Faces().front(), traced),
              "a face from a shape the document does not contain is refused");
    }

    // ---- 4. Ctrl+click, where adding and toggling actually live ----
    //
    // ProfileSelection is the selection in this app that owns the
    // additive bargain, so this is where it can be tested. Three regions
    // of one sketch, named by hand.
    {
        ProfileSelection& selection = ProfileSelection::Instance();
        selection.SetSketchName("Sketch1");
        selection.Clear();

        const ProfileRef left  = Region({1, 2, 3, 4}, -10.0, 0.0);
        const ProfileRef right = Region({5, 6, 7, 8}, 10.0, 0.0);

        selection.Toggle(left, false);
        checkCount(selection.Count(), 1, "a plain click picks one region");
        check(selection.Contains(left), "and it is the one clicked");

        selection.Toggle(right, true);
        checkCount(selection.Count(), 2, "Ctrl+click adds the second");
        check(selection.Contains(left) && selection.Contains(right), "and keeps both");

        selection.Toggle(right, true);
        checkCount(selection.Count(), 1, "Ctrl+clicking it again takes it back out");
        check(selection.Contains(left), "leaving the first alone");

        selection.Toggle(right, false);
        checkCount(selection.Count(), 1, "a plain click REPLACES rather than adds");
        check(selection.Contains(right) && !selection.Contains(left), "with what was clicked");

        selection.Toggle(ProfileRef(), true);
        checkCount(selection.Count(), 1, "Ctrl+click on empty space keeps the selection");
        selection.Toggle(ProfileRef(), false);
        check(selection.IsEmpty(), "a plain click on empty space clears it");

        // Two regions bounded by the same curves are different picks: the
        // seed is what tells them apart, exactly as in FindProfile. The
        // lens and lune of two overlapping circles are this case, and
        // toggling one must not toggle the other.
        const ProfileRef lens = Region({1, 2}, 6.0, 0.0);
        const ProfileRef lune = Region({1, 2}, -5.0, 0.0);
        selection.Toggle(lens, false);
        selection.Toggle(lune, true);
        checkCount(selection.Count(), 2, "same bounding ids, different seeds: two picks");
        selection.Toggle(lens, true);
        checkCount(selection.Count(), 1, "toggling the lens back out leaves the lune");
        check(selection.Contains(lune), "and it really is the lune that stayed");

        // A reference is only meaningful against the sketch whose entity
        // ids it names, so moving to another sketch drops the lot rather
        // than re-pointing ids 1 and 2 at whatever they mean there.
        selection.SetSketchName("Sketch2");
        check(selection.IsEmpty(), "switching sketch empties the selection");
        checkText(selection.SketchName(), "Sketch2", "and remembers which sketch it is on");

        selection.SetSketchName(std::string());
        selection.Clear();
    }

    // ---- 5. a pick that the rebuild invalidated is dropped, not reused ----
    //
    // The selection outlives the shape it was made against: every edit
    // re-evaluates the timeline and the faces come back as new objects.
    // A pick that no longer names anything has to go, because the
    // alternative is a tool quietly acting on the nearest face instead.
    //
    // A 20mm cube with a 5mm corner bitten out of it. Volume 8000 - 125 =
    // 7875, which is 1.6% down -- inside BodyTable's 2%, so the body keeps
    // its name and the geometry alone decides. The three faces meeting at
    // that corner go 400 -> 375; the three opposite them are untouched.
    {
        Document doc;
        auto     base = std::make_shared<BoxFeature>(20.0, 20.0, 20.0);
        base->SetName("Box1");
        doc.AddFeature(base);

        const std::vector<GeometryRef> picked =
            CollectGeometryRefs(*doc.Bodies().front(), EntityType::BRepFace);
        checkCount(picked.size(), 6, "all six faces picked");

        auto notch = std::make_shared<BoxFeature>(5.0, 5.0, 5.0);
        notch->SetName("Box2");
        notch->SetOperation(BooleanOp::Cut);
        doc.AddFeature(notch);
        check(doc.Errors().empty(), "the corner cut computes");
        checkNear(doc.Bodies().front()->Volume(), 7875.0, 1.0e-6, "8000 less a 125mm3 corner");
        checkText(doc.Bodies().front()->Name(), "Body1",
                  "the body keeps its name, so the name is not what drops the picks");

        const std::vector<GeometryRef> kept = SurvivorsIn(*doc.Bodies().front(), picked);
        checkCount(kept.size(), 3,
                   "three picks survive: the faces the cut never touched");
        for (const GeometryRef& ref : kept) {
            TopoDS_Shape found;
            check(ResolveGeometryRef(*doc.Bodies().front(), ref, found), "a survivor resolves");
            checkNear(FaceArea(found), 400.0, 1.0e-6, "to a face that is still the full 20 x 20");
        }

        // The other three are refused rather than re-pointed at the 375mm2
        // face standing where they used to be -- a 6.25% smaller face that
        // a press/pull would have acted on without a word.
        checkCount(picked.size() - kept.size(), 3, "and three are dropped");
        std::size_t bittenFaces = 0;
        for (const TopoDS_Face& face : doc.Bodies().front()->Faces()) {
            if (std::fabs(FaceArea(face) - 375.0) < 1.0e-6) {
                ++bittenFaces;
            }
        }
        checkCount(bittenFaces, 3, "the three bitten faces really are 375mm2 now");

        for (const GeometryRef& ref : picked) {
            bool survived = false;
            for (const GeometryRef& one : kept) {
                survived = survived || (one.Encode() == ref.Encode());
            }
            if (survived) {
                continue;
            }
            TopoDS_Shape found;
            check(!ResolveGeometryRefInShape(doc.Shape(), ref, found),
                  "a dropped pick finds nothing in the raw shape either -- no nearest-face fallback");
        }
    }

    // ---- 5b. an edit big enough to rename the body drops everything ----
    //
    // Taking a 20mm cube to 20 x 20 x 30 is a 50% change: BodyTable will
    // not call that the same body, so it is Body2 and every stored pick
    // names a body that no longer exists. Numbering never rewinds, which
    // is what makes that refusal safe to rely on.
    {
        Document doc;
        auto     base = std::make_shared<BoxFeature>(20.0, 20.0, 20.0);
        base->SetName("Box1");
        doc.AddFeature(base);
        const std::vector<GeometryRef> picked =
            CollectGeometryRefs(*doc.Bodies().front(), EntityType::BRepFace);

        base->SetParameter(Parameter::MakeDouble("Height", 30.0));
        doc.Rebuild();
        checkText(doc.Bodies().front()->Name(), "Body2", "the resized box is a NEW body");
        checkCount(SurvivorsIn(*doc.Bodies().front(), picked).size(), 0,
                   "so every pick made against Body1 is dropped on the name alone");

        // Against the raw shape, where the name cannot filter, geometry
        // still refuses five of the six: the top moved 10mm and the four
        // walls went from 400mm2 to 600mm2. Only the floor is untouched.
        std::size_t geometricSurvivors = 0;
        for (const GeometryRef& ref : picked) {
            TopoDS_Shape found;
            geometricSurvivors += ResolveGeometryRefInShape(doc.Shape(), ref, found) ? 1 : 0;
        }
        checkCount(geometricSurvivors, 1, "and only the floor survives on geometry alone");
    }

    // ---- 5c. a small edit keeps every pick ----
    //
    // The flip side, and the reason a reference is a signature rather than
    // an index: nudging the height 20 -> 20.2 moves the top face 0.2mm and
    // grows the walls by 1%, all inside tolerance. Dropping picks there
    // would make the selection useless for exactly the edits a parametric
    // model is for.
    {
        Document doc;
        auto     base = std::make_shared<BoxFeature>(20.0, 20.0, 20.0);
        base->SetName("Box1");
        doc.AddFeature(base);
        const std::vector<GeometryRef> picked =
            CollectGeometryRefs(*doc.Bodies().front(), EntityType::BRepFace);

        base->SetParameter(Parameter::MakeDouble("Height", 20.2));
        doc.Rebuild();
        checkText(doc.Bodies().front()->Name(), "Body1", "a 1% edit is the same body");
        checkCount(SurvivorsIn(*doc.Bodies().front(), picked).size(), 6,
                   "and all six picks follow the geometry that moved");
    }

    std::cout << std::endl;
    if (failures == 0) {
        std::cout << "ALL SELECTION TESTS PASSED" << std::endl;
        return 0;
    }
    std::cout << failures << " SELECTION TEST(S) FAILED" << std::endl;
    return 1;
}

// ---- what this suite does NOT cover, and why ----
//
// ARCHITECTURE.md lists four traps under "Viewport selection". Three of
// them are interactive, and a headless binary cannot reach them. Naming
// them here is the honest alternative to a mock that would assert nothing:
//
// TRAP 1 -- bodies must be Load()-ed, not just Activate()-d.
//   The whole symptom is inside AIS_InteractiveContext: Activate on an
//   unloaded object reports the mode as active, so every question the
//   context can answer says the body is armed. The only thing that tells
//   the two apart is whether the selector built primitives, and the only
//   way to ask that is to run a real detection -- MoveTo/Select against a
//   V3d_View, which needs a graphic driver and a display connection. This
//   harness links no TKV3d or TKOpenGl at all and runs with no window
//   server. The working guard is the screenshot script under "Seeing your
//   own work": an unloaded body is a click that selects nothing.
//
// TRAP 2 -- mousePressEvent must MoveTo at the press point first.
//   That call lives in OcctNativeWindow::mousePressEvent, a QWindow
//   subclass. Reproducing the bug means delivering a real QMouseEvent to a
//   mapped native window with a live context behind it, and then asking
//   that context what it detected. Qt's offscreen platform gives no native
//   window and no GL context. The case that exposed it -- a scripted
//   click, which arrives with no cursor movement before it -- is
//   reproducible only through --script, not from here.
//
// TRAP 3 -- SelectInViewer must treat a degenerate rectangle as a point.
//   The classification (bounding box of the point sequence, then a
//   two-pixel slop) is written inline inside
//   OcctNativeWindow::SelectInViewer and is not exposed as a function, so
//   a headless test has nothing to call; and the branch it guards is
//   MoveTo/SelectDetected/Redraw, all of which need a live view. Lifting
//   the click-versus-drag test into a free function in core would make
//   that half testable -- that is an edit to src/OcctViewport.*, which the
//   author of this suite does not own.
//
// TRAP 4 -- decoration must be displayed with selection mode -1.
//   Half covered. Section 2 pins the mode numbers, including the mode 0
//   that the two-argument Display overload turns on by itself and that
//   made the origin axes steal clicks. What is NOT covered is the call
//   sites: that every decorative Display in OcctViewport really passes -1.
//   Checking that needs either the AIS context (the wall trap 1 hits) or a
//   grep of the source, which would go green the moment someone wrapped
//   the line differently.
