#include "io/NativeFormat.h"

#include "core/Expression.h"
#include "core/GeometryRef.h"
#include "core/ProfileProvider.h"
#include "core/ShapeFeature.h"
#include "features/CombineFeature.h"
#include "features/ConstructionFeatures.h"
#include "features/LoftFeature.h"
#include "features/ModifyFeatures.h"
#include "features/PatternFeatures.h"
#include "features/PressPullFeature.h"
#include "features/PrimitiveFeatures.h"
#include "features/ProfileFeatures.h"
#include "features/SweepFeature.h"
#include "gizmos/TransformFeature.h"
#include "sketch/SketchFeature.h"

#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <Standard_Failure.hxx>
#include <gp_Ax3.hxx>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSaveFile>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <locale>
#include <locale.h>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <typeindex>
#include <typeinfo>
#include <utility>

namespace lcad {

const char* const kNativeFileSuffix = "pcad";

namespace {

constexpr const char* kFormatName = "penguincad";

// ---- numbers that do not depend on where the user lives ----

// The JSON numbers are Qt's own and never touch the C locale. But several
// features spell their picks into their reflected string rows with printf
// ("Body1|12.5,0,7.25|1000"), and Qt sets the C locale from the desktop
// at startup, so under a German locale those rows would read "12,5" -- a
// file whose bytes depend on who saved it, and one those features cannot
// even parse back. uselocale() swaps THIS thread alone to the C numeric
// rules for as long as a read or write runs, and puts back what it found.
class ClassicNumbers
{
public:
    ClassicNumbers()
        : myLocale(newlocale(LC_NUMERIC_MASK, "C", static_cast<locale_t>(0)))
    {
        if (myLocale != static_cast<locale_t>(0)) {
            myPrevious = uselocale(myLocale);
        }
    }

    ~ClassicNumbers()
    {
        if (myLocale != static_cast<locale_t>(0)) {
            uselocale(myPrevious);
            freelocale(myLocale);
        }
    }

    ClassicNumbers(const ClassicNumbers&) = delete;
    ClassicNumbers& operator=(const ClassicNumbers&) = delete;

private:
    locale_t myLocale = static_cast<locale_t>(0);
    locale_t myPrevious = static_cast<locale_t>(0);
};

// ---- writing helpers ----

QString Text(const std::string& theText)
{
    return QString::fromStdString(theText);
}

QJsonArray Array2(double theX, double theY)
{
    return QJsonArray{theX, theY};
}

QJsonArray Array3(const gp_XYZ& theXYZ)
{
    return QJsonArray{theXYZ.X(), theXYZ.Y(), theXYZ.Z()};
}

// ---- reading helpers ----
//
// Every one of these names WHERE in the file it was looking, so a message
// reads "features[3] (Extrude1): parameters: \"Distance\" must be a number"
// rather than leaving the user to guess which of forty features broke.

bool Fail(std::string& theError, const std::string& theWhere, const std::string& theWhat)
{
    theError = theWhere.empty() ? theWhat : theWhere + ": " + theWhat;
    return false;
}

// A key this version does not know is refused, not skipped: it was written
// by something that meant it, and quietly dropping it would reopen a
// different design from the one that was saved. A format change that an
// older reader would misread takes a new version number instead.
bool OnlyKeys(const QJsonObject&                  theObject,
              std::initializer_list<const char*>  theAllowed,
              const std::string&                  theWhere,
              std::string&                        theError)
{
    for (auto it = theObject.begin(); it != theObject.end(); ++it) {
        bool known = false;
        for (const char* allowed : theAllowed) {
            known = known || it.key() == QLatin1String(allowed);
        }
        if (!known) {
            return Fail(theError, theWhere, "unknown key \"" + it.key().toStdString() + "\"");
        }
    }
    return true;
}

bool GetString(const QJsonObject& theObject, const char* theKey, std::string& theValue,
               const std::string& theWhere, std::string& theError)
{
    const QJsonValue value = theObject.value(QLatin1String(theKey));
    if (!value.isString()) {
        return Fail(theError, theWhere, std::string("\"") + theKey + "\" must be a string");
    }
    theValue = value.toString().toStdString();
    return true;
}

bool GetBool(const QJsonObject& theObject, const char* theKey, bool& theValue,
             const std::string& theWhere, std::string& theError)
{
    const QJsonValue value = theObject.value(QLatin1String(theKey));
    if (!value.isBool()) {
        return Fail(theError, theWhere, std::string("\"") + theKey + "\" must be true or false");
    }
    theValue = value.toBool();
    return true;
}

bool ToNumber(const QJsonValue& theValue, double& theNumber)
{
    if (!theValue.isDouble()) {
        return false;
    }
    theNumber = theValue.toDouble();
    return std::isfinite(theNumber);
}

bool ToInt(const QJsonValue& theValue, int& theNumber)
{
    double number = 0.0;
    if (!ToNumber(theValue, number) || number != std::floor(number)
        || number < std::numeric_limits<int>::min() || number > std::numeric_limits<int>::max()) {
        return false;
    }
    theNumber = static_cast<int>(number);
    return true;
}

bool GetNumber(const QJsonObject& theObject, const char* theKey, double& theValue,
               const std::string& theWhere, std::string& theError)
{
    if (!ToNumber(theObject.value(QLatin1String(theKey)), theValue)) {
        return Fail(theError, theWhere, std::string("\"") + theKey + "\" must be a number");
    }
    return true;
}

bool GetInt(const QJsonObject& theObject, const char* theKey, int& theValue,
            const std::string& theWhere, std::string& theError)
{
    if (!ToInt(theObject.value(QLatin1String(theKey)), theValue)) {
        return Fail(theError, theWhere, std::string("\"") + theKey + "\" must be a whole number");
    }
    return true;
}

bool GetObject(const QJsonObject& theObject, const char* theKey, QJsonObject& theValue,
               const std::string& theWhere, std::string& theError)
{
    const QJsonValue value = theObject.value(QLatin1String(theKey));
    if (!value.isObject()) {
        return Fail(theError, theWhere, std::string("\"") + theKey + "\" must be an object");
    }
    theValue = value.toObject();
    return true;
}

bool GetArray(const QJsonObject& theObject, const char* theKey, QJsonArray& theValue,
              const std::string& theWhere, std::string& theError)
{
    const QJsonValue value = theObject.value(QLatin1String(theKey));
    if (!value.isArray()) {
        return Fail(theError, theWhere, std::string("\"") + theKey + "\" must be a list");
    }
    theValue = value.toArray();
    return true;
}

bool ToNumbers(const QJsonValue& theValue, std::size_t theCount, double* theNumbers)
{
    const QJsonArray array = theValue.toArray();
    if (!theValue.isArray() || static_cast<std::size_t>(array.size()) != theCount) {
        return false;
    }
    for (std::size_t i = 0; i < theCount; ++i) {
        if (!ToNumber(array.at(static_cast<int>(i)), theNumbers[i])) {
            return false;
        }
    }
    return true;
}

bool GetPoint2d(const QJsonObject& theObject, const char* theKey, gp_Pnt2d& thePoint,
                const std::string& theWhere, std::string& theError)
{
    double xy[2];
    if (!ToNumbers(theObject.value(QLatin1String(theKey)), 2, xy)) {
        return Fail(theError, theWhere, std::string("\"") + theKey + "\" must be [x, y]");
    }
    thePoint.SetCoord(xy[0], xy[1]);
    return true;
}

bool GetXYZ(const QJsonObject& theObject, const char* theKey, gp_XYZ& theXYZ,
            const std::string& theWhere, std::string& theError)
{
    double xyz[3];
    if (!ToNumbers(theObject.value(QLatin1String(theKey)), 3, xyz)) {
        return Fail(theError, theWhere, std::string("\"") + theKey + "\" must be [x, y, z]");
    }
    theXYZ.SetCoord(xyz[0], xyz[1], xyz[2]);
    return true;
}

// ---- units ----

// The canonical symbols only. A file is not something a person typed into
// a field, so "inches" and "\"" are not accepted as spellings of "in".
const std::vector<LengthUnit>& AllLengthUnits()
{
    static const std::vector<LengthUnit> units = {LengthUnit::Millimeter, LengthUnit::Centimeter,
                                                  LengthUnit::Meter, LengthUnit::Inch,
                                                  LengthUnit::Foot};
    return units;
}

bool LengthUnitFromSymbol(const std::string& theSymbol, LengthUnit& theUnit)
{
    for (const LengthUnit unit : AllLengthUnits()) {
        if (SymbolOf(unit) == theSymbol) {
            theUnit = unit;
            return true;
        }
    }
    return false;
}

std::string UnitSymbolOf(const UserParameter& theRow)
{
    switch (theRow.kind) {
        case UnitKind::Length:   return SymbolOf(theRow.lengthUnit);
        case UnitKind::Angle:    return SymbolOf(theRow.angleUnit);
        case UnitKind::Unitless: return std::string();
    }
    return std::string();
}

bool UnitFromSymbol(const std::string& theSymbol, UserParameter& theRow)
{
    if (theSymbol.empty()) {
        theRow.kind = UnitKind::Unitless;
        return true;
    }
    if (LengthUnitFromSymbol(theSymbol, theRow.lengthUnit)) {
        theRow.kind = UnitKind::Length;
        return true;
    }
    for (const AngleUnit unit : {AngleUnit::Degree, AngleUnit::Radian}) {
        if (SymbolOf(unit) == theSymbol) {
            theRow.kind = UnitKind::Angle;
            theRow.angleUnit = unit;
            return true;
        }
    }
    return false;
}

// ---- stable names for the enums a file spells out ----
//
// Spelled here rather than borrowed from the UI's own labels, which exist
// to be read and may be reworded; a file written today has to open after
// that happens.

template <typename Enum>
struct Names
{
    std::vector<std::pair<Enum, const char*>> entries;

    const char* NameOf(Enum theValue) const
    {
        for (const auto& entry : entries) {
            if (entry.first == theValue) {
                return entry.second;
            }
        }
        return "";
    }

    bool Parse(const std::string& theName, Enum& theValue) const
    {
        for (const auto& entry : entries) {
            if (theName == entry.second) {
                theValue = entry.first;
                return true;
            }
        }
        return false;
    }
};

const Names<SketchEntity::Kind>& EntityKinds()
{
    using K = SketchEntity::Kind;
    static const Names<K> names{{{K::Line, "Line"},
                                 {K::Circle, "Circle"},
                                 {K::Arc, "Arc"},
                                 {K::Ellipse, "Ellipse"},
                                 {K::Spline, "Spline"},
                                 {K::ControlPointSpline, "ControlPointSpline"},
                                 {K::Conic, "Conic"},
                                 {K::Point, "Point"}}};
    return names;
}

const Names<SketchConstraintType>& ConstraintTypes()
{
    using T = SketchConstraintType;
    static const Names<T> names{{{T::Coincident, "Coincident"},
                                 {T::Horizontal, "Horizontal"},
                                 {T::Vertical, "Vertical"},
                                 {T::Parallel, "Parallel"},
                                 {T::Perpendicular, "Perpendicular"},
                                 {T::Equal, "Equal"},
                                 {T::Tangent, "Tangent"},
                                 {T::Midpoint, "Midpoint"},
                                 {T::Concentric, "Concentric"},
                                 {T::Collinear, "Collinear"},
                                 {T::Fix, "Fix"},
                                 {T::Symmetric, "Symmetric"},
                                 {T::Distance, "Distance"},
                                 {T::DistanceX, "DistanceX"},
                                 {T::DistanceY, "DistanceY"},
                                 {T::Radius, "Radius"},
                                 {T::Diameter, "Diameter"},
                                 {T::Angle, "Angle"}}};
    return names;
}

const Names<SketchPointRole>& PointRoles()
{
    using R = SketchPointRole;
    static const Names<R> names{
        {{R::Whole, "Whole"}, {R::Start, "Start"}, {R::End, "End"}, {R::Centre, "Centre"}}};
    return names;
}

const Names<EntityType>& PickTypes()
{
    static const Names<EntityType> names{{{EntityType::BRepFace, "BRepFace"},
                                          {EntityType::BRepEdge, "BRepEdge"},
                                          {EntityType::BRepVertex, "BRepVertex"}}};
    return names;
}

// ---- references a feature holds ----

QJsonObject WriteProfileRef(const ProfileRef& theRef)
{
    QJsonArray boundary;
    for (const int id : theRef.boundary) {
        boundary.append(id);
    }
    QJsonObject object;
    object["boundary"] = boundary;
    object["seed"] = Array2(theRef.seed.X(), theRef.seed.Y());
    return object;
}

bool ReadProfileRef(const QJsonValue& theValue, ProfileRef& theRef, const std::string& theWhere,
                    std::string& theError)
{
    if (!theValue.isObject()) {
        return Fail(theError, theWhere, "a profile must be an object");
    }
    const QJsonObject object = theValue.toObject();
    QJsonArray boundary;
    if (!OnlyKeys(object, {"boundary", "seed"}, theWhere, theError)
        || !GetArray(object, "boundary", boundary, theWhere, theError)
        || !GetPoint2d(object, "seed", theRef.seed, theWhere, theError)) {
        return false;
    }
    theRef.boundary.clear();
    for (const QJsonValue& id : boundary) {
        int value = 0;
        if (!ToInt(id, value)) {
            return Fail(theError, theWhere, "a profile boundary lists curve ids, whole numbers");
        }
        theRef.boundary.push_back(value);
    }
    if (theRef.boundary.empty()) {
        return Fail(theError, theWhere, "a profile boundary names no curve");
    }
    return true;
}

QJsonObject WriteBodyRef(const CombineBodyRef& theRef)
{
    QJsonObject object;
    object["name"] = Text(theRef.name);
    object["centre"] = Array3(theRef.centre.XYZ());
    object["volume"] = theRef.volume;
    return object;
}

bool ReadBodyRef(const QJsonValue& theValue, CombineBodyRef& theRef, const std::string& theWhere,
                 std::string& theError)
{
    if (!theValue.isObject()) {
        return Fail(theError, theWhere, "a body pick must be an object");
    }
    const QJsonObject object = theValue.toObject();
    gp_XYZ centre;
    if (!OnlyKeys(object, {"name", "centre", "volume"}, theWhere, theError)
        || !GetString(object, "name", theRef.name, theWhere, theError)
        || !GetXYZ(object, "centre", centre, theWhere, theError)
        || !GetNumber(object, "volume", theRef.volume, theWhere, theError)) {
        return false;
    }
    theRef.centre = gp_Pnt(centre);
    return true;
}

bool ReadBodyRefs(const QJsonObject& theExtra, const char* theKey,
                  std::vector<CombineBodyRef>& theRefs, const std::string& theWhere,
                  std::string& theError)
{
    QJsonArray array;
    if (!GetArray(theExtra, theKey, array, theWhere, theError)) {
        return false;
    }
    theRefs.clear();
    for (const QJsonValue& value : array) {
        CombineBodyRef ref;
        if (!ReadBodyRef(value, ref, theWhere + "." + theKey, theError)) {
            return false;
        }
        theRefs.push_back(ref);
    }
    return true;
}

QJsonArray WriteBodyRefs(const std::vector<CombineBodyRef>& theRefs)
{
    QJsonArray array;
    for (const CombineBodyRef& ref : theRefs) {
        array.append(WriteBodyRef(ref));
    }
    return array;
}

// ---- extra blocks, one per feature family ----
//
// Each writes what the reflection does not carry exactly, and each reader
// puts it back through the setter the app's own commands use.

QJsonObject WriteProfiles(const Feature& theFeature)
{
    QJsonArray profiles;
    for (const ProfileRef& ref : static_cast<const ProfileFeature&>(theFeature).Profiles()) {
        profiles.append(WriteProfileRef(ref));
    }
    QJsonObject extra;
    extra["profiles"] = profiles;
    return extra;
}

bool ReadProfiles(Feature& theFeature, const QJsonObject& theExtra, std::string& theError)
{
    QJsonArray array;
    if (!OnlyKeys(theExtra, {"profiles"}, "extra", theError)
        || !GetArray(theExtra, "profiles", array, "extra", theError)) {
        return false;
    }
    std::vector<ProfileRef> refs;
    for (const QJsonValue& value : array) {
        ProfileRef ref;
        if (!ReadProfileRef(value, ref, "extra.profiles", theError)) {
            return false;
        }
        refs.push_back(ref);
    }
    // After the Sketch row has been applied: naming a sketch clears the
    // picks, since they number THAT sketch's curves.
    static_cast<ProfileFeature&>(theFeature).SetProfiles(std::move(refs));
    return true;
}

QJsonObject WriteLoft(const Feature& theFeature)
{
    QJsonArray sections;
    for (const LoftSection& section : static_cast<const LoftFeature&>(theFeature).Sections()) {
        QJsonObject object;
        object["sketch"] = Text(section.sketch);
        if (!section.profile.IsNull()) {
            object["profile"] = WriteProfileRef(section.profile);
        }
        sections.append(object);
    }
    QJsonObject extra;
    extra["sections"] = sections;
    return extra;
}

bool ReadLoft(Feature& theFeature, const QJsonObject& theExtra, std::string& theError)
{
    QJsonArray array;
    if (!OnlyKeys(theExtra, {"sections"}, "extra", theError)
        || !GetArray(theExtra, "sections", array, "extra", theError)) {
        return false;
    }
    std::vector<LoftSection> sections;
    for (const QJsonValue& value : array) {
        const std::string where = "extra.sections";
        if (!value.isObject()) {
            return Fail(theError, where, "a section must be an object");
        }
        const QJsonObject object = value.toObject();
        LoftSection section;
        if (!OnlyKeys(object, {"sketch", "profile"}, where, theError)
            || !GetString(object, "sketch", section.sketch, where, theError)) {
            return false;
        }
        if (object.contains("profile")
            && !ReadProfileRef(object.value("profile"), section.profile, where, theError)) {
            return false;
        }
        sections.push_back(section);
    }
    static_cast<LoftFeature&>(theFeature).SetSections(std::move(sections));
    return true;
}

QJsonObject WriteCombine(const Feature& theFeature)
{
    const auto& combine = static_cast<const CombineFeature&>(theFeature);
    QJsonObject extra;
    extra["target"] = WriteBodyRef(combine.Target());
    extra["tools"] = WriteBodyRefs(combine.Tools());
    return extra;
}

bool ReadCombine(Feature& theFeature, const QJsonObject& theExtra, std::string& theError)
{
    auto& combine = static_cast<CombineFeature&>(theFeature);
    CombineBodyRef target;
    std::vector<CombineBodyRef> tools;
    if (!OnlyKeys(theExtra, {"target", "tools"}, "extra", theError)
        || !ReadBodyRef(theExtra.value("target"), target, "extra.target", theError)
        || !ReadBodyRefs(theExtra, "tools", tools, "extra", theError)) {
        return false;
    }
    combine.SetTarget(target);
    combine.SetTools(std::move(tools));
    return true;
}

QJsonObject WritePattern(const Feature& theFeature)
{
    QJsonObject extra;
    extra["bodies"] = WriteBodyRefs(static_cast<const PatternFeature&>(theFeature).Bodies());
    return extra;
}

bool ReadPattern(Feature& theFeature, const QJsonObject& theExtra, std::string& theError)
{
    std::vector<CombineBodyRef> bodies;
    if (!OnlyKeys(theExtra, {"bodies"}, "extra", theError)
        || !ReadBodyRefs(theExtra, "bodies", bodies, "extra", theError)) {
        return false;
    }
    static_cast<PatternFeature&>(theFeature).SetBodies(std::move(bodies));
    return true;
}

QJsonObject WriteMove(const Feature& theFeature)
{
    QJsonObject extra;
    extra["body"] = WriteBodyRef(static_cast<const TransformFeature&>(theFeature).Target());
    return extra;
}

bool ReadMove(Feature& theFeature, const QJsonObject& theExtra, std::string& theError)
{
    CombineBodyRef target;
    if (!OnlyKeys(theExtra, {"body"}, "extra", theError)
        || !ReadBodyRef(theExtra.value("body"), target, "extra.body", theError)) {
        return false;
    }
    static_cast<TransformFeature&>(theFeature).SetTarget(target);
    return true;
}

QJsonObject WritePressPull(const Feature& theFeature)
{
    QJsonArray faces;
    for (const GeometryRef& ref : static_cast<const PressPullFeature&>(theFeature).Faces()) {
        QJsonObject object;
        object["type"] = PickTypes().NameOf(ref.type);
        object["body"] = Text(ref.body);
        object["point"] = Array3(ref.point.XYZ());
        object["measure"] = ref.measure;
        faces.append(object);
    }
    QJsonObject extra;
    extra["faces"] = faces;
    return extra;
}

bool ReadPressPull(Feature& theFeature, const QJsonObject& theExtra, std::string& theError)
{
    QJsonArray array;
    if (!OnlyKeys(theExtra, {"faces"}, "extra", theError)
        || !GetArray(theExtra, "faces", array, "extra", theError)) {
        return false;
    }
    std::vector<GeometryRef> faces;
    for (const QJsonValue& value : array) {
        const std::string where = "extra.faces";
        if (!value.isObject()) {
            return Fail(theError, where, "a face pick must be an object");
        }
        const QJsonObject object = value.toObject();
        GeometryRef ref;
        std::string type;
        gp_XYZ point;
        if (!OnlyKeys(object, {"type", "body", "point", "measure"}, where, theError)
            || !GetString(object, "type", type, where, theError)
            || !GetString(object, "body", ref.body, where, theError)
            || !GetXYZ(object, "point", point, where, theError)
            || !GetNumber(object, "measure", ref.measure, where, theError)) {
            return false;
        }
        if (!PickTypes().Parse(type, ref.type)) {
            return Fail(theError, where, "\"" + type + "\" is not a face, edge or vertex");
        }
        ref.point = gp_Pnt(point);
        faces.push_back(ref);
    }
    static_cast<PressPullFeature&>(theFeature).SetFaces(std::move(faces));
    return true;
}

// -- sketches --

QJsonObject WriteEntity(const SketchEntity& theEntity)
{
    // Only what differs from a fresh entity is written. A line has no
    // radius and a circle no second point, and spelling out zeros for them
    // would bury the numbers that matter; leaving them out loses nothing,
    // since a missing field reads back as exactly that default.
    const SketchEntity blank;
    QJsonObject object;
    object["id"] = theEntity.id;
    object["kind"] = EntityKinds().NameOf(theEntity.kind);
    if (theEntity.isConstruction) {
        object["construction"] = true;
    }
    const auto point = [&object](const char* theKey, const gp_Pnt2d& theValue,
                                 const gp_Pnt2d& theDefault) {
        if (theValue.X() != theDefault.X() || theValue.Y() != theDefault.Y()) {
            object[theKey] = Array2(theValue.X(), theValue.Y());
        }
    };
    const auto number = [&object](const char* theKey, double theValue, double theDefault) {
        if (theValue != theDefault) {
            object[theKey] = theValue;
        }
    };
    point("first", theEntity.first, blank.first);
    point("second", theEntity.second, blank.second);
    number("radius", theEntity.radius, blank.radius);
    number("minorRadius", theEntity.minorRadius, blank.minorRadius);
    number("rotation", theEntity.rotation, blank.rotation);
    number("startAngle", theEntity.startAngle, blank.startAngle);
    number("endAngle", theEntity.endAngle, blank.endAngle);
    if (!theEntity.points.empty()) {
        QJsonArray points;
        for (const gp_Pnt2d& p : theEntity.points) {
            points.append(Array2(p.X(), p.Y()));
        }
        object["points"] = points;
    }
    number("rho", theEntity.rho, blank.rho);
    number("trimFirst", theEntity.trimFirst, blank.trimFirst);
    number("trimLast", theEntity.trimLast, blank.trimLast);
    return object;
}

bool ReadEntity(const QJsonValue& theValue, SketchEntity& theEntity, std::string& theError)
{
    const std::string where = "extra.entities";
    if (!theValue.isObject()) {
        return Fail(theError, where, "a curve must be an object");
    }
    const QJsonObject object = theValue.toObject();
    if (!OnlyKeys(object,
                  {"id", "kind", "construction", "first", "second", "radius", "minorRadius",
                   "rotation", "startAngle", "endAngle", "points", "rho", "trimFirst", "trimLast"},
                  where, theError)) {
        return false;
    }
    std::string kind;
    if (!GetInt(object, "id", theEntity.id, where, theError)
        || !GetString(object, "kind", kind, where, theError)) {
        return false;
    }
    const std::string at = where + " (curve " + std::to_string(theEntity.id) + ")";
    if (!EntityKinds().Parse(kind, theEntity.kind)) {
        return Fail(theError, at, "\"" + kind + "\" is not a kind of sketch curve");
    }
    if (object.contains("construction")
        && !GetBool(object, "construction", theEntity.isConstruction, at, theError)) {
        return false;
    }
    const std::pair<const char*, gp_Pnt2d*> points[] = {{"first", &theEntity.first},
                                                          {"second", &theEntity.second}};
    for (const auto& field : points) {
        if (object.contains(field.first)
            && !GetPoint2d(object, field.first, *field.second, at, theError)) {
            return false;
        }
    }
    const std::pair<const char*, double*> numbers[] = {
        {"radius", &theEntity.radius},         {"minorRadius", &theEntity.minorRadius},
        {"rotation", &theEntity.rotation},     {"startAngle", &theEntity.startAngle},
        {"endAngle", &theEntity.endAngle},     {"rho", &theEntity.rho},
        {"trimFirst", &theEntity.trimFirst},   {"trimLast", &theEntity.trimLast}};
    for (const auto& field : numbers) {
        if (object.contains(field.first)
            && !GetNumber(object, field.first, *field.second, at, theError)) {
            return false;
        }
    }
    if (object.contains("points")) {
        QJsonArray points;
        if (!GetArray(object, "points", points, at, theError)) {
            return false;
        }
        for (const QJsonValue& point : points) {
            double xy[2];
            if (!ToNumbers(point, 2, xy)) {
                return Fail(theError, at, "\"points\" must be a list of [x, y]");
            }
            theEntity.points.emplace_back(xy[0], xy[1]);
        }
    }
    return true;
}

QJsonObject WritePointRef(const SketchPointRef& theRef)
{
    QJsonObject object;
    object["entity"] = theRef.entity;
    object["role"] = PointRoles().NameOf(theRef.role);
    return object;
}

QJsonObject WriteConstraint(const SketchConstraint& theConstraint)
{
    const SketchConstraint blank;
    QJsonObject object;
    object["id"] = theConstraint.id;
    object["type"] = ConstraintTypes().NameOf(theConstraint.type);
    // An operand the constraint does not use is left out, like an unused
    // field of a curve.
    const auto operand = [&object](const char* theKey, const SketchPointRef& theRef) {
        if (theRef.entity != 0 || theRef.role != SketchPointRole::Whole) {
            object[theKey] = WritePointRef(theRef);
        }
    };
    operand("a", theConstraint.a);
    operand("b", theConstraint.b);
    operand("c", theConstraint.c);
    if (theConstraint.value != blank.value) {
        // Internal units: millimetres, and RADIANS for an angle -- the
        // number the solver uses, not the degrees the panel shows.
        object["value"] = theConstraint.value;
    }
    if (!theConstraint.label.empty()) {
        object["label"] = Text(theConstraint.label);
    }
    if (theConstraint.labelPosition.X() != 0.0 || theConstraint.labelPosition.Y() != 0.0) {
        object["labelPosition"] =
            Array2(theConstraint.labelPosition.X(), theConstraint.labelPosition.Y());
    }
    return object;
}

bool ReadConstraint(const QJsonValue& theValue, SketchConstraint& theConstraint,
                    std::string& theError)
{
    const std::string where = "extra.constraints";
    if (!theValue.isObject()) {
        return Fail(theError, where, "a constraint must be an object");
    }
    const QJsonObject object = theValue.toObject();
    std::string type;
    if (!OnlyKeys(object, {"id", "type", "a", "b", "c", "value", "label", "labelPosition"}, where,
                  theError)
        || !GetInt(object, "id", theConstraint.id, where, theError)
        || !GetString(object, "type", type, where, theError)) {
        return false;
    }
    const std::string at = where + " (constraint " + std::to_string(theConstraint.id) + ")";
    if (!ConstraintTypes().Parse(type, theConstraint.type)) {
        return Fail(theError, at, "\"" + type + "\" is not a kind of sketch constraint");
    }
    const std::pair<const char*, SketchPointRef*> operands[] = {
        {"a", &theConstraint.a}, {"b", &theConstraint.b}, {"c", &theConstraint.c}};
    for (const auto& operand : operands) {
        if (!object.contains(operand.first)) {
            continue;
        }
        QJsonObject ref;
        std::string role;
        if (!GetObject(object, operand.first, ref, at, theError)
            || !OnlyKeys(ref, {"entity", "role"}, at, theError)
            || !GetInt(ref, "entity", operand.second->entity, at, theError)
            || !GetString(ref, "role", role, at, theError)) {
            return false;
        }
        if (!PointRoles().Parse(role, operand.second->role)) {
            return Fail(theError, at, "\"" + role + "\" is not a point of a curve");
        }
    }
    if (object.contains("value") && !GetNumber(object, "value", theConstraint.value, at, theError)) {
        return false;
    }
    if (object.contains("label") && !GetString(object, "label", theConstraint.label, at, theError)) {
        return false;
    }
    if (object.contains("labelPosition")
        && !GetPoint2d(object, "labelPosition", theConstraint.labelPosition, at, theError)) {
        return false;
    }
    return true;
}

// A frame with exactly these axes. gp_Ax3's constructors re-derive X and Y
// from the normal, and gp_Dir renormalises whatever it is given, which
// moves a saved plane in its last digit most of the time -- 93 of every
// 100 random frames did not survive the trip -- so a sketch on an angled
// face would change the file on every save after a reopen. The accessors
// hand back references to the stored axes, so the frame is put back by
// writing through them, once the numbers have been checked to be the unit,
// square, right-handed frame a gp_Ax3 promises.
bool ExactFrame(const gp_XYZ& theOrigin, const gp_XYZ& theNormal, const gp_XYZ& theX,
                const gp_XYZ& theY, gp_Ax3& theFrame, std::string& theError)
{
    constexpr double kSlack = 1.0e-9;
    const auto unit = [](const gp_XYZ& theVector) {
        return std::fabs(theVector.Modulus() - 1.0) <= kSlack;
    };
    if (!unit(theNormal) || !unit(theX) || !unit(theY)
        || std::fabs(theNormal.Dot(theX)) > kSlack || std::fabs(theNormal.Dot(theY)) > kSlack
        || std::fabs(theX.Dot(theY)) > kSlack || theX.Crossed(theY).Dot(theNormal) <= 0.0) {
        theError = "extra.plane: the normal, xDirection and yDirection are not a right-handed "
                   "set of unit axes at right angles";
        return false;
    }
    theFrame = gp_Ax3(gp_Pnt(theOrigin), gp_Dir(theNormal), gp_Dir(theX));
    const_cast<gp_Pnt&>(theFrame.Location()).SetXYZ(theOrigin);
    const_cast<gp_XYZ&>(theFrame.Direction().XYZ()) = theNormal;
    const_cast<gp_XYZ&>(theFrame.XDirection().XYZ()) = theX;
    const_cast<gp_XYZ&>(theFrame.YDirection().XYZ()) = theY;
    return true;
}

QJsonObject WriteSketch(const Feature& theFeature)
{
    const auto& sketch = static_cast<const SketchFeature&>(theFeature);

    // The plane as the sketch was started on it, before the Offset row
    // pushes it along its normal. All four vectors, so it comes back exact.
    const gp_Ax3& position = sketch.PlanePosition();
    QJsonObject plane;
    plane["origin"] = Array3(position.Location().XYZ());
    plane["normal"] = Array3(position.Direction().XYZ());
    plane["xDirection"] = Array3(position.XDirection().XYZ());
    plane["yDirection"] = Array3(position.YDirection().XYZ());

    QJsonArray entities;
    for (const SketchEntity& entity : sketch.Entities()) {
        entities.append(WriteEntity(entity));
    }
    QJsonArray constraints;
    for (const SketchConstraint& constraint : sketch.Constraints()) {
        constraints.append(WriteConstraint(constraint));
    }

    QJsonObject extra;
    extra["plane"] = plane;
    extra["entities"] = entities;
    extra["constraints"] = constraints;
    extra["nextEntityId"] = sketch.NextEntityId();
    extra["nextConstraintId"] = sketch.NextConstraintId();
    extra["nextDimension"] = sketch.NextDimension();
    return extra;
}

bool ReadSketch(Feature& theFeature, const QJsonObject& theExtra, std::string& theError)
{
    QJsonObject plane;
    QJsonArray entityArray;
    QJsonArray constraintArray;
    int nextEntity = 0;
    int nextConstraint = 0;
    int nextDimension = 0;
    if (!OnlyKeys(theExtra,
                  {"plane", "entities", "constraints", "nextEntityId", "nextConstraintId",
                   "nextDimension"},
                  "extra", theError)
        || !GetObject(theExtra, "plane", plane, "extra", theError)
        || !GetArray(theExtra, "entities", entityArray, "extra", theError)
        || !GetArray(theExtra, "constraints", constraintArray, "extra", theError)
        || !GetInt(theExtra, "nextEntityId", nextEntity, "extra", theError)
        || !GetInt(theExtra, "nextConstraintId", nextConstraint, "extra", theError)
        || !GetInt(theExtra, "nextDimension", nextDimension, "extra", theError)) {
        return false;
    }

    gp_XYZ origin, normal, xDirection, yDirection;
    gp_Ax3 frame;
    if (!OnlyKeys(plane, {"origin", "normal", "xDirection", "yDirection"}, "extra.plane", theError)
        || !GetXYZ(plane, "origin", origin, "extra.plane", theError)
        || !GetXYZ(plane, "normal", normal, "extra.plane", theError)
        || !GetXYZ(plane, "xDirection", xDirection, "extra.plane", theError)
        || !GetXYZ(plane, "yDirection", yDirection, "extra.plane", theError)
        || !ExactFrame(origin, normal, xDirection, yDirection, frame, theError)) {
        return false;
    }

    std::vector<SketchEntity> entities;
    for (const QJsonValue& value : entityArray) {
        SketchEntity entity;
        if (!ReadEntity(value, entity, theError)) {
            return false;
        }
        entities.push_back(entity);
    }
    std::vector<SketchConstraint> constraints;
    for (const QJsonValue& value : constraintArray) {
        SketchConstraint constraint;
        if (!ReadConstraint(value, constraint, theError)) {
            return false;
        }
        constraints.push_back(constraint);
    }

    std::string error;
    if (!static_cast<SketchFeature&>(theFeature)
             .Restore(frame, std::move(entities), std::move(constraints), nextEntity,
                      nextConstraint, nextDimension, error)) {
        return Fail(theError, "extra", error);
    }
    return true;
}

// -- imported shapes --

// BRep text for a shape, remembered for the few most recently written. An
// imported part can run to megabytes of it, and the window writes the whole
// design after every edit to decide whether it is modified; writing the
// same unchanging import out each time would make every click wait for it.
// Keyed by the shape itself -- the same TShape, placed and oriented the
// same way -- which the cache holds on to, so the key cannot be recycled.
std::string BRepTextOf(const TopoDS_Shape& theShape)
{
    static std::mutex theLock;
    static std::vector<std::pair<TopoDS_Shape, std::string>> theCache;
    constexpr std::size_t kCacheSize = 4;

    std::lock_guard<std::mutex> guard(theLock);
    for (std::size_t i = 0; i < theCache.size(); ++i) {
        if (theCache[i].first.IsEqual(theShape)) {
            std::rotate(theCache.begin(), theCache.begin() + static_cast<std::ptrdiff_t>(i),
                        theCache.begin() + static_cast<std::ptrdiff_t>(i) + 1);
            return theCache.front().second;
        }
    }

    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    // No triangulation: the viewer meshes every displayed shape IN PLACE, so
    // with it the text would change the moment the part was first drawn --
    // and it is a cache the viewer rebuilds anyway. Version 2 stores the
    // curve-on-surface points that make a reread shape match the original.
    BRepTools::Write(theShape, stream, Standard_False, Standard_False,
                     TopTools_FormatVersion_VERSION_2);
    theCache.insert(theCache.begin(), std::make_pair(theShape, stream.str()));
    if (theCache.size() > kCacheSize) {
        theCache.pop_back();
    }
    return theCache.front().second;
}

QJsonObject WriteShape(const Feature& theFeature)
{
    const auto& shape = static_cast<const ShapeFeature&>(theFeature);
    QJsonObject extra;
    // What the timeline calls it ("Import"), which is not the file's type:
    // one class carries every shape with no recipe behind it.
    extra["label"] = Text(shape.TypeName());
    extra["brep"] = Text(shape.Shape().IsNull() ? std::string() : BRepTextOf(shape.Shape()));
    return extra;
}

FeaturePtr CreateShape(const QJsonObject& theExtra, std::string& theError)
{
    std::string label;
    std::string brep;
    if (!OnlyKeys(theExtra, {"label", "brep"}, "extra", theError)
        || !GetString(theExtra, "label", label, "extra", theError)
        || !GetString(theExtra, "brep", brep, "extra", theError)) {
        return nullptr;
    }
    TopoDS_Shape shape;
    if (!brep.empty()) {
        try {
            std::istringstream stream(brep);
            stream.imbue(std::locale::classic());
            BRep_Builder builder;
            BRepTools::Read(shape, stream, builder);
        } catch (const Standard_Failure& failure) {
            const char* message = failure.GetMessageString();
            theError = std::string("extra.brep: the shape could not be read (")
                       + (message != nullptr ? message : "OCCT error") + ")";
            return nullptr;
        }
        if (shape.IsNull()) {
            theError = "extra.brep: the shape could not be read";
            return nullptr;
        }
    }
    return std::make_shared<ShapeFeature>(shape, label);
}

// ---- the table: one row per feature class ----

using Factory = std::function<FeaturePtr(const QJsonObject&, std::string&)>;
using ExtraWriter = std::function<QJsonObject(const Feature&)>;
using ExtraReader = std::function<bool(Feature&, const QJsonObject&, std::string&)>;

struct FeatureFormat
{
    // The file's name for it. The same as the class's TypeName() (the test
    // holds them together) except for ShapeFeature, whose TypeName is a
    // label that varies from import to import.
    const char*     type;
    std::type_index cls;
    Factory         create;
    ExtraWriter     writeExtra;   // empty: the reflection carries it all
    ExtraReader     readExtra;

    // Reflected string rows that spell a pick as text rounded for the
    // properties panel. The exact pick is in the extra block, so on reading
    // these are left to it; they are still written, to be read and diffed.
    std::set<std::string> carriedByExtra;
};

template <typename T>
FeatureFormat Row(const char* theType, ExtraWriter theWriter = {}, ExtraReader theReader = {},
                  std::set<std::string> theCarried = {})
{
    return FeatureFormat{theType, std::type_index(typeid(T)),
                         [](const QJsonObject&, std::string&) -> FeaturePtr {
                             return std::make_shared<T>();
                         },
                         std::move(theWriter), std::move(theReader), std::move(theCarried)};
}

// Matched on the EXACT class, never through a base: a new feature derived
// from Extrude must get a row of its own, not be saved as a plain Extrude
// that quietly loses whatever it added.
const std::vector<FeatureFormat>& Formats()
{
    static const std::vector<FeatureFormat> formats = [] {
        std::vector<FeatureFormat> rows;
        rows.push_back(Row<SketchFeature>("Sketch", WriteSketch, ReadSketch));
        rows.push_back(Row<ExtrudeFeature>("Extrude", WriteProfiles, ReadProfiles, {"Profiles"}));
        rows.push_back(Row<RevolveFeature>("Revolve", WriteProfiles, ReadProfiles, {"Profiles"}));
        rows.push_back(Row<SweepFeature>("Sweep", WriteProfiles, ReadProfiles, {"Profiles"}));
        rows.push_back(Row<LoftFeature>("Loft", WriteLoft, ReadLoft, {"Sections"}));
        rows.push_back(Row<BoxFeature>("Box"));
        rows.push_back(Row<CylinderFeature>("Cylinder"));
        rows.push_back(Row<SphereFeature>("Sphere"));
        rows.push_back(Row<ConeFeature>("Cone"));
        rows.push_back(Row<TorusFeature>("Torus"));
        rows.push_back(Row<FilletFeature>("Fillet"));
        rows.push_back(Row<ChamferFeature>("Chamfer"));
        rows.push_back(Row<ShellFeature>("Shell"));
        rows.push_back(Row<PressPullFeature>("PressPull", WritePressPull, ReadPressPull, {"Faces"}));
        rows.push_back(Row<CombineFeature>("Combine", WriteCombine, ReadCombine, {"Target", "Tools"}));
        rows.push_back(Row<RectangularPatternFeature>("RectangularPattern", WritePattern,
                                                      ReadPattern, {"Bodies"}));
        rows.push_back(Row<CircularPatternFeature>("CircularPattern", WritePattern, ReadPattern,
                                                   {"Bodies"}));
        rows.push_back(Row<MirrorFeature>("Mirror", WritePattern, ReadPattern, {"Bodies"}));
        rows.push_back(Row<TransformFeature>("Move", WriteMove, ReadMove, {"Body"}));
        rows.push_back(Row<ConstructionPlaneFeature>("ConstructionPlane"));
        rows.push_back(Row<ConstructionAxisFeature>("ConstructionAxis"));
        rows.push_back(Row<ConstructionPointFeature>("ConstructionPoint"));

        FeatureFormat shape = Row<ShapeFeature>("Shape", WriteShape);
        shape.create = CreateShape;
        rows.push_back(shape);
        return rows;
    }();
    return formats;
}

const FeatureFormat* FormatOf(const Feature& theFeature)
{
    const std::type_index cls(typeid(theFeature));
    for (const FeatureFormat& format : Formats()) {
        if (format.cls == cls) {
            return &format;
        }
    }
    return nullptr;
}

const FeatureFormat* FormatNamed(const std::string& theType)
{
    for (const FeatureFormat& format : Formats()) {
        if (theType == format.type) {
            return &format;
        }
    }
    return nullptr;
}

// ---- one feature ----

bool WriteFeature(const Feature& theFeature, QJsonObject& theObject, std::string& theError)
{
    const FeatureFormat* format = FormatOf(theFeature);
    if (format == nullptr) {
        theError = theFeature.Name() + " is a " + theFeature.TypeName()
                   + ", which this version of PenguinCAD cannot save yet";
        return false;
    }

    QJsonObject parameters;
    for (const Parameter& parameter : theFeature.Parameters()) {
        const QString key = Text(parameter.name);
        if (parameters.contains(key)) {
            // Two rows of one name would collapse into one key, and the
            // file would reopen with only one of them.
            theError = theFeature.Name() + " has two parameters called " + parameter.name;
            return false;
        }
        switch (parameter.type) {
            case Parameter::Type::Double:
                if (!std::isfinite(parameter.doubleValue)) {
                    theError = theFeature.Name() + "'s " + parameter.name
                               + " is not a finite number, which a file cannot hold";
                    return false;
                }
                parameters[key] = parameter.doubleValue;
                break;
            case Parameter::Type::Int:    parameters[key] = parameter.intValue; break;
            case Parameter::Type::Bool:   parameters[key] = parameter.boolValue; break;
            case Parameter::Type::String: parameters[key] = Text(parameter.stringValue); break;
        }
    }

    // Everything the base class keeps, stale keys included: an expression
    // or a model name left behind by a deleted sketch dimension is part of
    // the design's state (the name stays retired), not litter to tidy away.
    QJsonObject expressions;
    for (const auto& entry : theFeature.Expressions()) {
        expressions[Text(entry.first)] = Text(entry.second);
    }
    QJsonObject modelNames;
    for (const auto& entry : theFeature.ModelNames()) {
        modelNames[Text(entry.first)] = Text(entry.second);
    }

    theObject = QJsonObject();
    theObject["type"] = format->type;
    theObject["name"] = Text(theFeature.Name());
    theObject["suppressed"] = theFeature.IsSuppressed();
    theObject["parameters"] = parameters;
    theObject["expressions"] = expressions;
    theObject["modelNames"] = modelNames;
    theObject["extra"] = format->writeExtra ? format->writeExtra(theFeature) : QJsonObject();
    return true;
}

// Push the saved parameter values into theFeature, in the order the feature
// itself lists its parameters -- not the file's, which is alphabetical --
// because some rows change what others mean: naming a profile's sketch
// clears its picks, and a construction plane's Type decides which rows it
// has at all. So this goes round until a pass applies nothing new; rows a
// feature only grows once its extra block is in (a sketch's dimensions)
// are picked up by the caller running it again afterwards.
//
// A value the feature already holds is not pushed: re-setting a sketch
// dimension marks the sketch for a solve, and a solve that was not needed
// still writes every curve back.
bool ApplyParameters(Feature& theFeature, std::map<std::string, QJsonValue>& thePending,
                     std::string& theError)
{
    bool progressed = true;
    while (progressed && !thePending.empty()) {
        progressed = false;
        for (const Parameter& current : theFeature.Parameters()) {
            const auto found = thePending.find(current.name);
            if (found == thePending.end()) {
                continue;
            }
            const QJsonValue value = found->second;
            const std::string where = "parameters: \"" + current.name + "\"";
            Parameter saved = current;
            bool same = false;
            switch (current.type) {
                case Parameter::Type::Double:
                    if (!ToNumber(value, saved.doubleValue)) {
                        return Fail(theError, where, "must be a number");
                    }
                    same = saved.doubleValue == current.doubleValue;
                    break;
                case Parameter::Type::Int:
                    if (!ToInt(value, saved.intValue)) {
                        return Fail(theError, where, "must be a whole number");
                    }
                    same = saved.intValue == current.intValue;
                    break;
                case Parameter::Type::Bool:
                    if (!value.isBool()) {
                        return Fail(theError, where, "must be true or false");
                    }
                    saved.boolValue = value.toBool();
                    same = saved.boolValue == current.boolValue;
                    break;
                case Parameter::Type::String:
                    if (!value.isString()) {
                        return Fail(theError, where, "must be a string");
                    }
                    saved.stringValue = value.toString().toStdString();
                    same = saved.stringValue == current.stringValue;
                    break;
            }
            thePending.erase(found);
            progressed = true;
            if (!same && !theFeature.SetParameter(saved)) {
                return Fail(theError, where, theFeature.TypeName() + " does not accept that value");
            }
        }
    }
    return true;
}

bool ReadFeature(const QJsonObject& theObject, FeaturePtr& theFeature, std::string& theError)
{
    std::string type;
    std::string name;
    bool suppressed = false;
    QJsonObject parameters;
    QJsonObject expressions;
    QJsonObject modelNames;
    QJsonObject extra;
    if (!OnlyKeys(theObject,
                  {"type", "name", "suppressed", "parameters", "expressions", "modelNames",
                   "extra"},
                  std::string(), theError)
        || !GetString(theObject, "type", type, std::string(), theError)
        || !GetString(theObject, "name", name, std::string(), theError)
        || !GetBool(theObject, "suppressed", suppressed, std::string(), theError)
        || !GetObject(theObject, "parameters", parameters, std::string(), theError)
        || !GetObject(theObject, "expressions", expressions, std::string(), theError)
        || !GetObject(theObject, "modelNames", modelNames, std::string(), theError)
        || !GetObject(theObject, "extra", extra, std::string(), theError)) {
        return false;
    }
    if (name.empty()) {
        return Fail(theError, std::string(), "a feature needs a name");
    }
    const FeatureFormat* format = FormatNamed(type);
    if (format == nullptr) {
        return Fail(theError, std::string(),
                    "\"" + type + "\" is not a kind of feature this version of PenguinCAD knows");
    }

    FeaturePtr feature = format->create(extra, theError);
    if (!feature) {
        return false;
    }
    feature->SetName(name);
    feature->SetSuppressed(suppressed);

    std::map<std::string, QJsonValue> pending;
    for (auto it = parameters.begin(); it != parameters.end(); ++it) {
        const std::string key = it.key().toStdString();
        if (format->carriedByExtra.count(key) == 0) {
            pending[key] = it.value();
        }
    }
    if (!ApplyParameters(*feature, pending, theError)) {
        return false;
    }
    if (format->readExtra && !format->readExtra(*feature, extra, theError)) {
        return false;
    }
    if (!ApplyParameters(*feature, pending, theError)) {
        return false;
    }
    if (!pending.empty()) {
        return Fail(theError, "parameters",
                    type + " has no parameter \"" + pending.begin()->first + "\"");
    }

    for (auto it = expressions.begin(); it != expressions.end(); ++it) {
        if (!it.value().isString() || it.value().toString().trimmed().isEmpty()) {
            return Fail(theError, "expressions",
                        "\"" + it.key().toStdString() + "\" must be a non-empty string");
        }
        feature->SetExpression(it.key().toStdString(), it.value().toString().toStdString());
    }
    for (auto it = modelNames.begin(); it != modelNames.end(); ++it) {
        const std::string modelName = it.value().toString().toStdString();
        if (!it.value().isString() || !IsExpressionIdentifier(modelName)
            || IsReservedExpressionName(modelName)) {
            return Fail(theError, "modelNames",
                        "\"" + it.key().toStdString() + "\" must be a parameter name");
        }
        feature->SetModelName(it.key().toStdString(), modelName);
    }

    theFeature = feature;
    return true;
}

// ---- the camera ----

QJsonObject WriteView(const SavedView& theView)
{
    QJsonObject view;
    view["eye"] = QJsonArray{theView.eye[0], theView.eye[1], theView.eye[2]};
    view["centre"] = QJsonArray{theView.centre[0], theView.centre[1], theView.centre[2]};
    view["up"] = QJsonArray{theView.up[0], theView.up[1], theView.up[2]};
    view["scale"] = theView.scale;
    view["projection"] = theView.perspective ? "perspective" : "orthographic";
    view["fieldOfView"] = theView.fieldOfView;
    return view;
}

bool ReadView(const QJsonObject& theObject, SavedView& theView, std::string& theError)
{
    const std::string where = "view";
    std::string projection;
    if (!OnlyKeys(theObject, {"eye", "centre", "up", "scale", "projection", "fieldOfView"}, where,
                  theError)
        || !GetNumber(theObject, "scale", theView.scale, where, theError)
        || !GetString(theObject, "projection", projection, where, theError)
        || !GetNumber(theObject, "fieldOfView", theView.fieldOfView, where, theError)) {
        return false;
    }
    if (!ToNumbers(theObject.value("eye"), 3, theView.eye)
        || !ToNumbers(theObject.value("centre"), 3, theView.centre)
        || !ToNumbers(theObject.value("up"), 3, theView.up)) {
        return Fail(theError, where, "eye, centre and up must each be [x, y, z]");
    }
    if (projection != "perspective" && projection != "orthographic") {
        return Fail(theError, where, "\"projection\" must be perspective or orthographic");
    }
    theView.perspective = projection == "perspective";
    theView.isSet = true;
    return true;
}

std::string FeatureWhere(int theIndex, const QJsonValue& theValue)
{
    std::string where = "features[" + std::to_string(theIndex) + "]";
    const QJsonValue name = theValue.toObject().value("name");
    if (name.isString()) {
        where += " (" + name.toString().toStdString() + ")";
    }
    return where;
}

} // namespace

bool WriteNativeText(const Document&     theDocument,
                     const DesignExtras& theExtras,
                     QByteArray&         theText,
                     std::string&        theError)
{
    ClassicNumbers classic;

    QJsonArray parameters;
    for (const UserParameter& row : theDocument.UserParameters().Parameters()) {
        QJsonObject object;
        object["name"] = Text(row.name);
        object["expression"] = Text(row.expression);
        object["unit"] = Text(UnitSymbolOf(row));
        object["comment"] = Text(row.comment);
        parameters.append(object);
    }

    QJsonArray features;
    for (const FeaturePtr& feature : theDocument.Features()) {
        if (!feature) {
            continue;
        }
        QJsonObject object;
        if (!WriteFeature(*feature, object, theError)) {
            return false;
        }
        features.append(object);
    }

    QJsonObject timeline;
    timeline["rollbackIndex"] =
        theDocument.RollbackIndex() == Document::npos
            ? QJsonValue(QJsonValue::Null)
            : QJsonValue(static_cast<qint64>(theDocument.RollbackIndex()));

    QJsonArray bodyList;
    for (const BodyPtr& body : theDocument.Bodies()) {
        QJsonObject object;
        object["name"] = Text(body->Name());
        object["visible"] = body->IsVisible();
        bodyList.append(object);
    }
    QJsonObject bodies;
    bodies["list"] = bodyList;
    bodies["next"] = theDocument.NextBodyIndex();

    QJsonObject root;
    root["format"] = kFormatName;
    root["version"] = kNativeFormatVersion;
    root["units"] = Text(SymbolOf(theExtras.units));
    root["parameters"] = parameters;
    root["features"] = features;
    root["timeline"] = timeline;
    root["bodies"] = bodies;
    if (theExtras.view.isSet) {
        root["view"] = WriteView(theExtras.view);
    }
    theText = QJsonDocument(root).toJson(QJsonDocument::Indented);
    return true;
}

bool ReadNativeText(const QByteArray&      theText,
                    Document::DesignState& theDesign,
                    DesignExtras&          theExtras,
                    std::string&           theError)
{
    ClassicNumbers classic;

    QJsonParseError parseError;
    const QJsonDocument json = QJsonDocument::fromJson(theText, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        theError = "this is not a PenguinCAD design: the text is not valid JSON ("
                   + parseError.errorString().toStdString() + " at byte "
                   + std::to_string(parseError.offset) + ")";
        return false;
    }
    if (!json.isObject() || json.object().value("format").toString() != QLatin1String(kFormatName)) {
        theError = "this is not a PenguinCAD design: it does not say \"format\": \"penguincad\"";
        return false;
    }
    const QJsonObject root = json.object();

    // The version before anything else, so a file from a newer PenguinCAD
    // says so rather than tripping over the first key it added.
    int version = 0;
    if (!ToInt(root.value("version"), version) || version < 1) {
        theError = "this PenguinCAD design has no valid \"version\"";
        return false;
    }
    if (version > kNativeFormatVersion) {
        theError = "this design was saved by a newer version of PenguinCAD (file format version "
                   + std::to_string(version) + "); this version reads up to version "
                   + std::to_string(kNativeFormatVersion) + ". Update PenguinCAD to open it.";
        return false;
    }

    Document::DesignState design;
    DesignExtras extras;
    std::string units;
    QJsonArray parameters;
    QJsonArray features;
    QJsonObject timeline;
    QJsonObject bodies;
    if (!OnlyKeys(root,
                  {"format", "version", "units", "parameters", "features", "timeline", "bodies",
                   "view"},
                  std::string(), theError)
        || !GetString(root, "units", units, std::string(), theError)
        || !GetArray(root, "parameters", parameters, std::string(), theError)
        || !GetArray(root, "features", features, std::string(), theError)
        || !GetObject(root, "timeline", timeline, std::string(), theError)
        || !GetObject(root, "bodies", bodies, std::string(), theError)) {
        return false;
    }
    if (!LengthUnitFromSymbol(units, extras.units)) {
        return Fail(theError, "units", "\"" + units + "\" is not a length unit");
    }

    for (int i = 0; i < parameters.size(); ++i) {
        const std::string where = "parameters[" + std::to_string(i) + "]";
        if (!parameters.at(i).isObject()) {
            return Fail(theError, where, "must be an object");
        }
        const QJsonObject object = parameters.at(i).toObject();
        UserParameter row;
        std::string unit;
        if (!OnlyKeys(object, {"name", "expression", "unit", "comment"}, where, theError)
            || !GetString(object, "name", row.name, where, theError)
            || !GetString(object, "expression", row.expression, where, theError)
            || !GetString(object, "unit", unit, where, theError)
            || !GetString(object, "comment", row.comment, where, theError)) {
            return false;
        }
        if (!UnitFromSymbol(unit, row)) {
            return Fail(theError, where, "\"" + unit + "\" is not a unit");
        }
        design.parameters.push_back(row);
    }

    for (int i = 0; i < features.size(); ++i) {
        const QJsonValue value = features.at(i);
        std::string error;
        FeaturePtr feature;
        if (!value.isObject()) {
            error = "must be an object";
        } else if (ReadFeature(value.toObject(), feature, error)) {
            design.features.push_back(feature);
            continue;
        }
        return Fail(theError, FeatureWhere(i, value), error);
    }

    if (!OnlyKeys(timeline, {"rollbackIndex"}, "timeline", theError)) {
        return false;
    }
    const QJsonValue rollback = timeline.value("rollbackIndex");
    int rollbackIndex = 0;
    if (rollback.isNull()) {
        design.rollbackIndex = Document::npos;
    } else if (ToInt(rollback, rollbackIndex) && rollbackIndex >= 0) {
        design.rollbackIndex = static_cast<std::size_t>(rollbackIndex);
    } else {
        return Fail(theError, "timeline", "\"rollbackIndex\" must be null or a feature position");
    }

    QJsonArray bodyList;
    if (!OnlyKeys(bodies, {"list", "next"}, "bodies", theError)
        || !GetArray(bodies, "list", bodyList, "bodies", theError)
        || !GetInt(bodies, "next", design.nextBodyIndex, "bodies", theError)) {
        return false;
    }
    for (const QJsonValue& value : bodyList) {
        BodyState body;
        if (!value.isObject()) {
            return Fail(theError, "bodies.list", "a body must be an object");
        }
        const QJsonObject object = value.toObject();
        if (!OnlyKeys(object, {"name", "visible"}, "bodies.list", theError)
            || !GetString(object, "name", body.name, "bodies.list", theError)
            || !GetBool(object, "visible", body.visible, "bodies.list", theError)) {
            return false;
        }
        if (body.name.empty()) {
            return Fail(theError, "bodies.list", "a body needs a name");
        }
        design.bodies.push_back(body);
    }

    if (root.contains("view")) {
        QJsonObject view;
        if (!GetObject(root, "view", view, std::string(), theError)
            || !ReadView(view, extras.view, theError)) {
            return false;
        }
    }

    theDesign = std::move(design);
    theExtras = extras;
    return true;
}

bool SaveNativeFile(const QString&      thePath,
                    const Document&     theDocument,
                    const DesignExtras& theExtras,
                    std::string&        theError)
{
    QByteArray text;
    if (!WriteNativeText(theDocument, theExtras, text, theError)) {
        return false;
    }
    // QSaveFile writes beside the target and renames over it on commit, so
    // until the new file is whole the old one is untouched -- a crash, a
    // full disk or a killed process mid-save leaves the last good save.
    QSaveFile file(thePath);
    if (!file.open(QIODevice::WriteOnly)) {
        theError = file.errorString().toStdString();
        return false;
    }
    if (file.write(text) != text.size()) {
        theError = file.errorString().toStdString();
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        theError = file.errorString().toStdString();
        return false;
    }
    return true;
}

bool OpenNativeFile(const QString& thePath,
                    Document&      theDocument,
                    DesignExtras&  theExtras,
                    std::string&   theError)
{
    QFile file(thePath);
    if (!file.open(QIODevice::ReadOnly)) {
        theError = file.errorString().toStdString();
        return false;
    }
    const QByteArray text = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        theError = file.errorString().toStdString();
        return false;
    }

    Document::DesignState design;
    DesignExtras extras;
    if (!ReadNativeText(text, design, extras, theError)) {
        return false;
    }
    if (!theDocument.ReplaceDesign(std::move(design), theError)) {
        return false;
    }
    theExtras = extras;
    return true;
}

std::vector<std::string> NativeFeatureTypes()
{
    std::vector<std::string> types;
    for (const FeatureFormat& format : Formats()) {
        types.emplace_back(format.type);
    }
    return types;
}

} // namespace lcad
