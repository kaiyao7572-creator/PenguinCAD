#include "StepImport.h"

#include <STEPControl_Reader.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <TopoDS_Compound.hxx>
#include <BRep_Builder.hxx>

TopoDS_Shape ImportStepFile(const std::string& path)
{
    STEPControl_Reader reader;
    IFSelect_ReturnStatus status = reader.ReadFile(path.c_str());
    if (status != IFSelect_RetDone) {
        return TopoDS_Shape();
    }

    reader.TransferRoots();

    const Standard_Integer numShapes = reader.NbShapes();
    if (numShapes <= 0) {
        return TopoDS_Shape();
    }
    if (numShapes == 1) {
        return reader.Shape(1);
    }

    // Multiple root shapes: combine into a single compound.
    TopoDS_Compound compound;
    BRep_Builder builder;
    builder.MakeCompound(compound);
    for (Standard_Integer i = 1; i <= numShapes; ++i) {
        builder.Add(compound, reader.Shape(i));
    }
    return compound;
}
