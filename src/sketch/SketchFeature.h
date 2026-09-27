#pragma once

#include "core/Feature.h"
#include "core/ProfileProvider.h"
#include "sketch/SketchConstraints.h"
#include "sketch/SketchEntity.h"

#include <TopoDS_Edge.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Ax3.hxx>
#include <gp_Pnt.hxx>
#include <gp_Pnt2d.hxx>

#include <string>
#include <vector>

namespace lcad {

// A 2D sketch on a plane: one timeline entry that contributes no solid,
// but hands its closed regions to the features downstream through
// ProfileProvider.
//
// The sketch owns three things -- a plane, a list of entities, and the
// constraints binding them. Solving happens inside the feature so an undo
// or a parameter edit re-establishes the same geometry the user saw.
class SketchFeature : public Feature, public ProfileProvider
{
public:
    SketchFeature();
    explicit SketchFeature(const gp_Ax3& thePlanePosition, double theOffset = 0.0);

    std::string TypeName() const override { return "Sketch"; }

    bool Compute(const ComputeContext& theContext,
                 const TopoDS_Shape&   theInput,
                 TopoDS_Shape&         theOutput,
                 std::string&          theError) override;

    std::unique_ptr<Feature> Clone() const override;

    std::vector<Parameter> Parameters() const override;
    bool SetParameter(const Parameter& theParameter) override;

    // ---- plane ----

    // The plane as picked, before the offset is applied.
    const gp_Ax3& PlanePosition() const { return myPlanePosition; }
    void SetPlanePosition(const gp_Ax3& thePosition);

    double Offset() const { return myOffset; }
    void SetOffset(double theOffset) { myOffset = theOffset; }

    // Plane the entities actually live on: the base plane pushed along
    // its own normal by the offset.
    gp_Ax3 Position() const;
    gp_Pln Plane() const;

    gp_Pnt   To3d(const gp_Pnt2d& thePoint) const;
    gp_Pnt2d To2d(const gp_Pnt& thePoint) const;

    // Standard construction planes, origin-centred.
    static gp_Ax3 PlaneXY();
    static gp_Ax3 PlaneXZ();
    static gp_Ax3 PlaneYZ();

    // ---- entities ----

    const std::vector<SketchEntity>& Entities() const { return myEntities; }

    // Returns the id the sketch assigned, which is how constraints and
    // the selection refer to the entity from then on.
    int AddEntity(const SketchEntity& theEntity);
    std::vector<int> AddEntities(const std::vector<SketchEntity>& theEntities);

    // Replace an entity's geometry, keeping its id and its constraints.
    bool ReplaceEntity(int theId, const SketchEntity& theEntity);

    // Removes the entity and every constraint that referenced it -- a
    // dangling constraint would silently stop the solver.
    bool RemoveEntity(int theId);

    const SketchEntity* FindEntity(int theId) const;
    SketchEntity*       FindEntity(int theId);

    // Stored as four lines rather than its own kind, so the wire
    // assembler has one less special case to reason about.
    void AddRectangle(const gp_Pnt2d& theCorner, const gp_Pnt2d& theOpposite);

    void RemoveLastEntity();
    void ClearEntities();

    // Endpoints of every entity, in sketch coordinates. Tools snap new
    // points onto these so hand-drawn loops actually close.
    std::vector<gp_Pnt2d> EndPoints() const;

    // Everything worth snapping to: endpoints plus the centres of the
    // curves that have one, plus standalone sketch points.
    std::vector<gp_Pnt2d> SnapPoints() const;

    // ---- constraints ----

    const std::vector<SketchConstraint>& Constraints() const { return myConstraints; }

    // Adds the constraint and marks the sketch for re-solving. Dimensions
    // get their label ("d1", "d2"...) assigned here.
    int AddConstraint(const SketchConstraint& theConstraint);

    bool RemoveConstraint(int theId);

    // A dimension's label IS its model parameter name (d1, d2 ...); the
    // document renames one when two sketches would both have a d1, or when
    // the user renames it in Change Parameters.
    bool RenameParameter(const std::string& theOldName, const std::string& theNewName) override;
    void ClearConstraints();

    const SketchConstraint* FindConstraint(int theId) const;

    // True when some entity already carries this exact relation, so a
    // second click on the same pair doesn't pile duplicates up.
    bool HasConstraint(const SketchConstraint& theConstraint) const;

    // Run the solver now. Returns false and fills theError when the
    // system could not be satisfied; the geometry is still left at the
    // closest fit found so the user can see what went wrong.
    bool Solve(std::string& theError);

    // Ask for a solve on the next compute. Tools call this after they
    // change geometry a constraint depends on.
    void Invalidate() { myNeedsSolve = true; }

    // How far the sketch is from satisfying its constraints right now.
    double ConstraintResidual() const;

    // ---- geometry ----

    // 3D edge for one entity; null if the entity is degenerate or is a
    // bare sketch point.
    TopoDS_Edge BuildEdge(const SketchEntity& theEntity) const;

    // Compound of edges for an arbitrary entity list -- used for both the
    // sketch display and the rubber-band preview. Null when empty.
    TopoDS_Shape BuildCompound(const std::vector<SketchEntity>& theEntities) const;
    TopoDS_Shape EdgeCompound() const { return BuildCompound(myEntities); }

    // Construction geometry drawn on its own, so the display can grey it
    // out the way Fusion does instead of mixing it with real profile
    // curves.
    TopoDS_Shape ConstructionCompound() const;
    TopoDS_Shape ProfileCompound() const;

    // Vertices for standalone sketch points, which have no edge of their
    // own but still need to be visible.
    TopoDS_Shape PointCompound() const;

    // ---- ProfileProvider ----

    gp_Pln ProfilePlane() const override { return Plane(); }

    // Closed wires only, largest first so ProfileFaces() treats the outer
    // boundary as the face and the rest as holes.
    std::vector<TopoDS_Wire> ProfileWires() const override;

    // Minimal closed regions -- what the user can point at. Computed from
    // scratch each time: a sketch small enough to draw by hand costs a
    // millisecond or two, and a cache here would have to be invalidated by
    // every tool that touches geometry, which is a far better source of
    // bugs than it is of speed.
    std::vector<ProfileRegion> ProfileRegions() const override;

    // Every region's face, rather than the outer wire with the rest
    // punched out as holes. Two disjoint squares are two faces, not one
    // face with an absurd hole in it; a rectangle drawn around a circle is
    // the ring AND the disc, which is what selecting both in Fusion gives.
    std::vector<TopoDS_Face> ProfileFaces() const override;

    // ---- display ----

    bool IsVisible() const { return myIsVisible; }
    void SetVisible(bool theValue) { myIsVisible = theValue; }

private:
    gp_Ax3                        myPlanePosition;
    double                        myOffset    = 0.0;
    bool                          myIsVisible = true;
    std::vector<SketchEntity>     myEntities;
    std::vector<SketchConstraint> myConstraints;

    int  myNextEntityId     = 1;
    int  myNextConstraintId = 1;
    int  myNextDimension    = 1;
    bool myNeedsSolve       = false;

    // Left over from the last solve so the timeline can show it without
    // re-running the solver.
    std::string mySolverError;
};

} // namespace lcad
