#include "features/ConstructionFeatures.h"

#include "core/Document.h"

#include <Standard_Failure.hxx>
#include <gp_Dir.hxx>
#include <gp_Lin.hxx>
#include <gp_Pln.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <cctype>
#include <cmath>

namespace lcad {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Three points closer together than this, or more nearly in a line than
// this, do not define a plane. Generous on purpose: a nearly-degenerate
// plane is worse than no plane, because its normal flips under a rebuild.
constexpr double kMinSpan = 1.0e-7;

std::string ToLower(const std::string& theText)
{
    std::string lower = theText;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char theChar) {
        return static_cast<char>(std::tolower(theChar));
    });
    return lower;
}

// Accepts a label ("Offset", case-insensitive) or the bare index, which is
// what lets the kind be edited as a plain string parameter.
bool ParseKind(const std::vector<std::string>& theNames, const std::string& theText, int& theIndex)
{
    const std::string lower = ToLower(theText);
    for (std::size_t i = 0; i < theNames.size(); ++i) {
        if (ToLower(theNames[i]) == lower) {
            theIndex = static_cast<int>(i);
            return true;
        }
    }
    try {
        const int parsed = std::stoi(theText);
        if (parsed >= 0 && parsed < static_cast<int>(theNames.size())) {
            theIndex = parsed;
            return true;
        }
    } catch (const std::exception&) {
    }
    return false;
}

} // namespace

// ---- plane ----

const std::vector<std::string>& PlaneKindNames()
{
    static const std::vector<std::string> theNames = {
        "Offset", "At Angle", "Midplane", "Three Points"};
    return theNames;
}

std::string PlaneKindName(PlaneKind theKind)
{
    const std::size_t index = static_cast<std::size_t>(theKind);
    return index < PlaneKindNames().size() ? PlaneKindNames()[index] : std::string("Offset");
}

bool ParsePlaneKind(const std::string& theText, PlaneKind& theKind)
{
    int index = 0;
    if (!ParseKind(PlaneKindNames(), theText, index)) {
        return false;
    }
    theKind = static_cast<PlaneKind>(index);
    return true;
}

ConstructionPlaneFeature::ConstructionPlaneFeature(std::string theBasePlane, double theOffset)
    : myBasePlane(std::move(theBasePlane))
    , myOffset(theOffset)
{
}

void ConstructionPlaneFeature::SetPoints(const gp_Pnt& theA, const gp_Pnt& theB, const gp_Pnt& theC)
{
    myPointA = theA;
    myPointB = theB;
    myPointC = theC;
}

bool ConstructionPlaneFeature::Build(const ComputeContext& theContext,
                                     gp_Ax3&               theResult,
                                     std::string&          theError) const
{
    switch (myKind) {
        case PlaneKind::Offset: {
            gp_Ax3 base;
            if (!ResolvePlaneByName(theContext, myBasePlane, base)) {
                theError = "no plane called '" + myBasePlane + "' earlier in the timeline";
                return false;
            }
            theResult = base;
            theResult.SetLocation(base.Location().Translated(gp_Vec(base.Direction()) * myOffset));
            return true;
        }

        case PlaneKind::AtAngle: {
            gp_Ax3 base;
            if (!ResolvePlaneByName(theContext, myBasePlane, base)) {
                theError = "no plane called '" + myBasePlane + "' earlier in the timeline";
                return false;
            }
            // Turned about its own X axis, so the hinge is a line lying in
            // the base plane -- which is what "at an angle to that plane"
            // means to a user, and what Fusion hinges about when the edge
            // picked is the plane's own.
            const gp_Ax1 hinge(base.Location(), base.XDirection());
            theResult = base;
            theResult.Rotate(hinge, myAngleDegrees * kPi / 180.0);
            return true;
        }

        case PlaneKind::Midplane: {
            gp_Ax3 first;
            gp_Ax3 second;
            if (!ResolvePlaneByName(theContext, myBasePlane, first)) {
                theError = "no plane called '" + myBasePlane + "' earlier in the timeline";
                return false;
            }
            if (!ResolvePlaneByName(theContext, mySecondPlane, second)) {
                theError = "no plane called '" + mySecondPlane + "' earlier in the timeline";
                return false;
            }
            // Only meaningful between parallel planes: halfway between two
            // planes that cross is a line, not a plane.
            if (!first.Direction().IsParallel(second.Direction(), 1.0e-7)) {
                theError = "a midplane needs two parallel planes";
                return false;
            }
            theResult = first;
            const gp_Pnt middle((first.Location().X() + second.Location().X()) * 0.5,
                                (first.Location().Y() + second.Location().Y()) * 0.5,
                                (first.Location().Z() + second.Location().Z()) * 0.5);
            theResult.SetLocation(middle);
            return true;
        }

        case PlaneKind::ThreePoints: {
            const gp_Vec first(myPointA, myPointB);
            const gp_Vec second(myPointA, myPointC);
            const gp_Vec normal = first.Crossed(second);
            if (normal.Magnitude() <= kMinSpan) {
                theError = "those three points are in a line";
                return false;
            }
            theResult = gp_Ax3(myPointA, gp_Dir(normal), gp_Dir(first));
            return true;
        }
    }
    theError = "unknown plane type";
    return false;
}

bool ConstructionPlaneFeature::Compute(const ComputeContext& theContext,
                                       const TopoDS_Shape&   theInput,
                                       TopoDS_Shape&         theOutput,
                                       std::string&          theError)
{
    // Construction geometry contributes no solid: the model passes
    // through untouched, exactly as a sketch does.
    theOutput = theInput;
    myIsValid = false;

    try {
        gp_Ax3 result;
        if (!Build(theContext, result, theError)) {
            return false;
        }
        myResult = result;
        myIsValid = true;
    } catch (const Standard_Failure& failure) {
        const Standard_CString message = failure.GetMessageString();
        theError = message != nullptr && message[0] != '\0' ? message : "could not build the plane";
        return false;
    }
    return true;
}

std::unique_ptr<Feature> ConstructionPlaneFeature::Clone() const
{
    auto copy = std::make_unique<ConstructionPlaneFeature>();
    copy->myKind = myKind;
    copy->myBasePlane = myBasePlane;
    copy->mySecondPlane = mySecondPlane;
    copy->myOffset = myOffset;
    copy->myAngleDegrees = myAngleDegrees;
    copy->myPointA = myPointA;
    copy->myPointB = myPointB;
    copy->myPointC = myPointC;
    copy->myResult = myResult;
    copy->myIsValid = myIsValid;
    CopyBaseTo(*copy);
    return copy;
}

std::vector<Parameter> ConstructionPlaneFeature::Parameters() const
{
    std::vector<Parameter> parameters;
    parameters.push_back(Parameter::MakeString("Type", PlaneKindName(myKind)));
    parameters.push_back(Parameter::MakeString("Base Plane", myBasePlane));

    switch (myKind) {
        case PlaneKind::Offset:
            parameters.push_back(Parameter::MakeDouble("Offset", myOffset));
            break;
        case PlaneKind::AtAngle:
            parameters.push_back(Parameter::MakeDouble("Angle", myAngleDegrees, "deg"));
            break;
        case PlaneKind::Midplane:
            parameters.push_back(Parameter::MakeString("Second Plane", mySecondPlane));
            break;
        case PlaneKind::ThreePoints:
            parameters.push_back(Parameter::MakeDouble("A X", myPointA.X()));
            parameters.push_back(Parameter::MakeDouble("A Y", myPointA.Y()));
            parameters.push_back(Parameter::MakeDouble("A Z", myPointA.Z()));
            parameters.push_back(Parameter::MakeDouble("B X", myPointB.X()));
            parameters.push_back(Parameter::MakeDouble("B Y", myPointB.Y()));
            parameters.push_back(Parameter::MakeDouble("B Z", myPointB.Z()));
            parameters.push_back(Parameter::MakeDouble("C X", myPointC.X()));
            parameters.push_back(Parameter::MakeDouble("C Y", myPointC.Y()));
            parameters.push_back(Parameter::MakeDouble("C Z", myPointC.Z()));
            break;
    }
    return parameters;
}

bool ConstructionPlaneFeature::SetParameter(const Parameter& theParameter)
{
    if (theParameter.name == "Type") {
        PlaneKind kind = myKind;
        if (!ParsePlaneKind(theParameter.stringValue, kind)) {
            return false;
        }
        myKind = kind;
        return true;
    }
    if (theParameter.name == "Base Plane") {
        myBasePlane = theParameter.stringValue;
        return true;
    }
    if (theParameter.name == "Second Plane") {
        mySecondPlane = theParameter.stringValue;
        return true;
    }
    if (theParameter.name == "Offset") {
        myOffset = theParameter.doubleValue;
        return true;
    }
    if (theParameter.name == "Angle") {
        myAngleDegrees = theParameter.doubleValue;
        return true;
    }

    // The nine "A X" ... "C Z" rows, kept as one rule rather than nine.
    if (theParameter.name.size() == 3 && theParameter.name[1] == ' ') {
        gp_Pnt* target = nullptr;
        switch (theParameter.name[0]) {
            case 'A': target = &myPointA; break;
            case 'B': target = &myPointB; break;
            case 'C': target = &myPointC; break;
            default: return false;
        }
        gp_Pnt updated = *target;
        switch (theParameter.name[2]) {
            case 'X': updated.SetX(theParameter.doubleValue); break;
            case 'Y': updated.SetY(theParameter.doubleValue); break;
            case 'Z': updated.SetZ(theParameter.doubleValue); break;
            default: return false;
        }
        *target = updated;
        return true;
    }
    return false;
}

bool ConstructionPlaneFeature::AsPlane(gp_Ax3& thePlane) const
{
    if (!myIsValid) {
        return false;
    }
    thePlane = myResult;
    return true;
}

bool ConstructionPlaneFeature::AsPoint(gp_Pnt& thePoint) const
{
    if (!myIsValid) {
        return false;
    }
    thePoint = myResult.Location();
    return true;
}

// ---- axis ----

const std::vector<std::string>& AxisKindNames()
{
    static const std::vector<std::string> theNames = {"Two Points", "Perpendicular To Plane"};
    return theNames;
}

std::string AxisKindName(AxisKind theKind)
{
    const std::size_t index = static_cast<std::size_t>(theKind);
    return index < AxisKindNames().size() ? AxisKindNames()[index] : std::string("Two Points");
}

bool ParseAxisKind(const std::string& theText, AxisKind& theKind)
{
    int index = 0;
    if (!ParseKind(AxisKindNames(), theText, index)) {
        return false;
    }
    theKind = static_cast<AxisKind>(index);
    return true;
}

void ConstructionAxisFeature::SetPoints(const gp_Pnt& theFrom, const gp_Pnt& theTo)
{
    myFrom = theFrom;
    myTo = theTo;
}

bool ConstructionAxisFeature::Build(const ComputeContext& theContext,
                                    gp_Ax1&               theResult,
                                    std::string&          theError) const
{
    switch (myKind) {
        case AxisKind::TwoPoints: {
            const gp_Vec along(myFrom, myTo);
            if (along.Magnitude() <= kMinSpan) {
                theError = "those two points are in the same place";
                return false;
            }
            theResult = gp_Ax1(myFrom, gp_Dir(along));
            return true;
        }
        case AxisKind::PerpendicularToPlane: {
            gp_Ax3 base;
            if (!ResolvePlaneByName(theContext, myBasePlane, base)) {
                theError = "no plane called '" + myBasePlane + "' earlier in the timeline";
                return false;
            }
            theResult = gp_Ax1(base.Location(), base.Direction());
            return true;
        }
    }
    theError = "unknown axis type";
    return false;
}

bool ConstructionAxisFeature::Compute(const ComputeContext& theContext,
                                      const TopoDS_Shape&   theInput,
                                      TopoDS_Shape&         theOutput,
                                      std::string&          theError)
{
    theOutput = theInput;
    myIsValid = false;
    try {
        gp_Ax1 result;
        if (!Build(theContext, result, theError)) {
            return false;
        }
        myResult = result;
        myIsValid = true;
    } catch (const Standard_Failure& failure) {
        const Standard_CString message = failure.GetMessageString();
        theError = message != nullptr && message[0] != '\0' ? message : "could not build the axis";
        return false;
    }
    return true;
}

std::unique_ptr<Feature> ConstructionAxisFeature::Clone() const
{
    auto copy = std::make_unique<ConstructionAxisFeature>();
    copy->myKind = myKind;
    copy->myFrom = myFrom;
    copy->myTo = myTo;
    copy->myBasePlane = myBasePlane;
    copy->myResult = myResult;
    copy->myIsValid = myIsValid;
    CopyBaseTo(*copy);
    return copy;
}

std::vector<Parameter> ConstructionAxisFeature::Parameters() const
{
    std::vector<Parameter> parameters;
    parameters.push_back(Parameter::MakeString("Type", AxisKindName(myKind)));
    if (myKind == AxisKind::TwoPoints) {
        parameters.push_back(Parameter::MakeDouble("From X", myFrom.X()));
        parameters.push_back(Parameter::MakeDouble("From Y", myFrom.Y()));
        parameters.push_back(Parameter::MakeDouble("From Z", myFrom.Z()));
        parameters.push_back(Parameter::MakeDouble("To X", myTo.X()));
        parameters.push_back(Parameter::MakeDouble("To Y", myTo.Y()));
        parameters.push_back(Parameter::MakeDouble("To Z", myTo.Z()));
    } else {
        parameters.push_back(Parameter::MakeString("Base Plane", myBasePlane));
    }
    return parameters;
}

bool ConstructionAxisFeature::SetParameter(const Parameter& theParameter)
{
    if (theParameter.name == "Type") {
        AxisKind kind = myKind;
        if (!ParseAxisKind(theParameter.stringValue, kind)) {
            return false;
        }
        myKind = kind;
        return true;
    }
    if (theParameter.name == "Base Plane") {
        myBasePlane = theParameter.stringValue;
        return true;
    }

    gp_Pnt* target = nullptr;
    char axis = '\0';
    if (theParameter.name.rfind("From ", 0) == 0 && theParameter.name.size() == 6) {
        target = &myFrom;
        axis = theParameter.name[5];
    } else if (theParameter.name.rfind("To ", 0) == 0 && theParameter.name.size() == 4) {
        target = &myTo;
        axis = theParameter.name[3];
    }
    if (target == nullptr) {
        return false;
    }

    gp_Pnt updated = *target;
    switch (axis) {
        case 'X': updated.SetX(theParameter.doubleValue); break;
        case 'Y': updated.SetY(theParameter.doubleValue); break;
        case 'Z': updated.SetZ(theParameter.doubleValue); break;
        default: return false;
    }
    *target = updated;
    return true;
}

bool ConstructionAxisFeature::AsAxis(gp_Ax1& theAxis) const
{
    if (!myIsValid) {
        return false;
    }
    theAxis = myResult;
    return true;
}

bool ConstructionAxisFeature::AsPoint(gp_Pnt& thePoint) const
{
    if (!myIsValid) {
        return false;
    }
    thePoint = myResult.Location();
    return true;
}

// ---- point ----

const std::vector<std::string>& PointKindNames()
{
    static const std::vector<std::string> theNames = {"At Coordinates", "Axis And Plane"};
    return theNames;
}

std::string PointKindName(PointKind theKind)
{
    const std::size_t index = static_cast<std::size_t>(theKind);
    return index < PointKindNames().size() ? PointKindNames()[index] : std::string("At Coordinates");
}

bool ParsePointKind(const std::string& theText, PointKind& theKind)
{
    int index = 0;
    if (!ParseKind(PointKindNames(), theText, index)) {
        return false;
    }
    theKind = static_cast<PointKind>(index);
    return true;
}

ConstructionPointFeature::ConstructionPointFeature(const gp_Pnt& thePosition)
    : myPosition(thePosition)
{
}

bool ConstructionPointFeature::Build(const ComputeContext& theContext,
                                     gp_Pnt&               theResult,
                                     std::string&          theError) const
{
    switch (myKind) {
        case PointKind::AtCoordinates:
            theResult = myPosition;
            return true;

        case PointKind::AxisPlaneIntersection: {
            gp_Ax1 axis;
            gp_Ax3 plane;
            if (!ResolveAxisByName(theContext, myAxisName, axis)) {
                theError = "no axis called '" + myAxisName + "' earlier in the timeline";
                return false;
            }
            if (!ResolvePlaneByName(theContext, myPlaneName, plane)) {
                theError = "no plane called '" + myPlaneName + "' earlier in the timeline";
                return false;
            }

            const double along = gp_Vec(axis.Direction()).Dot(gp_Vec(plane.Direction()));
            if (std::fabs(along) <= kMinSpan) {
                theError = "that axis runs along the plane, so it never crosses it";
                return false;
            }
            const gp_Vec toPlane(axis.Location(), plane.Location());
            const double distance = toPlane.Dot(gp_Vec(plane.Direction())) / along;
            theResult = axis.Location().Translated(gp_Vec(axis.Direction()) * distance);
            return true;
        }
    }
    theError = "unknown point type";
    return false;
}

bool ConstructionPointFeature::Compute(const ComputeContext& theContext,
                                       const TopoDS_Shape&   theInput,
                                       TopoDS_Shape&         theOutput,
                                       std::string&          theError)
{
    theOutput = theInput;
    myIsValid = false;
    try {
        gp_Pnt result;
        if (!Build(theContext, result, theError)) {
            return false;
        }
        myResult = result;
        myIsValid = true;
    } catch (const Standard_Failure& failure) {
        const Standard_CString message = failure.GetMessageString();
        theError = message != nullptr && message[0] != '\0' ? message : "could not build the point";
        return false;
    }
    return true;
}

std::unique_ptr<Feature> ConstructionPointFeature::Clone() const
{
    auto copy = std::make_unique<ConstructionPointFeature>();
    copy->myKind = myKind;
    copy->myPosition = myPosition;
    copy->myAxisName = myAxisName;
    copy->myPlaneName = myPlaneName;
    copy->myResult = myResult;
    copy->myIsValid = myIsValid;
    CopyBaseTo(*copy);
    return copy;
}

std::vector<Parameter> ConstructionPointFeature::Parameters() const
{
    std::vector<Parameter> parameters;
    parameters.push_back(Parameter::MakeString("Type", PointKindName(myKind)));
    if (myKind == PointKind::AtCoordinates) {
        parameters.push_back(Parameter::MakeDouble("X", myPosition.X()));
        parameters.push_back(Parameter::MakeDouble("Y", myPosition.Y()));
        parameters.push_back(Parameter::MakeDouble("Z", myPosition.Z()));
    } else {
        parameters.push_back(Parameter::MakeString("Axis", myAxisName));
        parameters.push_back(Parameter::MakeString("Plane", myPlaneName));
    }
    return parameters;
}

bool ConstructionPointFeature::SetParameter(const Parameter& theParameter)
{
    if (theParameter.name == "Type") {
        PointKind kind = myKind;
        if (!ParsePointKind(theParameter.stringValue, kind)) {
            return false;
        }
        myKind = kind;
        return true;
    }
    if (theParameter.name == "Axis") {
        myAxisName = theParameter.stringValue;
        return true;
    }
    if (theParameter.name == "Plane") {
        myPlaneName = theParameter.stringValue;
        return true;
    }
    if (theParameter.name == "X") {
        myPosition.SetX(theParameter.doubleValue);
        return true;
    }
    if (theParameter.name == "Y") {
        myPosition.SetY(theParameter.doubleValue);
        return true;
    }
    if (theParameter.name == "Z") {
        myPosition.SetZ(theParameter.doubleValue);
        return true;
    }
    return false;
}

bool ConstructionPointFeature::AsPoint(gp_Pnt& thePoint) const
{
    if (!myIsValid) {
        return false;
    }
    thePoint = myResult;
    return true;
}

} // namespace lcad
