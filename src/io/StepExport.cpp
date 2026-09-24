#include "io/StepExport.h"

#include "core/Body.h"

#include <IFSelect_ReturnStatus.hxx>
#include <Interface_Static.hxx>
#include <STEPControl_Controller.hxx>
#include <STEPControl_StepModelType.hxx>
#include <STEPControl_Writer.hxx>
#include <Standard_Failure.hxx>

#include <cstddef>
#include <string>
#include <vector>

namespace lcad {

namespace {

// AP214 rather than AP242. Both carry the solid geometry this app makes;
// AP214 is the one everything on the other end reads, and AP242's extra
// vocabulary is PMI and assembly metadata we never write.
const char* const kSchema = "AP214IS";

// The app stores millimetres everywhere (core/Units.h) and a STEP file
// records the unit it was written in, so this is the seam where the two
// have to agree -- it is also where src/StepImport.cpp meets us, since
// the reader converts an incoming file to millimetres.
//
// Set explicitly rather than trusting the default: these are process-wide
// statics that anything could have moved, and a file written in inches
// reads back 25.4x too big with nothing on screen to say why.
const char* const kUnit = "MM";

std::string StatusText(IFSelect_ReturnStatus theStatus)
{
    switch (theStatus) {
        case IFSelect_RetVoid:  return "nothing was produced";
        case IFSelect_RetDone:  return "done";
        case IFSelect_RetError: return "bad data";
        case IFSelect_RetFail:  return "the kernel refused it";
        case IFSelect_RetStop:  return "interrupted";
    }
    return "unknown error";
}

std::string OcctMessage(const Standard_Failure& theFailure, const std::string& theFallback)
{
    const Standard_CString message = theFailure.GetMessageString();
    return (message != nullptr && message[0] != '\0') ? std::string(message) : theFallback;
}

std::string BodyNameAt(const std::vector<std::string>& theNames, std::size_t theIndex)
{
    if (theIndex < theNames.size() && !theNames[theIndex].empty()) {
        return theNames[theIndex];
    }
    return "Body" + std::to_string(theIndex + 1);
}

} // namespace

bool ExportShapeToStep(const TopoDS_Shape&             theShape,
                       const std::vector<std::string>& theBodyNames,
                       const std::string&              thePath,
                       std::string&                    theError)
{
    theError.clear();

    if (thePath.empty()) {
        theError = "no file name was given";
        return false;
    }
    if (theShape.IsNull()) {
        theError = "there is nothing to export";
        return false;
    }

    try {
        // Defines the write.step.* statics; SetCVal below silently does
        // nothing until it has run.
        STEPControl_Controller::Init();

        if (!Interface_Static::SetCVal("write.step.schema", kSchema)
            || !Interface_Static::SetCVal("write.step.unit", kUnit)) {
            theError = "STEP is not configured in this build of OCCT: "
                       "the schema and unit could not be set";
            return false;
        }

        // Built after the statics above, which is where its model reads
        // the schema and unit from.
        STEPControl_Writer writer;

        // One Transfer per body, so three bodies leave as three products.
        // Handing the whole compound over in one Transfer also works, but
        // then every body shares one product and nothing downstream can
        // tell them apart or name them.
        const std::vector<TopoDS_Shape> bodies = SplitIntoBodies(theShape);
        if (bodies.empty()) {
            theError = "there is nothing to export";
            return false;
        }

        for (std::size_t i = 0; i < bodies.size(); ++i) {
            const std::string name = BodyNameAt(theBodyNames, i);

            // Cosmetic only, so a failure here is not worth refusing the
            // export over. OCCT appends its own product index, which is
            // why the file says "Body1 2" rather than "Body1".
            Interface_Static::SetCVal("write.step.product.name", name.c_str());

            const IFSelect_ReturnStatus status = writer.Transfer(bodies[i], STEPControl_AsIs);
            if (status != IFSelect_RetDone) {
                theError = "could not translate " + name + " to STEP: " + StatusText(status);
                return false;
            }
        }

        // A half-written file is reported, not deleted: the path may be a
        // file the user already had, and destroying it to tidy up after a
        // failed export would cost them more than the stale file does.
        const IFSelect_ReturnStatus status = writer.Write(thePath.c_str());
        if (status != IFSelect_RetDone) {
            theError = "could not write " + thePath + ": " + StatusText(status);
            return false;
        }
        return true;
    } catch (const Standard_Failure& failure) {
        theError = OcctMessage(failure, "STEP export failed");
        return false;
    }
}

} // namespace lcad
