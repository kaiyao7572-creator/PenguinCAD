#pragma once

#include "core/Units.h"

#include <TopoDS_Shape.hxx>

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace lcad {

// A single editable value on a feature (extrude distance, fillet radius,
// sketch plane offset...). This is deliberately a tiny reflection system:
// it lets the properties panel edit any feature without knowing its type.
struct Parameter
{
    enum class Type { Double, Int, Bool, String };

    std::string name;
    Type type = Type::Double;

    double      doubleValue = 0.0;
    int         intValue    = 0;
    bool        boolValue   = false;
    std::string stringValue;

    // Optional UI hints; ignored when min == max.
    double minimum = 0.0;
    double maximum = 0.0;
    std::string unit;

    // A Double may be DRIVEN by an expression over the document's user
    // parameters ("plate_t * 2") rather than hold a plain number. Empty
    // means a plain number. doubleValue is always what it last evaluated
    // to, so code that only reads numbers never needs to know.
    //
    // Features never fill this in: Feature::EditableParameters() does,
    // from the expressions the base class stores. A feature implements
    // SetParameter for numbers and gets expressions for free.
    std::string expression;

    // What a Double measures, read off its unit: "deg" is an angle, no
    // unit is a count or ratio, anything else a length in millimetres.
    UnitKind Kind() const
    {
        return unit == "deg" ? UnitKind::Angle : unit.empty() ? UnitKind::Unitless : UnitKind::Length;
    }

    static Parameter MakeDouble(std::string theName, double theValue, std::string theUnit = "mm")
    {
        Parameter p;
        p.name = std::move(theName);
        p.type = Type::Double;
        p.doubleValue = theValue;
        p.unit = std::move(theUnit);
        return p;
    }

    static Parameter MakeInt(std::string theName, int theValue)
    {
        Parameter p;
        p.name = std::move(theName);
        p.type = Type::Int;
        p.intValue = theValue;
        return p;
    }

    static Parameter MakeBool(std::string theName, bool theValue)
    {
        Parameter p;
        p.name = std::move(theName);
        p.type = Type::Bool;
        p.boolValue = theValue;
        return p;
    }

    static Parameter MakeString(std::string theName, std::string theValue)
    {
        Parameter p;
        p.name = std::move(theName);
        p.type = Type::String;
        p.stringValue = std::move(theValue);
        return p;
    }
};

class Document;
class Feature;

// Passed into Feature::Compute so a feature can look up the ones it
// depends on. References are by NAME, never by pointer: undo/redo clones
// the whole timeline, so raw pointers to other features go stale.
struct ComputeContext
{
    Document* document = nullptr;

    // Find an earlier feature by name, or nullptr. Only features before
    // the caller in the timeline are visible, which prevents cycles.
    Feature* FindFeature(const std::string& theName) const;

    // Index of the feature currently being computed.
    std::size_t currentIndex = 0;
};

// One entry in the timeline. Features are evaluated in order, each one
// receiving the shape produced by everything before it and returning the
// new shape. That linear-history model is what Fusion's timeline is, and
// it keeps rebuild logic simple.
//
// A feature that produces no solid (a sketch, a construction plane) just
// passes the input shape straight through.
class Feature
{
public:
    virtual ~Feature() = default;

    // Stable type identifier, e.g. "Extrude". Used for icons and
    // serialization.
    virtual std::string TypeName() const = 0;

    // Evaluate this feature. Return false and fill theError on failure;
    // the document keeps going with the upstream shape so one broken
    // feature doesn't wipe out the whole model.
    //
    // Must never throw: catch Standard_Failure from OCCT and report it
    // through theError instead.
    virtual bool Compute(const ComputeContext& theContext,
                         const TopoDS_Shape&   theInput,
                         TopoDS_Shape&         theOutput,
                         std::string&          theError) = 0;

    // Deep copy, used for undo snapshots.
    virtual std::unique_ptr<Feature> Clone() const = 0;

    // Editable values surfaced in the properties panel. Default: none.
    virtual std::vector<Parameter> Parameters() const { return {}; }

    // Apply an edited value. Return true if it was accepted (the document
    // then triggers a rebuild). Default: reject everything.
    virtual bool SetParameter(const Parameter& theParameter)
    {
        (void)theParameter;
        return false;
    }

    // Parameters() with each driven Double's expression filled in -- what
    // an editor should show.
    std::vector<Parameter> EditableParameters() const
    {
        std::vector<Parameter> parameters = Parameters();
        for (Parameter& parameter : parameters) {
            if (parameter.type == Parameter::Type::Double) {
                parameter.expression = ExpressionOf(parameter.name);
            }
        }
        return parameters;
    }

    // ---- expressions ----
    //
    // Which numeric parameters are driven by an expression, keyed by
    // parameter name. They live HERE rather than in each feature so every
    // numeric parameter of every feature can be driven without the feature
    // knowing: Document::Rebuild evaluates them and pushes the numbers
    // through SetParameter before the feature computes. Go through
    // Document::SetFeatureParameter to change one -- it checks the
    // expression evaluates before storing it and makes the edit undoable.
    const std::map<std::string, std::string>& Expressions() const { return myExpressions; }

    std::string ExpressionOf(const std::string& theParameterName) const
    {
        const auto found = myExpressions.find(theParameterName);
        return found != myExpressions.end() ? found->second : std::string();
    }

    // Empty clears it, leaving the parameter a plain number again.
    void SetExpression(const std::string& theParameterName, std::string theExpression)
    {
        if (theExpression.empty()) {
            myExpressions.erase(theParameterName);
        } else {
            myExpressions[theParameterName] = std::move(theExpression);
        }
    }

    const std::string& Name() const { return myName; }
    void SetName(std::string theName) { myName = std::move(theName); }

    bool IsSuppressed() const { return myIsSuppressed; }
    void SetSuppressed(bool theValue) { myIsSuppressed = theValue; }

    // Populated by Document::Rebuild().
    const std::string& LastError() const { return myLastError; }
    void SetLastError(std::string theError) { myLastError = std::move(theError); }

    // Shape as of this point in the timeline; set during rebuild so the
    // timeline UI can roll back to any step.
    const TopoDS_Shape& ResultShape() const { return myResultShape; }
    void SetResultShape(const TopoDS_Shape& theShape) { myResultShape = theShape; }

protected:
    // Helper for Clone() implementations to carry the base fields over.
    void CopyBaseTo(Feature& theOther) const
    {
        theOther.myName = myName;
        theOther.myIsSuppressed = myIsSuppressed;
        theOther.myExpressions = myExpressions;
    }

private:
    std::string  myName;
    bool         myIsSuppressed = false;
    std::map<std::string, std::string> myExpressions;
    std::string  myLastError;
    TopoDS_Shape myResultShape;
};

using FeaturePtr = std::shared_ptr<Feature>;

} // namespace lcad
