#pragma once

#include "features/ProfileFeatures.h"

#include <TopoDS_Wire.hxx>

#include <string>
#include <vector>

namespace lcad {

// How the section is carried along the path -- Fusion's Orientation
// dropdown.
//
// Perpendicular keeps the section square to the path, which is what makes
// a swept tube keep its bore all the way round a bend. Parallel keeps the
// section in the pose it was drawn in and simply slides it along, so a
// path that turns produces a sheared solid rather than a turned one. The
// integer values are part of the stored parameter, so don't renumber them.
enum class SweepOrientation
{
    Perpendicular = 0,
    Parallel      = 1
};

// Labels in enum order; the dialog and the properties panel share them so
// a value typed into one is understood by the other.
const std::vector<std::string>& SweepOrientationNames();
std::string SweepOrientationName(SweepOrientation theOrientation);
SweepOrientation SweepOrientationFromInt(int theValue);
bool ParseSweepOrientation(const std::string& theText, SweepOrientation& theOrientation);

// Sketch profile driven along a path -- Fusion's CREATE > SWEEP.
//
// The profile comes from ProfileFeature: a sketch held by name, optionally
// narrowed to the regions the user clicked. The PATH is a second sketch,
// also held by name and resolved through the same ProfileProvider seam, so
// this feature knows no more about the sketch subsystem than Extrude does.
//
// Only a closed path can be resolved today. ProfileProvider hands over
// closed wires and nothing else, and an open chain -- the usual Fusion
// sweep path -- never reaches this side of the seam. Rather than guess
// something up, Compute() says so in as many words; widening it is a
// change to ProfileProvider, not to this file.
class SweepFeature : public ProfileFeature
{
public:
    SweepFeature() = default;
    SweepFeature(std::string theSketchName, std::string thePathSketchName);

    std::string TypeName() const override { return "Sweep"; }

    bool Compute(const ComputeContext& theContext,
                 const TopoDS_Shape&   theInput,
                 TopoDS_Shape&         theOutput,
                 std::string&          theError) override;

    std::unique_ptr<Feature> Clone() const override;

    std::vector<Parameter> Parameters() const override;
    bool SetParameter(const Parameter& theParameter) override;

    const std::string& PathSketchName() const { return myPathSketchName; }
    void SetPathSketchName(std::string theName) { myPathSketchName = std::move(theName); }

    SweepOrientation Orientation() const { return myOrientation; }
    void SetOrientation(SweepOrientation theOrientation) { myOrientation = theOrientation; }

    // Both angles are stored in DEGREES, the way every other angle in the
    // app is stored and shown. The single conversion to the radians OCCT
    // wants happens inside Compute().
    double TaperDegrees() const { return myTaperDegrees; }
    void SetTaperDegrees(double theAngle) { myTaperDegrees = theAngle; }

    double TwistDegrees() const { return myTwistDegrees; }
    void SetTwistDegrees(double theAngle) { myTwistDegrees = theAngle; }

private:
    // The path wire of the named path sketch, or a message saying why
    // there isn't one. Deliberately refuses to choose between several
    // loops rather than picking the biggest.
    bool ResolvePath(const ComputeContext& theContext,
                     TopoDS_Wire&          thePath,
                     std::string&          theError) const;

    std::string      myPathSketchName;
    SweepOrientation myOrientation  = SweepOrientation::Perpendicular;
    double           myTaperDegrees = 0.0;
    double           myTwistDegrees = 0.0;
};

} // namespace lcad
