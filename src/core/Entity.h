#pragma once

#include <string>
#include <vector>

namespace lcad {

// Every kind of thing this document can contain, named the way Fusion
// names it.
//
// Fusion's object model is the target, not an approximation of it: the
// browser, the selection filter and the properties panel all need to ask
// "what kind of thing is this" and branch, and matching Fusion's
// vocabulary exactly is what lets a user's expectations carry over. A
// flat enum rather than a class hierarchy on purpose -- nine abstract
// base classes and a visitor would be a lot of ceremony for one question.
//
// Fusion's B-Rep containment order, which the values below preserve:
//
//   BRepBody > BRepLump > BRepShell > BRepFace > BRepLoop > BRepCoEdge
//                                              > BRepEdge > BRepVertex
//
// A Lump is one connected chunk of a body; a Shell is one closed (or
// open) boundary of a lump -- a hollow cube has an outer and an inner
// shell in one lump. A CoEdge is the use of an edge by one particular
// face, which is how the two faces meeting at an edge are told apart.
enum class EntityType
{
    Unknown = 0,

    // ---- B-Rep ----
    BRepBody,
    BRepLump,
    BRepShell,
    BRepFace,
    BRepLoop,
    BRepCoEdge,
    BRepEdge,
    BRepVertex,
    BRepWire,
    MeshBody,

    // ---- sketch ----
    Sketch,
    SketchLine,
    SketchArc,
    SketchCircle,
    SketchEllipse,
    SketchEllipticalArc,
    SketchConicCurve,
    SketchFittedSpline,
    SketchControlPointSpline,
    SketchFixedSpline,
    SketchPoint,
    SketchText,
    Profile,
    SketchDimension,
    GeometricConstraint,

    // ---- construction ----
    ConstructionPlane,
    ConstructionAxis,
    ConstructionPoint,

    // ---- structure ----
    Component,
    Occurrence,
    Feature,
    Joint,
    JointOrigin,
    RigidGroup
};

// The exact Fusion API type name, e.g. "BRepFace", "SketchEllipticalArc".
// Used by the browser's Type column and by anything that logs or stores a
// type, so a name here is part of the file format -- don't rename one.
std::string EntityTypeName(EntityType theType);

// What the user sees, e.g. "Face", "Elliptical Arc". Fusion shows the
// friendly name in the browser and the API name nowhere.
std::string EntityDisplayName(EntityType theType);

// Which browser folder an entity belongs under. Fusion's browser is not a
// free-form tree: a component has a fixed set of folders, each appearing
// only once it has something in it, and always in this order.
enum class BrowserFolder
{
    None = 0,        // sits directly under the component (features do)
    DocumentSettings,
    NamedViews,
    Origin,
    Analysis,
    Joints,
    Bodies,
    Sketches,
    Construction,
    Canvases,
    Decals
};

std::string BrowserFolderName(BrowserFolder theFolder);

// Folder order as Fusion draws it, top to bottom.
const std::vector<BrowserFolder>& BrowserFolderOrder();

BrowserFolder FolderOf(EntityType theType);

// True for the curve types -- everything Fusion derives from SketchCurve,
// which is every sketch entity except points and text.
bool IsSketchCurve(EntityType theType);

// True for the types a user can pick in the viewport. Loops, co-edges,
// lumps and shells are structural: real in the model, never selectable.
bool IsSelectable(EntityType theType);

} // namespace lcad
