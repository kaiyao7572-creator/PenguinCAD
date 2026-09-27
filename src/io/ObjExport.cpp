#include "io/ObjExport.h"

#include "core/Body.h"

#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <Poly_Triangle.hxx>
#include <Poly_Triangulation.hxx>
#include <Standard_Failure.hxx>
#include <TopAbs_Orientation.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>

#include <cstddef>
#include <fstream>
#include <iomanip>
#include <ios>
#include <locale>
#include <string>
#include <utility>
#include <vector>

namespace lcad {

namespace {

// The same numbers src/StlExport.cpp meshes with. The two mesh exporters
// disagreeing would mean the same model leaves the app as two different
// shapes depending on which button was pressed.
constexpr double kLinearDeflection  = 0.1;
constexpr double kAngularDeflection = 0.5;

// Decimal places per coordinate. Fixed notation rather than the shortest
// round-trip form: 1e-05 is legal OBJ that a surprising number of readers
// mis-parse, and six places is a nanometre on a millimetre model.
constexpr int kCoordinateDigits = 6;

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

// A `g` line is split on whitespace into several group names, so a body
// the user called "Left Bracket" would arrive as two groups called "Left"
// and "Bracket". Anything unprintable goes the same way.
std::string GroupName(const std::string& theName)
{
    std::string safe;
    safe.reserve(theName.size());
    for (const char character : theName) {
        const unsigned char code = static_cast<unsigned char>(character);
        safe += (code <= ' ' || code == 0x7F) ? '_' : character;
    }
    return safe.empty() ? std::string("Body") : safe;
}

} // namespace

bool ExportShapeToObj(const TopoDS_Shape&             theShape,
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
        // OBJ carries triangles, not B-rep, so the model has to be
        // faceted first. Meshing writes into the shape's own triangulation
        // slots, which is why the const reference is enough.
        BRepMesh_IncrementalMesh mesh(theShape, kLinearDeflection, Standard_False,
                                      kAngularDeflection, Standard_True);
        if (!mesh.IsDone()) {
            theError = "the model could not be triangulated";
            return false;
        }

        const std::vector<TopoDS_Shape> bodies = SplitIntoBodies(theShape);
        if (bodies.empty()) {
            theError = "there is nothing to export";
            return false;
        }

        std::ofstream out(thePath, std::ios::out | std::ios::trunc);
        if (!out.is_open()) {
            theError = "could not open " + thePath + " for writing";
            return false;
        }

        // Before a single number is written. A locale with a comma
        // decimal separator turns "1,5 0,0 2,0" into a vertex with five
        // coordinates, and no reader on earth complains about it out loud.
        out.imbue(std::locale::classic());
        out << std::fixed << std::setprecision(kCoordinateDigits);

        out << "# Wavefront OBJ written by PenguinCAD\n";
        out << "# units: millimetres\n";

        // OBJ vertex indices are 1-based and count from the top of the
        // FILE, not from the current group -- so every face index is this
        // running total plus the triangulation's own 1-based node index.
        std::size_t vertexTotal = 0;
        std::size_t triangleTotal = 0;

        for (std::size_t i = 0; i < bodies.size(); ++i) {
            const std::string name = GroupName(BodyNameAt(theBodyNames, i));
            out << "g " << name << '\n';

            std::size_t bodyTriangles = 0;
            for (TopExp_Explorer explorer(bodies[i], TopAbs_FACE); explorer.More();
                 explorer.Next()) {
                const TopoDS_Face face = TopoDS::Face(explorer.Current());

                TopLoc_Location                  location;
                const Handle(Poly_Triangulation) triangulation =
                    BRep_Tool::Triangulation(face, location);
                if (triangulation.IsNull() || triangulation->NbTriangles() <= 0) {
                    continue;
                }

                const gp_Trsf& transform = location.Transformation();
                const bool     isMoved   = !location.IsIdentity();

                const std::size_t base = vertexTotal;
                for (Standard_Integer node = 1; node <= triangulation->NbNodes(); ++node) {
                    gp_Pnt point = triangulation->Node(node);
                    if (isMoved) {
                        point.Transform(transform);
                    }
                    out << "v " << point.X() << ' ' << point.Y() << ' ' << point.Z() << '\n';
                }
                vertexTotal += static_cast<std::size_t>(triangulation->NbNodes());

                // A triangulation is wound for the surface's own normal,
                // and a REVERSED face is that same surface used the other
                // way round -- half the faces of a plain box, so getting
                // this wrong is not an edge case, it is half the model.
                //
                // The location's handedness is deliberately NOT folded in
                // here. A mirrored location leaves a solid inside out by
                // OCCT's own reckoning (BRepGProp reports its volume as
                // negative), and StlAPI_Writer writes it inside out to
                // match. Correcting it here would make the OBJ and the STL
                // of one body disagree about which way it faces.
                const bool flip = face.Orientation() == TopAbs_REVERSED;

                for (Standard_Integer index = 1; index <= triangulation->NbTriangles(); ++index) {
                    Standard_Integer n1 = 0;
                    Standard_Integer n2 = 0;
                    Standard_Integer n3 = 0;
                    triangulation->Triangle(index).Get(n1, n2, n3);
                    if (n1 == n2 || n2 == n3 || n1 == n3) {
                        continue;  // collapsed at a pole; carries no surface
                    }
                    if (flip) {
                        std::swap(n2, n3);
                    }
                    out << "f " << (base + static_cast<std::size_t>(n1)) << ' '
                        << (base + static_cast<std::size_t>(n2)) << ' '
                        << (base + static_cast<std::size_t>(n3)) << '\n';
                    ++bodyTriangles;
                }
            }

            // OBJ has no way to say "wire body", and a body that just
            // vanished from the file is the kind of thing nobody notices
            // until the part comes back wrong.
            if (bodyTriangles == 0) {
                out << "# " << name << " has no faces to mesh\n";
            }
            triangleTotal += bodyTriangles;
        }

        if (triangleTotal == 0) {
            theError = "the model has no surfaces to write as a mesh";
            return false;
        }

        // close() is what actually flushes, so the check has to come
        // after it or a full disk reports success.
        out.close();
        if (out.fail()) {
            theError = "could not finish writing " + thePath;
            return false;
        }
        return true;
    } catch (const Standard_Failure& failure) {
        theError = OcctMessage(failure, "OBJ export failed");
        return false;
    }
}

} // namespace lcad
