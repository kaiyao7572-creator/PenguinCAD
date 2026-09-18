#include "core/Entity.h"

namespace lcad {

std::string EntityTypeName(EntityType theType)
{
    switch (theType) {
        case EntityType::BRepBody:                 return "BRepBody";
        case EntityType::BRepLump:                 return "BRepLump";
        case EntityType::BRepShell:                return "BRepShell";
        case EntityType::BRepFace:                 return "BRepFace";
        case EntityType::BRepLoop:                 return "BRepLoop";
        case EntityType::BRepCoEdge:               return "BRepCoEdge";
        case EntityType::BRepEdge:                 return "BRepEdge";
        case EntityType::BRepVertex:               return "BRepVertex";
        case EntityType::BRepWire:                 return "BRepWire";
        case EntityType::MeshBody:                 return "MeshBody";

        case EntityType::Sketch:                   return "Sketch";
        case EntityType::SketchLine:               return "SketchLine";
        case EntityType::SketchArc:                return "SketchArc";
        case EntityType::SketchCircle:             return "SketchCircle";
        case EntityType::SketchEllipse:            return "SketchEllipse";
        case EntityType::SketchEllipticalArc:      return "SketchEllipticalArc";
        case EntityType::SketchConicCurve:         return "SketchConicCurve";
        case EntityType::SketchFittedSpline:       return "SketchFittedSpline";
        case EntityType::SketchControlPointSpline: return "SketchControlPointSpline";
        case EntityType::SketchFixedSpline:        return "SketchFixedSpline";
        case EntityType::SketchPoint:              return "SketchPoint";
        case EntityType::SketchText:               return "SketchText";
        case EntityType::Profile:                  return "Profile";
        case EntityType::SketchDimension:          return "SketchDimension";
        case EntityType::GeometricConstraint:      return "GeometricConstraint";

        case EntityType::ConstructionPlane:        return "ConstructionPlane";
        case EntityType::ConstructionAxis:         return "ConstructionAxis";
        case EntityType::ConstructionPoint:        return "ConstructionPoint";

        case EntityType::Component:                return "Component";
        case EntityType::Occurrence:               return "Occurrence";
        case EntityType::Feature:                  return "Feature";
        case EntityType::Joint:                    return "Joint";
        case EntityType::JointOrigin:              return "JointOrigin";
        case EntityType::RigidGroup:               return "RigidGroup";

        case EntityType::Unknown:                  break;
    }
    return "Unknown";
}

std::string EntityDisplayName(EntityType theType)
{
    switch (theType) {
        case EntityType::BRepBody:                 return "Body";
        case EntityType::BRepLump:                 return "Lump";
        case EntityType::BRepShell:                return "Shell";
        case EntityType::BRepFace:                 return "Face";
        case EntityType::BRepLoop:                 return "Loop";
        case EntityType::BRepCoEdge:               return "Co-edge";
        case EntityType::BRepEdge:                 return "Edge";
        case EntityType::BRepVertex:               return "Vertex";
        case EntityType::BRepWire:                 return "Wire";
        case EntityType::MeshBody:                 return "Mesh Body";

        case EntityType::Sketch:                   return "Sketch";
        case EntityType::SketchLine:               return "Line";
        case EntityType::SketchArc:                return "Arc";
        case EntityType::SketchCircle:             return "Circle";
        case EntityType::SketchEllipse:            return "Ellipse";
        case EntityType::SketchEllipticalArc:      return "Elliptical Arc";
        case EntityType::SketchConicCurve:         return "Conic Curve";
        case EntityType::SketchFittedSpline:       return "Fit Point Spline";
        case EntityType::SketchControlPointSpline: return "Control Point Spline";
        case EntityType::SketchFixedSpline:        return "Fixed Spline";
        case EntityType::SketchPoint:              return "Point";
        case EntityType::SketchText:               return "Text";
        case EntityType::Profile:                  return "Profile";
        case EntityType::SketchDimension:          return "Dimension";
        case EntityType::GeometricConstraint:      return "Constraint";

        case EntityType::ConstructionPlane:        return "Plane";
        case EntityType::ConstructionAxis:         return "Axis";
        case EntityType::ConstructionPoint:        return "Point";

        case EntityType::Component:                return "Component";
        case EntityType::Occurrence:               return "Occurrence";
        case EntityType::Feature:                  return "Feature";
        case EntityType::Joint:                    return "Joint";
        case EntityType::JointOrigin:              return "Joint Origin";
        case EntityType::RigidGroup:               return "Rigid Group";

        case EntityType::Unknown:                  break;
    }
    return "Unknown";
}

std::string BrowserFolderName(BrowserFolder theFolder)
{
    switch (theFolder) {
        case BrowserFolder::DocumentSettings: return "Document Settings";
        case BrowserFolder::NamedViews:       return "Named Views";
        case BrowserFolder::Origin:           return "Origin";
        case BrowserFolder::Analysis:         return "Analysis";
        case BrowserFolder::Joints:           return "Joints";
        case BrowserFolder::Bodies:           return "Bodies";
        case BrowserFolder::Sketches:         return "Sketches";
        case BrowserFolder::Construction:     return "Construction";
        case BrowserFolder::Canvases:         return "Canvases";
        case BrowserFolder::Decals:           return "Decals";
        case BrowserFolder::None:             break;
    }
    return std::string();
}

const std::vector<BrowserFolder>& BrowserFolderOrder()
{
    // Fusion's order, top to bottom. A folder is drawn only once it holds
    // something, but when two are present their relative order is fixed --
    // which is why this is a list and not a set.
    static const std::vector<BrowserFolder> theOrder = {
        BrowserFolder::DocumentSettings,
        BrowserFolder::NamedViews,
        BrowserFolder::Origin,
        BrowserFolder::Analysis,
        BrowserFolder::Joints,
        BrowserFolder::Bodies,
        BrowserFolder::Sketches,
        BrowserFolder::Construction,
        BrowserFolder::Canvases,
        BrowserFolder::Decals
    };
    return theOrder;
}

BrowserFolder FolderOf(EntityType theType)
{
    switch (theType) {
        case EntityType::BRepBody:
        case EntityType::MeshBody:
            return BrowserFolder::Bodies;

        case EntityType::Sketch:
            return BrowserFolder::Sketches;

        case EntityType::ConstructionPlane:
        case EntityType::ConstructionAxis:
        case EntityType::ConstructionPoint:
            return BrowserFolder::Construction;

        case EntityType::Joint:
        case EntityType::JointOrigin:
        case EntityType::RigidGroup:
            return BrowserFolder::Joints;

        default:
            // Features sit directly under the component, and the sub-shapes
            // of a body are reached through the body rather than listed.
            return BrowserFolder::None;
    }
}

bool IsSketchCurve(EntityType theType)
{
    switch (theType) {
        case EntityType::SketchLine:
        case EntityType::SketchArc:
        case EntityType::SketchCircle:
        case EntityType::SketchEllipse:
        case EntityType::SketchEllipticalArc:
        case EntityType::SketchConicCurve:
        case EntityType::SketchFittedSpline:
        case EntityType::SketchControlPointSpline:
        case EntityType::SketchFixedSpline:
            return true;
        default:
            return false;
    }
}

bool IsSelectable(EntityType theType)
{
    switch (theType) {
        // Structural, never pickable: a loop and a co-edge exist to say how
        // faces and edges relate, and lumps and shells exist to say how a
        // body is put together. Fusion picks bodies, faces, edges and
        // vertices, and nothing between them.
        case EntityType::BRepLump:
        case EntityType::BRepShell:
        case EntityType::BRepLoop:
        case EntityType::BRepCoEdge:
        case EntityType::Unknown:
            return false;
        default:
            return true;
    }
}

} // namespace lcad
