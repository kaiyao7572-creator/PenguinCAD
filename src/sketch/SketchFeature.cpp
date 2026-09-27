#include "sketch/SketchFeature.h"

#include "core/Origin.h"
#include "sketch/SketchGeometry.h"
#include "sketch/SketchProfiles.h"

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRep_Builder.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Dir.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <cmath>

namespace lcad {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Sketch coordinates are millimetres, so a micron is a safe "same point"
// threshold: far tighter than anything a user can click, loose enough to
// absorb the float noise two different tools produce for one corner.
constexpr double kNodeTolerance = SketchGeometry::kTolerance;

constexpr std::size_t kNoSegment = static_cast<std::size_t>(-1);

double SquaredNodeTolerance()
{
    return kNodeTolerance * kNodeTolerance;
}

// Shoelace area of the loop's node polygon. Arcs are measured by their
// chord, which is rough but only ever used to decide which loop is the
// outer boundary.
double PolygonArea(const std::vector<gp_Pnt2d>& theNodes,
                   const std::vector<std::size_t>& theLoop)
{
    if (theLoop.size() < 3) {
        return 0.0;
    }

    double twiceArea = 0.0;
    for (std::size_t i = 0; i < theLoop.size(); ++i) {
        const gp_Pnt2d& a = theNodes[theLoop[i]];
        const gp_Pnt2d& b = theNodes[theLoop[(i + 1) % theLoop.size()]];
        twiceArea += a.X() * b.Y() - b.X() * a.Y();
    }
    return std::fabs(twiceArea) * 0.5;
}

// Area of a curve that closes on itself. Only used for ranking loops, so
// a sampled polygon is plenty for the shapes with no closed form.
double SelfClosedArea(const SketchEntity& theEntity)
{
    switch (theEntity.kind) {
        case SketchEntity::Kind::Circle:
        case SketchEntity::Kind::Arc:
            return kPi * theEntity.radius * theEntity.radius;

        case SketchEntity::Kind::Ellipse:
            return kPi * theEntity.radius * theEntity.minorRadius;

        case SketchEntity::Kind::Conic:
            return 0.0;  // three points and a rho never close a loop

        case SketchEntity::Kind::Spline:
        case SketchEntity::Kind::ControlPointSpline: {
            double first = 0.0, last = 0.0;
            if (!SketchGeometry::ParamRange(theEntity, first, last)) {
                return 0.0;
            }
            constexpr int kSamples = 64;
            double twiceArea = 0.0;
            gp_Pnt2d previous = SketchGeometry::PointAt(theEntity, first);
            for (int i = 1; i <= kSamples; ++i) {
                const double param = first + (last - first) * i / kSamples;
                const gp_Pnt2d current = SketchGeometry::PointAt(theEntity, param);
                twiceArea += previous.X() * current.Y() - current.X() * previous.Y();
                previous = current;
            }
            return std::fabs(twiceArea) * 0.5;
        }

        case SketchEntity::Kind::Line:
        case SketchEntity::Kind::Point:
            return 0.0;
    }
    return 0.0;
}

// Entities that can bound a face: construction geometry and bare points
// are guides, not profile boundaries.
bool ContributesToProfile(const SketchEntity& theEntity)
{
    return theEntity.IsCurve() && !theEntity.isConstruction && !theEntity.IsDegenerate();
}

} // namespace

// ---- SketchFeature ----

SketchFeature::SketchFeature()
    : myPlanePosition(PlaneXY())
{
}

SketchFeature::SketchFeature(const gp_Ax3& thePlanePosition, double theOffset)
    : myOffset(theOffset)
{
    SetPlanePosition(thePlanePosition);
}

bool SketchFeature::Compute(const ComputeContext& theContext,
                            const TopoDS_Shape&   theInput,
                            TopoDS_Shape&         theOutput,
                            std::string&          theError)
{
    (void)theContext;

    // A sketch contributes no solid of its own -- it only feeds profiles
    // to the features downstream of it, which reach it through
    // ProfileProvider. Set the passthrough first so a failed solve still
    // carries the model forward.
    theOutput = theInput;

    if (myNeedsSolve && !myConstraints.empty()) {
        std::string error;
        if (!Solve(error)) {
            theError = error;
            return false;
        }
    }
    return true;
}

std::unique_ptr<Feature> SketchFeature::Clone() const
{
    auto copy = std::make_unique<SketchFeature>(myPlanePosition, myOffset);
    // Entities and constraints are values, so the vector copies are
    // already the deep copy undo needs.
    copy->myEntities = myEntities;
    copy->myConstraints = myConstraints;
    copy->myIsVisible = myIsVisible;
    copy->myNextEntityId = myNextEntityId;
    copy->myNextConstraintId = myNextConstraintId;
    copy->myNextDimension = myNextDimension;
    copy->myNeedsSolve = myNeedsSolve;
    copy->mySolverError = mySolverError;
    CopyBaseTo(*copy);
    return copy;
}

std::vector<Parameter> SketchFeature::Parameters() const
{
    std::vector<Parameter> parameters;
    parameters.push_back(Parameter::MakeDouble("Offset", myOffset));
    parameters.push_back(Parameter::MakeBool("Visible", myIsVisible));

    // Driving dimensions are the sketch's real parameters: typing a new
    // value here is what makes the geometry move.
    for (const SketchConstraint& constraint : myConstraints) {
        if (!constraint.IsDimension()) {
            continue;
        }
        const std::string name =
            constraint.label.empty() ? SketchConstraint::TypeName(constraint.type)
                                     : constraint.label;
        if (constraint.IsAngular()) {
            parameters.push_back(
                Parameter::MakeDouble(name, constraint.value * 180.0 / kPi, "deg"));
        } else {
            parameters.push_back(Parameter::MakeDouble(name, constraint.value));
        }
    }
    return parameters;
}

bool SketchFeature::SetParameter(const Parameter& theParameter)
{
    if (theParameter.name == "Offset") {
        myOffset = theParameter.doubleValue;
        return true;
    }
    if (theParameter.name == "Visible") {
        myIsVisible = theParameter.boolValue;
        return true;
    }

    for (SketchConstraint& constraint : myConstraints) {
        if (!constraint.IsDimension() || constraint.label != theParameter.name) {
            continue;
        }
        constraint.value = constraint.IsAngular() ? theParameter.doubleValue * kPi / 180.0
                                                  : theParameter.doubleValue;
        myNeedsSolve = true;
        return true;
    }
    return false;
}

void SketchFeature::SetPlanePosition(const gp_Ax3& thePosition)
{
    // Force a right-handed frame. 2D sketch coordinates assume
    // Y = normal x X, which is what gp_Ax2 -- and therefore every circle
    // and arc built below -- uses; an indirect frame would mirror them.
    myPlanePosition = gp_Ax3(thePosition.Location(),
                             thePosition.Direction(),
                             thePosition.XDirection());
}

gp_Ax3 SketchFeature::Position() const
{
    gp_Ax3 position = myPlanePosition;
    if (myOffset != 0.0) {
        position.SetLocation(
            position.Location().Translated(gp_Vec(position.Direction()) * myOffset));
    }
    return position;
}

gp_Pln SketchFeature::Plane() const
{
    return gp_Pln(Position());
}

gp_Pnt SketchFeature::To3d(const gp_Pnt2d& thePoint) const
{
    const gp_Ax3 position = Position();
    const gp_Vec offset = gp_Vec(position.XDirection()) * thePoint.X()
                        + gp_Vec(position.YDirection()) * thePoint.Y();
    return position.Location().Translated(offset);
}

gp_Pnt2d SketchFeature::To2d(const gp_Pnt& thePoint) const
{
    const gp_Ax3 position = Position();
    const gp_Vec delta(position.Location(), thePoint);
    return gp_Pnt2d(delta.Dot(gp_Vec(position.XDirection())),
                    delta.Dot(gp_Vec(position.YDirection())));
}

// The origin planes are defined once, in core/Origin.cpp, and read back
// here. They were duplicated for a while and the two copies agreed --
// which is luck, not a guarantee. A sketch drawn on "XZ" and a
// construction plane offset from "XZ" have to mean the same plane, and
// the only way to be sure is for there to be one of it.
//
// XZ's normal is -Y so that sketch (u, v) reads as world (X, Z), which is
// how a front view is normally set up.
namespace {

gp_Ax3 OriginPlane(const char* theName)
{
    const OriginEntity* entity = FindOriginEntity(theName);
    return entity != nullptr ? entity->plane : gp_Ax3();
}

} // namespace

gp_Ax3 SketchFeature::PlaneXY()
{
    return OriginPlane("XY");
}

gp_Ax3 SketchFeature::PlaneXZ()
{
    return OriginPlane("XZ");
}

gp_Ax3 SketchFeature::PlaneYZ()
{
    return OriginPlane("YZ");
}

int SketchFeature::AddEntity(const SketchEntity& theEntity)
{
    SketchEntity entity = theEntity;
    entity.id = myNextEntityId++;
    myEntities.push_back(entity);
    return entity.id;
}

std::vector<int> SketchFeature::AddEntities(const std::vector<SketchEntity>& theEntities)
{
    std::vector<int> ids;
    ids.reserve(theEntities.size());
    for (const SketchEntity& entity : theEntities) {
        ids.push_back(AddEntity(entity));
    }
    return ids;
}

bool SketchFeature::ReplaceEntity(int theId, const SketchEntity& theEntity)
{
    for (SketchEntity& entity : myEntities) {
        if (entity.id != theId) {
            continue;
        }
        const int id = entity.id;
        entity = theEntity;
        entity.id = id;  // constraints point at the id; it has to survive
        myNeedsSolve = true;
        return true;
    }
    return false;
}

bool SketchFeature::RemoveEntity(int theId)
{
    const auto found = std::find_if(myEntities.begin(), myEntities.end(),
                                    [theId](const SketchEntity& theEntity) {
                                        return theEntity.id == theId;
                                    });
    if (found == myEntities.end()) {
        return false;
    }
    myEntities.erase(found);

    // A constraint pointing at geometry that no longer exists would stop
    // the whole system from solving, so it goes with the entity.
    myConstraints.erase(
        std::remove_if(myConstraints.begin(), myConstraints.end(),
                       [theId](const SketchConstraint& theConstraint) {
                           return theConstraint.a.entity == theId
                               || theConstraint.b.entity == theId
                               || theConstraint.c.entity == theId;
                       }),
        myConstraints.end());
    return true;
}

const SketchEntity* SketchFeature::FindEntity(int theId) const
{
    for (const SketchEntity& entity : myEntities) {
        if (entity.id == theId) {
            return &entity;
        }
    }
    return nullptr;
}

SketchEntity* SketchFeature::FindEntity(int theId)
{
    for (SketchEntity& entity : myEntities) {
        if (entity.id == theId) {
            return &entity;
        }
    }
    return nullptr;
}

void SketchFeature::AddRectangle(const gp_Pnt2d& theCorner, const gp_Pnt2d& theOpposite)
{
    AddEntities(SketchGeometry::RectangleTwoPoint(theCorner, theOpposite));
}

void SketchFeature::RemoveLastEntity()
{
    if (myEntities.empty()) {
        return;
    }
    RemoveEntity(myEntities.back().id);
}

void SketchFeature::ClearEntities()
{
    myEntities.clear();
    myConstraints.clear();
}

std::vector<gp_Pnt2d> SketchFeature::EndPoints() const
{
    std::vector<gp_Pnt2d> points;
    points.reserve(myEntities.size() * 2);
    for (const SketchEntity& entity : myEntities) {
        if (entity.IsSelfClosed() || !entity.IsCurve()) {
            continue;
        }
        points.push_back(entity.StartPoint());
        points.push_back(entity.EndPoint());
    }
    return points;
}

std::vector<gp_Pnt2d> SketchFeature::SnapPoints() const
{
    std::vector<gp_Pnt2d> points;
    points.reserve(myEntities.size() * 4 + 1);

    // The sketch origin is always snappable. Anchoring geometry to it is
    // how a sketch stays put when it is later dimensioned, and Fusion
    // snaps to it from the very first click of an empty sketch.
    points.emplace_back(0.0, 0.0);

    for (const SketchEntity& entity : myEntities) {
        if (!entity.IsCurve()) {
            points.push_back(entity.first);
            continue;
        }
        if (!entity.IsSelfClosed()) {
            points.push_back(entity.StartPoint());
            points.push_back(entity.EndPoint());
        }
        switch (entity.kind) {
            case SketchEntity::Kind::Circle:
            case SketchEntity::Kind::Arc:
            case SketchEntity::Kind::Ellipse:
                points.push_back(entity.first);
                break;
            case SketchEntity::Kind::Line: {
                // Midpoint: heavily used in Fusion for centring and
                // symmetry, and cheap to offer.
                const gp_Pnt2d start = entity.StartPoint();
                const gp_Pnt2d end = entity.EndPoint();
                points.emplace_back(0.5 * (start.X() + end.X()), 0.5 * (start.Y() + end.Y()));
                break;
            }
            case SketchEntity::Kind::Spline:
            case SketchEntity::Kind::ControlPointSpline:
            case SketchEntity::Kind::Conic:
            case SketchEntity::Kind::Point:
                break;
        }
    }
    return points;
}

// ---- constraints ----

int SketchFeature::AddConstraint(const SketchConstraint& theConstraint)
{
    SketchConstraint constraint = theConstraint;
    constraint.id = myNextConstraintId++;
    if (constraint.IsDimension() && constraint.label.empty()) {
        constraint.label = "d" + std::to_string(myNextDimension++);
    }
    myConstraints.push_back(constraint);
    myNeedsSolve = true;
    return constraint.id;
}

bool SketchFeature::RenameParameter(const std::string& theOldName, const std::string& theNewName)
{
    SketchConstraint* target = nullptr;
    for (SketchConstraint& constraint : myConstraints) {
        if (!constraint.IsDimension()) {
            continue;
        }
        if (constraint.label == theNewName) {
            return false;   // two dimensions answering to one name
        }
        if (constraint.label == theOldName) {
            target = &constraint;
        }
    }
    if (target == nullptr) {
        return false;
    }
    target->label = theNewName;
    return true;
}

bool SketchFeature::RemoveConstraint(int theId)
{
    const auto found = std::find_if(myConstraints.begin(), myConstraints.end(),
                                    [theId](const SketchConstraint& theConstraint) {
                                        return theConstraint.id == theId;
                                    });
    if (found == myConstraints.end()) {
        return false;
    }
    myConstraints.erase(found);
    myNeedsSolve = true;
    return true;
}

void SketchFeature::ClearConstraints()
{
    myConstraints.clear();
    myNeedsSolve = false;
    mySolverError.clear();
}

const SketchConstraint* SketchFeature::FindConstraint(int theId) const
{
    for (const SketchConstraint& constraint : myConstraints) {
        if (constraint.id == theId) {
            return &constraint;
        }
    }
    return nullptr;
}

bool SketchFeature::HasConstraint(const SketchConstraint& theConstraint) const
{
    for (const SketchConstraint& existing : myConstraints) {
        if (existing.type != theConstraint.type) {
            continue;
        }
        // Relations are symmetric, so A-B and B-A are the same constraint
        // and applying it twice would only over-constrain the sketch.
        const bool sameOrder = existing.a == theConstraint.a && existing.b == theConstraint.b;
        const bool swapped = existing.a == theConstraint.b && existing.b == theConstraint.a;
        if ((sameOrder || swapped) && existing.c == theConstraint.c) {
            return true;
        }
    }
    return false;
}

bool SketchFeature::Restore(const gp_Ax3&                 thePlanePosition,
                            std::vector<SketchEntity>     theEntities,
                            std::vector<SketchConstraint> theConstraints,
                            int                           theNextEntityId,
                            int                           theNextConstraintId,
                            int                           theNextDimension,
                            std::string&                  theError)
{
    if (!thePlanePosition.Direct()) {
        // Every 2D-to-3D mapping here assumes Y = normal x X; a mirrored
        // frame would draw every curve reflected.
        theError = "the sketch plane is a left-handed frame";
        return false;
    }

    const auto checkIds = [&theError](const std::vector<int>& theIds, int theNext,
                                      const char* theWhat) {
        std::vector<int> sorted = theIds;
        std::sort(sorted.begin(), sorted.end());
        for (std::size_t i = 0; i < sorted.size(); ++i) {
            if (sorted[i] <= 0 || (i > 0 && sorted[i] == sorted[i - 1])) {
                theError = std::string(theWhat) + " id " + std::to_string(sorted[i])
                           + (sorted[i] <= 0 ? " is not positive" : " is used twice");
                return false;
            }
        }
        if (!sorted.empty() && theNext <= sorted.back()) {
            theError = std::string("the next ") + theWhat + " id (" + std::to_string(theNext)
                       + ") is not past the ones in use";
            return false;
        }
        return true;
    };

    std::vector<int> entityIds;
    for (const SketchEntity& entity : theEntities) {
        entityIds.push_back(entity.id);
    }
    std::vector<int> constraintIds;
    for (const SketchConstraint& constraint : theConstraints) {
        constraintIds.push_back(constraint.id);
    }
    if (!checkIds(entityIds, theNextEntityId, "curve")
        || !checkIds(constraintIds, theNextConstraintId, "constraint")) {
        return false;
    }
    if (theNextDimension < 1) {
        theError = "the next dimension number is not positive";
        return false;
    }

    // A constraint on a curve that is not there would quietly stop the
    // solver (RemoveEntity drops them for exactly that reason), and two
    // dimensions sharing a label would be one parameter row that edits
    // only the first of them.
    std::sort(entityIds.begin(), entityIds.end());
    std::vector<std::string> labels;
    for (const SketchConstraint& constraint : theConstraints) {
        for (const SketchPointRef* operand : {&constraint.a, &constraint.b, &constraint.c}) {
            if (operand->entity != 0
                && !std::binary_search(entityIds.begin(), entityIds.end(), operand->entity)) {
                theError = "constraint " + std::to_string(constraint.id) + " names curve "
                           + std::to_string(operand->entity) + ", which the sketch does not have";
                return false;
            }
        }
        if (constraint.IsDimension() && !constraint.label.empty()) {
            if (std::find(labels.begin(), labels.end(), constraint.label) != labels.end()) {
                theError = "two dimensions are both called " + constraint.label;
                return false;
            }
            labels.push_back(constraint.label);
        }
    }

    myPlanePosition = thePlanePosition;
    myEntities = std::move(theEntities);
    myConstraints = std::move(theConstraints);
    myNextEntityId = theNextEntityId;
    myNextConstraintId = theNextConstraintId;
    myNextDimension = theNextDimension;
    mySolverError.clear();
    myNeedsSolve = !myConstraints.empty() && !SketchSolver::IsSatisfied(myEntities, myConstraints);
    return true;
}

bool SketchFeature::Solve(std::string& theError)
{
    const SketchSolver::Result result = SketchSolver::Solve(myEntities, myConstraints);
    myNeedsSolve = false;
    mySolverError = result.error;
    if (!result.converged) {
        theError = result.error.empty() ? "sketch constraints could not be satisfied"
                                        : result.error;
        return false;
    }
    return true;
}

double SketchFeature::ConstraintResidual() const
{
    return SketchSolver::Residual(myEntities, myConstraints);
}

// ---- geometry ----

TopoDS_Edge SketchFeature::BuildEdge(const SketchEntity& theEntity) const
{
    TopoDS_Edge edge;
    if (!theEntity.IsCurve() || theEntity.IsDegenerate()) {
        return edge;
    }

    try {
        const Handle(Geom2d_Curve) curve2d = SketchGeometry::Curve2dOf(theEntity);
        double first = 0.0, last = 0.0;
        if (curve2d.IsNull() || !SketchGeometry::ParamRange(theEntity, first, last)) {
            return edge;
        }

        const Handle(Geom_Curve) curve3d = SketchGeometry::To3dCurve(curve2d, Position());
        if (curve3d.IsNull()) {
            return edge;
        }

        BRepBuilderAPI_MakeEdge maker(curve3d, first, last);
        if (maker.IsDone()) {
            edge = maker.Edge();
        }
    } catch (const Standard_Failure&) {
        edge = TopoDS_Edge();
    }
    return edge;
}

TopoDS_Shape SketchFeature::BuildCompound(const std::vector<SketchEntity>& theEntities) const
{
    TopoDS_Compound compound;
    BRep_Builder builder;
    builder.MakeCompound(compound);

    bool any = false;
    for (const SketchEntity& entity : theEntities) {
        if (!entity.IsCurve()) {
            // A bare point still has to show up, and a vertex is the only
            // thing an AIS_Shape can draw for it.
            try {
                BRepBuilderAPI_MakeVertex maker(To3d(entity.first));
                if (maker.IsDone()) {
                    builder.Add(compound, maker.Vertex());
                    any = true;
                }
            } catch (const Standard_Failure&) {
                // a point that can't be built simply isn't drawn
            }
            continue;
        }

        const TopoDS_Edge edge = BuildEdge(entity);
        if (!edge.IsNull()) {
            builder.Add(compound, edge);
            any = true;
        }
    }

    return any ? TopoDS_Shape(compound) : TopoDS_Shape();
}

TopoDS_Shape SketchFeature::ConstructionCompound() const
{
    std::vector<SketchEntity> selected;
    for (const SketchEntity& entity : myEntities) {
        if (entity.isConstruction && entity.IsCurve()) {
            selected.push_back(entity);
        }
    }
    return BuildCompound(selected);
}

TopoDS_Shape SketchFeature::ProfileCompound() const
{
    std::vector<SketchEntity> selected;
    for (const SketchEntity& entity : myEntities) {
        if (!entity.isConstruction && entity.IsCurve()) {
            selected.push_back(entity);
        }
    }
    return BuildCompound(selected);
}

TopoDS_Shape SketchFeature::PointCompound() const
{
    std::vector<SketchEntity> selected;
    for (const SketchEntity& entity : myEntities) {
        if (!entity.IsCurve()) {
            selected.push_back(entity);
        }
    }
    return BuildCompound(selected);
}

std::vector<TopoDS_Wire> SketchFeature::ProfileWires() const
{
    std::vector<TopoDS_Wire> wires;
    if (myEntities.empty()) {
        return wires;
    }

    struct Loop
    {
        TopoDS_Wire wire;
        double      area = 0.0;
    };
    std::vector<Loop> loops;

    try {
        // Nodes are shared endpoints; every non-closed entity becomes a
        // segment between two of them, and closed loops fall out of
        // walking that little graph.
        std::vector<gp_Pnt2d> nodes;
        auto nodeIndex = [&nodes](const gp_Pnt2d& thePoint) -> std::size_t {
            for (std::size_t i = 0; i < nodes.size(); ++i) {
                if (nodes[i].SquareDistance(thePoint) <= SquaredNodeTolerance()) {
                    return i;
                }
            }
            nodes.push_back(thePoint);
            return nodes.size() - 1;
        };

        struct Segment
        {
            std::size_t entity = 0;
            std::size_t a = 0;
            std::size_t b = 0;
        };
        std::vector<Segment> segments;

        for (std::size_t i = 0; i < myEntities.size(); ++i) {
            const SketchEntity& entity = myEntities[i];
            if (!ContributesToProfile(entity)) {
                continue;
            }

            if (entity.IsSelfClosed()) {
                // Circles, whole ellipses and closed splines are already a
                // loop; no chaining needed.
                const TopoDS_Edge edge = BuildEdge(entity);
                if (edge.IsNull()) {
                    continue;
                }
                BRepBuilderAPI_MakeWire wireMaker(edge);
                if (!wireMaker.IsDone()) {
                    continue;
                }
                Loop loop;
                loop.wire = wireMaker.Wire();
                loop.wire.Closed(Standard_True);
                loop.area = SelfClosedArea(entity);
                loops.push_back(loop);
                continue;
            }

            Segment segment;
            segment.entity = i;
            segment.a = nodeIndex(entity.StartPoint());
            segment.b = nodeIndex(entity.EndPoint());
            if (segment.a == segment.b) {
                continue;  // degenerate: zero-length line or full-turn arc
            }
            segments.push_back(segment);
        }

        if (!segments.empty()) {
            std::vector<TopoDS_Vertex> vertices(nodes.size());
            for (std::size_t i = 0; i < nodes.size(); ++i) {
                BRepBuilderAPI_MakeVertex maker(To3d(nodes[i]));
                if (maker.IsDone()) {
                    vertices[i] = maker.Vertex();
                }
            }

            // Edges are built on those shared vertices on purpose: edges
            // made from bare points each get their own vertex, and
            // MakeWire then refuses to join them into a closed wire even
            // when the coordinates match.
            const gp_Ax3 position = Position();
            std::vector<TopoDS_Edge> edges(segments.size());
            for (std::size_t i = 0; i < segments.size(); ++i) {
                const SketchEntity& entity = myEntities[segments[i].entity];
                const TopoDS_Vertex& va = vertices[segments[i].a];
                const TopoDS_Vertex& vb = vertices[segments[i].b];
                if (va.IsNull() || vb.IsNull()) {
                    continue;
                }

                const Handle(Geom2d_Curve) curve2d = SketchGeometry::Curve2dOf(entity);
                double first = 0.0, last = 0.0;
                if (curve2d.IsNull() || !SketchGeometry::ParamRange(entity, first, last)) {
                    continue;
                }
                const Handle(Geom_Curve) curve3d =
                    SketchGeometry::To3dCurve(curve2d, position);
                if (curve3d.IsNull()) {
                    continue;
                }

                BRepBuilderAPI_MakeEdge maker(curve3d, va, vb, first, last);
                if (maker.IsDone()) {
                    edges[i] = maker.Edge();
                }
            }

            std::vector<bool> used(segments.size(), false);
            for (std::size_t i = 0; i < segments.size(); ++i) {
                if (edges[i].IsNull()) {
                    used[i] = true;
                }
            }

            for (std::size_t seed = 0; seed < segments.size(); ++seed) {
                if (used[seed]) {
                    continue;
                }

                std::vector<std::size_t> chain;
                std::vector<std::size_t> visited;
                const std::size_t startNode = segments[seed].a;
                std::size_t current = segments[seed].b;
                used[seed] = true;
                chain.push_back(seed);
                visited.push_back(startNode);

                while (current != startNode) {
                    visited.push_back(current);

                    std::size_t next = kNoSegment;
                    for (std::size_t j = 0; j < segments.size(); ++j) {
                        if (used[j] || (segments[j].a != current && segments[j].b != current)) {
                            continue;
                        }
                        next = j;
                        const std::size_t far =
                            (segments[j].a == current) ? segments[j].b : segments[j].a;
                        if (far == startNode) {
                            break;  // at a branch, prefer the edge that closes the loop
                        }
                    }
                    if (next == kNoSegment) {
                        break;
                    }

                    used[next] = true;
                    chain.push_back(next);
                    current = (segments[next].a == current) ? segments[next].b : segments[next].a;
                }

                if (current != startNode || chain.size() < 2) {
                    continue;  // open chain -- not a profile
                }

                BRepBuilderAPI_MakeWire wireMaker;
                for (const std::size_t index : chain) {
                    wireMaker.Add(edges[index]);
                }
                if (!wireMaker.IsDone()) {
                    continue;
                }

                Loop loop;
                loop.wire = wireMaker.Wire();
                loop.wire.Closed(Standard_True);
                loop.area = PolygonArea(nodes, visited);
                loops.push_back(loop);
            }
        }
    } catch (const Standard_Failure&) {
        return wires;
    }

    // Largest loop first: ProfileFaces() takes the first wire as the outer
    // boundary and punches the rest out as holes.
    std::stable_sort(loops.begin(), loops.end(),
                     [](const Loop& theLeft, const Loop& theRight) {
                         return theLeft.area > theRight.area;
                     });

    wires.reserve(loops.size());
    for (const Loop& loop : loops) {
        wires.push_back(loop.wire);
    }
    return wires;
}

std::vector<ProfileRegion> SketchFeature::ProfileRegions() const
{
    return ComputeProfileRegions(*this);
}

std::vector<TopoDS_Face> SketchFeature::ProfileFaces() const
{
    std::vector<TopoDS_Face> faces;
    for (const ProfileRegion& region : ProfileRegions()) {
        if (!region.face.IsNull()) {
            faces.push_back(region.face);
        }
    }
    return faces;
}

} // namespace lcad
