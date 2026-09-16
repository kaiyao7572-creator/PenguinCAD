#include "core/ProfileProvider.h"

#include "core/Feature.h"

#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepTools.hxx>
#include <Standard_Failure.hxx>

namespace lcad {

std::vector<TopoDS_Face> ProfileProvider::ProfileFaces() const
{
    std::vector<TopoDS_Face> faces;
    const std::vector<TopoDS_Wire> wires = ProfileWires();
    if (wires.empty()) {
        return faces;
    }

    try {
        const gp_Pln plane = ProfilePlane();

        // First wire is treated as the outer boundary; any others are
        // punched out as holes. That covers the common "rectangle with a
        // circle in it" case without needing full containment analysis.
        BRepBuilderAPI_MakeFace faceMaker(plane, wires.front());
        if (!faceMaker.IsDone()) {
            return faces;
        }

        for (std::size_t i = 1; i < wires.size(); ++i) {
            TopoDS_Wire hole = wires[i];
            hole.Reverse();
            faceMaker.Add(hole);
        }

        if (faceMaker.IsDone()) {
            faces.push_back(faceMaker.Face());
        }
    } catch (const Standard_Failure&) {
        faces.clear();
    }

    return faces;
}

ProfileProvider* AsProfileProvider(Feature* theFeature)
{
    return dynamic_cast<ProfileProvider*>(theFeature);
}

} // namespace lcad
