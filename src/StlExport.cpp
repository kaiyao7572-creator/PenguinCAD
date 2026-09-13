#include "StlExport.h"

#include <BRepMesh_IncrementalMesh.hxx>
#include <StlAPI_Writer.hxx>

bool ExportShapeToStl(const TopoDS_Shape& shape, const std::string& path)
{
    if (shape.IsNull()) {
        return false;
    }

    // STL export needs a triangulated mesh, not raw B-rep geometry.
    // Linear deflection controls mesh fineness; 0.1 is a reasonable
    // default for now (units follow the shape's own).
    const Standard_Real linearDeflection = 0.1;
    const Standard_Real angularDeflection = 0.5;
    BRepMesh_IncrementalMesh mesh(shape, linearDeflection, Standard_False, angularDeflection, Standard_True);
    mesh.Perform();
    if (!mesh.IsDone()) {
        return false;
    }

    StlAPI_Writer writer;
    return writer.Write(shape, path.c_str());
}
