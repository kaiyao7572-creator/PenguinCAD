#include "core/Document.h"

#include <Standard_Failure.hxx>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>

namespace lcad {

Feature* ComputeContext::FindFeature(const std::string& theName) const
{
    if (document == nullptr) {
        return nullptr;
    }
    const std::vector<FeaturePtr>& features = document->Features();
    // Only look upstream of the feature being computed, so a feature can
    // never depend on something that hasn't been evaluated yet.
    const std::size_t limit = std::min(currentIndex, features.size());
    for (std::size_t i = 0; i < limit; ++i) {
        if (features[i] && features[i]->Name() == theName) {
            return features[i].get();
        }
    }
    return nullptr;
}

namespace {
constexpr std::size_t kMaxUndoDepth = 64;

// Whether an expression's value differs from what the feature already
// holds. Only a real change is pushed through SetParameter: a sketch
// re-solves whenever a dimension is set, and re-solving on every rebuild
// because 30 degrees came back from radians as 29.999999999999996 would
// make every sketch drift a little on each one.
bool SameValue(double theA, double theB)
{
    const double scale = std::max(1.0, std::max(std::fabs(theA), std::fabs(theB)));
    return std::fabs(theA - theB) <= 1.0e-9 * scale;
}

std::string Trimmed(const std::string& theText)
{
    const std::size_t first = theText.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return std::string();
    }
    const std::size_t last = theText.find_last_not_of(" \t\r\n");
    return theText.substr(first, last - first + 1);
}

// "d12" -- the shape of a model parameter name, and of a sketch's labels.
bool IsModelStyleName(const std::string& theName)
{
    if (theName.size() < 2 || theName[0] != 'd') {
        return false;
    }
    return std::all_of(theName.begin() + 1, theName.end(),
                       [](char theChar) { return theChar >= '0' && theChar <= '9'; });
}

bool SameNameNoCase(const std::string& theA, const std::string& theB)
{
    return theA.size() == theB.size()
           && std::equal(theA.begin(), theA.end(), theB.begin(), [](char theX, char theY) {
                  return std::tolower(static_cast<unsigned char>(theX))
                         == std::tolower(static_cast<unsigned char>(theY));
              });
}

// A parameter's exact value as expression text. FormatValue rounds to
// four decimals, and a plain model parameter read by someone else's
// expression must give its real value, not its displayed one. Fixed
// notation, because the evaluator refuses "1e-05".
std::string ExactLiteral(double theValue, UnitKind theKind)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.15f", theValue);
    std::string text = buffer;
    if (text.find('.') != std::string::npos) {
        text.erase(text.find_last_not_of('0') + 1);
        if (!text.empty() && text.back() == '.') {
            text.pop_back();
        }
    }
    if (theKind == UnitKind::Length) {
        text += " mm";
    } else if (theKind == UnitKind::Angle) {
        text += " deg";
    }
    return text;
}

// Put theValue into a numeric parameter. An Int takes only a whole number:
// rounding 3.5 copies to 4 would build something nobody asked for.
bool SetNumber(Parameter& theParameter, double theValue, std::string& theError)
{
    if (theParameter.type != Parameter::Type::Int) {
        theParameter.doubleValue = theValue;
        return true;
    }
    const double whole = std::round(theValue);
    if (std::fabs(theValue - whole) > 1.0e-9 * std::max(1.0, std::fabs(theValue))
        || std::fabs(whole) > 1.0e9) {
        theError = theParameter.name + " needs a whole number, not "
                   + FormatValue(theValue, UnitKind::Unitless);
        return false;
    }
    theParameter.intValue = static_cast<int>(whole);
    return true;
}

// "must be between 0.001 mm and 1000000 mm", in the parameter's own terms.
bool InRange(const Parameter& theParameter, double theValue, std::string& theError)
{
    if (theParameter.minimum >= theParameter.maximum) {
        return true;
    }
    if (theValue >= theParameter.minimum && theValue <= theParameter.maximum) {
        return true;
    }
    const UnitKind kind = theParameter.Kind();
    theError = theParameter.name + " must be between " + FormatValue(theParameter.minimum, kind)
               + " and " + FormatValue(theParameter.maximum, kind) + ", not "
               + FormatValue(theValue, kind);
    return false;
}
} // namespace

Document::Document() = default;
Document::~Document() = default;

FeaturePtr Document::FeatureAt(std::size_t theIndex) const
{
    if (theIndex >= myFeatures.size()) {
        return nullptr;
    }
    return myFeatures[theIndex];
}

std::size_t Document::IndexOf(const Feature* theFeature) const
{
    for (std::size_t i = 0; i < myFeatures.size(); ++i) {
        if (myFeatures[i].get() == theFeature) {
            return i;
        }
    }
    return npos;
}

void Document::AddFeature(const FeaturePtr& theFeature)
{
    if (!theFeature) {
        return;
    }
    PushUndoSnapshot();
    if (theFeature->Name().empty()) {
        theFeature->SetName(MakeUniqueName(theFeature->TypeName()));
    }
    myFeatures.push_back(theFeature);
    myIsModified = true;
    Rebuild();
}

void Document::InsertFeature(std::size_t theIndex, const FeaturePtr& theFeature)
{
    if (!theFeature) {
        return;
    }
    PushUndoSnapshot();
    if (theFeature->Name().empty()) {
        theFeature->SetName(MakeUniqueName(theFeature->TypeName()));
    }
    theIndex = std::min(theIndex, myFeatures.size());
    myFeatures.insert(myFeatures.begin() + static_cast<std::ptrdiff_t>(theIndex), theFeature);
    myIsModified = true;
    Rebuild();
}

void Document::RemoveFeature(const FeaturePtr& theFeature)
{
    const std::size_t index = IndexOf(theFeature.get());
    if (index != npos) {
        RemoveFeatureAt(index);
    }
}

void Document::RemoveFeatureAt(std::size_t theIndex)
{
    if (theIndex >= myFeatures.size()) {
        return;
    }
    PushUndoSnapshot();
    if (myActiveFeature == myFeatures[theIndex]) {
        myActiveFeature.reset();
    }
    myFeatures.erase(myFeatures.begin() + static_cast<std::ptrdiff_t>(theIndex));
    myIsModified = true;
    Rebuild();
}

void Document::MoveFeature(std::size_t theFrom, std::size_t theTo)
{
    if (theFrom >= myFeatures.size() || theTo >= myFeatures.size() || theFrom == theTo) {
        return;
    }
    PushUndoSnapshot();
    FeaturePtr moved = myFeatures[theFrom];
    myFeatures.erase(myFeatures.begin() + static_cast<std::ptrdiff_t>(theFrom));
    myFeatures.insert(myFeatures.begin() + static_cast<std::ptrdiff_t>(theTo), moved);
    myIsModified = true;
    Rebuild();
}

void Document::SetRollbackIndex(std::size_t theIndex)
{
    myRollbackIndex = theIndex;
    Rebuild();
}

void Document::Rebuild()
{
    Evaluate();
    NotifyChanged();
}

void Document::Evaluate()
{
    myErrors.clear();

    // Every parameter, user and model alike, is resolved BEFORE the
    // timeline runs -- Fusion's order -- so a feature reads values that
    // already account for everything they depend on, wherever it sits.
    AssignModelNames();
    ResolveParameters();

    TopoDS_Shape current;
    const std::size_t limit = std::min(myRollbackIndex, myFeatures.size());

    for (std::size_t i = 0; i < myFeatures.size(); ++i) {
        const FeaturePtr& feature = myFeatures[i];
        if (!feature) {
            continue;
        }

        feature->SetLastError(std::string());

        if (i >= limit || feature->IsSuppressed()) {
            // Rolled back or suppressed: carry the upstream shape forward
            // unchanged so downstream indices stay meaningful.
            feature->SetResultShape(current);
            continue;
        }

        ComputeContext computeContext;
        computeContext.document = this;
        computeContext.currentIndex = i;

        TopoDS_Shape output;
        std::string error;
        bool ok = false;
        try {
            // Expressions first, so the feature computes with the numbers
            // its parameters evaluate to NOW. One that no longer evaluates
            // fails the feature like any other compute error, rather than
            // building on the last number it gave.
            ok = ApplyExpressions(*feature, error)
                 && feature->Compute(computeContext, current, output, error);
        } catch (const Standard_Failure& failure) {
            // A feature that lets an OCCT exception escape shouldn't take
            // the whole rebuild down with it.
            ok = false;
            const Standard_CString message = failure.GetMessageString();
            error = message != nullptr ? message : "OCCT exception";
        } catch (const std::exception& exception) {
            ok = false;
            error = exception.what();
        }

        if (!ok) {
            if (error.empty()) {
                error = feature->Name() + ": failed to compute";
            } else {
                error = feature->Name() + ": " + error;
            }
            feature->SetLastError(error);
            myErrors.push_back(error);
            // Keep the upstream shape so one bad feature doesn't erase
            // the model -- same forgiving behavior Fusion has.
            feature->SetResultShape(current);
            continue;
        }

        current = output;
        feature->SetResultShape(current);
    }

    myShape = current;
    // Before the observers run, not after: the browser and the viewport
    // both read Bodies() the moment they are told the document changed.
    myBodies.Update(myShape);
}

void Document::SetActiveFeature(const FeaturePtr& theFeature)
{
    if (myActiveFeature == theFeature) {
        return;
    }
    myActiveFeature = theFeature;
    for (DocumentObserver* observer : myObservers) {
        observer->OnActiveFeatureChanged(*this);
    }
}

bool Document::ApplyExpressions(Feature& theFeature, std::string& theError) const
{
    if (theFeature.Expressions().empty()) {
        return true;
    }
    const std::vector<Parameter> parameters = theFeature.Parameters();
    for (const auto& entry : theFeature.Expressions()) {
        const std::string& name = entry.first;
        const std::string& expression = entry.second;

        const Parameter* parameter = nullptr;
        for (const Parameter& candidate : parameters) {
            if (candidate.name == name && candidate.IsNumber()) {
                parameter = &candidate;
                break;
            }
        }
        if (parameter == nullptr) {
            // The parameter itself has gone -- a sketch dimension that was
            // deleted. Nothing is left for the expression to drive.
            continue;
        }

        // Resolved with everything else before the timeline ran; its row is
        // under the parameter's model name.
        ExpressionResult result;
        const UserParameter* row = myResolved.Find(theFeature.ModelNameOf(name));
        if (row != nullptr) {
            result.ok = row->isValid;
            result.value = row->value;
            result.error = row->error;
        } else {
            result = myResolved.EvaluateValue(expression, parameter->Kind());
        }
        if (!result.ok) {
            theError = name + " = " + expression + ": " + result.error;
            return false;
        }
        std::string rangeError;
        if (!InRange(*parameter, result.value, rangeError)) {
            theError = name + " = " + expression + ": " + rangeError;
            return false;
        }
        if (SameValue(result.value, parameter->Number())) {
            continue;
        }
        Parameter edited = *parameter;
        std::string wholeError;
        if (!SetNumber(edited, result.value, wholeError)) {
            theError = name + " = " + expression + ": " + wholeError;
            return false;
        }
        if (!theFeature.SetParameter(edited)) {
            theError = name + " = " + expression + " gives "
                       + FormatValue(result.value, parameter->Kind()) + ", which "
                       + theFeature.TypeName() + " does not accept";
            return false;
        }
    }
    return true;
}

bool Document::IsParameterNameTaken(const std::string& theName, std::string& theWho,
                                    const Feature*     theIgnoredFeature,
                                    const std::string& theIgnoredParameter) const
{
    for (const UserParameter& row : myParameters.Parameters()) {
        if (SameNameNoCase(row.name, theName)) {
            theWho = "the user parameter " + row.name;
            return true;
        }
    }
    for (const FeaturePtr& feature : myFeatures) {
        if (!feature) {
            continue;
        }
        for (const auto& entry : feature->ModelNames()) {
            if (feature.get() == theIgnoredFeature && entry.first == theIgnoredParameter) {
                continue;
            }
            if (SameNameNoCase(entry.second, theName)) {
                theWho = feature->Name() + "'s " + entry.first;
                return true;
            }
        }
    }
    return false;
}

void Document::AssignModelNames()
{
    // Numbering goes on from the highest d# in use and never rewinds, so
    // a deleted dimension's name is not handed to a different one.
    int next = 1;
    const auto consider = [&next](const std::string& theName) {
        if (IsModelStyleName(theName)) {
            next = std::max(next, std::atoi(theName.c_str() + 1) + 1);
        }
    };
    for (const UserParameter& row : myParameters.Parameters()) {
        consider(row.name);
    }
    for (const FeaturePtr& feature : myFeatures) {
        if (feature) {
            for (const auto& entry : feature->ModelNames()) {
                consider(entry.second);
            }
        }
    }

    std::string who;
    // First pass: a parameter already CALLED d-something (a sketch's
    // dimension labels) claims its own name while nobody else has it --
    // before any other parameter is handed a fresh one, or a sketch's plane
    // offset, listed first, would take d1 from the dimension labelled d1.
    for (const FeaturePtr& feature : myFeatures) {
        if (!feature) {
            continue;
        }
        for (const Parameter& parameter : feature->Parameters()) {
            if (parameter.IsNumber() && feature->ModelNameOf(parameter.name).empty()
                && IsModelStyleName(parameter.name) && !IsParameterNameTaken(parameter.name, who)) {
                feature->SetModelName(parameter.name, parameter.name);
                next = std::max(next, std::atoi(parameter.name.c_str() + 1) + 1);
            }
        }
    }
    // Second pass: everything still unnamed gets the next free d#.
    for (const FeaturePtr& feature : myFeatures) {
        if (!feature) {
            continue;
        }
        for (const Parameter& parameter : feature->Parameters()) {
            if (!parameter.IsNumber() || !feature->ModelNameOf(parameter.name).empty()) {
                continue;
            }
            std::string fresh;
            do {
                fresh = "d" + std::to_string(next++);
            } while (IsParameterNameTaken(fresh, who));

            std::string key = parameter.name;
            if (IsModelStyleName(parameter.name)
                && feature->RenameParameter(parameter.name, fresh)) {
                // Two sketches both started at d1: this one's becomes fresh,
                // label and model name together.
                feature->MoveParameterKeys(parameter.name, fresh);
                key = fresh;
            }
            feature->SetModelName(key, fresh);
        }
    }
}

std::vector<UserParameter> Document::EvaluationRows(const Feature*     theOverrideFeature,
                                                    const std::string& theOverrideParameter,
                                                    const std::string& theOverrideExpression) const
{
    std::vector<UserParameter> rows = myParameters.Parameters();
    for (const FeaturePtr& feature : myFeatures) {
        if (!feature) {
            continue;
        }
        for (const Parameter& parameter : feature->Parameters()) {
            const std::string model = feature->ModelNameOf(parameter.name);
            if (!parameter.IsNumber() || model.empty()) {
                continue;
            }
            UserParameter row;
            row.name = model;
            row.kind = parameter.Kind();
            const bool overridden =
                feature.get() == theOverrideFeature && parameter.name == theOverrideParameter;
            const std::string expression =
                overridden ? theOverrideExpression : feature->ExpressionOf(parameter.name);
            row.expression =
                expression.empty() ? ExactLiteral(parameter.Number(), row.kind) : expression;
            rows.push_back(row);
        }
    }
    return rows;
}

void Document::ResolveParameters()
{
    myResolved.Assign(EvaluationRows());
    myParameters.AdoptResults(myResolved);
}

bool Document::RenameModelParameter(const FeaturePtr& theFeature, const std::string& theParameter,
                                    const std::string& theNewName, std::string& theError)
{
    if (!theFeature || IndexOf(theFeature.get()) == npos) {
        theError = "that feature is not part of this design";
        return false;
    }
    const std::string oldName = theFeature->ModelNameOf(theParameter);
    if (oldName.empty()) {
        theError = theParameter + " has no model parameter name";
        return false;
    }
    if (theNewName == oldName) {
        return true;
    }
    if (!IsExpressionIdentifier(theNewName)) {
        theError = "\"" + theNewName
                   + "\" cannot be a parameter name: start with a letter or _, then letters,"
                     " digits or _";
        return false;
    }
    if (IsReservedExpressionName(theNewName)) {
        theError = "\"" + theNewName + "\" is already part of the expression language";
        return false;
    }
    std::string who;
    if (IsParameterNameTaken(theNewName, who, theFeature.get(), theParameter)) {
        theError = "\"" + theNewName + "\" is already " + who;
        return false;
    }

    Snapshot snapshot = TakeSnapshot();
    // Everything that read the old name reads the new one.
    for (const UserParameter& row : myParameters.Parameters()) {
        const std::string renamed = RenameExpressionVariable(row.expression, oldName, theNewName);
        if (renamed != row.expression) {
            std::string ignored;
            myParameters.SetExpression(row.name, renamed, ignored);
        }
    }
    for (const FeaturePtr& feature : myFeatures) {
        if (!feature) {
            continue;
        }
        const std::map<std::string, std::string> expressions = feature->Expressions();
        for (const auto& entry : expressions) {
            const std::string renamed = RenameExpressionVariable(entry.second, oldName, theNewName);
            if (renamed != entry.second) {
                feature->SetExpression(entry.first, renamed);
            }
        }
    }
    std::string key = theParameter;
    if (theParameter == oldName && theFeature->RenameParameter(oldName, theNewName)) {
        // A sketch dimension's label is its model name; they move together.
        theFeature->MoveParameterKeys(oldName, theNewName);
        key = theNewName;
    }
    theFeature->SetModelName(key, theNewName);
    PushSnapshot(std::move(snapshot));
    myIsModified = true;
    Rebuild();
    return true;
}

std::vector<FeaturePtr> Document::CloneTimeline() const
{
    std::vector<FeaturePtr> copy;
    copy.reserve(myFeatures.size());
    for (const FeaturePtr& feature : myFeatures) {
        if (!feature) {
            continue;
        }
        std::unique_ptr<Feature> cloned = feature->Clone();
        if (cloned) {
            copy.push_back(FeaturePtr(std::move(cloned)));
        }
    }
    return copy;
}

Document::Snapshot Document::TakeSnapshot() const
{
    return Snapshot{CloneTimeline(), myParameters};
}

void Document::RestoreSnapshot(Snapshot theSnapshot)
{
    myFeatures = std::move(theSnapshot.timeline);
    myParameters = std::move(theSnapshot.parameters);
    myActiveFeature.reset();
    myIsModified = true;
    Rebuild();
}

void Document::PushSnapshot(Snapshot theSnapshot)
{
    myUndoStack.push_back(std::move(theSnapshot));
    if (myUndoStack.size() > kMaxUndoDepth) {
        myUndoStack.erase(myUndoStack.begin());
    }
    myRedoStack.clear();
}

void Document::PushUndoSnapshot()
{
    PushSnapshot(TakeSnapshot());
}

void Document::Undo()
{
    if (myUndoStack.empty()) {
        return;
    }
    myRedoStack.push_back(TakeSnapshot());
    Snapshot previous = std::move(myUndoStack.back());
    myUndoStack.pop_back();
    RestoreSnapshot(std::move(previous));
}

void Document::Redo()
{
    if (myRedoStack.empty()) {
        return;
    }
    myUndoStack.push_back(TakeSnapshot());
    Snapshot next = std::move(myRedoStack.back());
    myRedoStack.pop_back();
    RestoreSnapshot(std::move(next));
}

template <typename Edit>
bool Document::EditUserParameters(Edit&& theEdit)
{
    Snapshot snapshot = TakeSnapshot();
    if (!theEdit()) {
        return false;
    }
    PushSnapshot(std::move(snapshot));
    myIsModified = true;
    Rebuild();
    return true;
}

bool Document::AddUserParameter(const UserParameter& theParameter, std::string& theError)
{
    std::string who;
    if (IsParameterNameTaken(theParameter.name, who)) {
        theError = "\"" + theParameter.name + "\" is already " + who;
        return false;
    }
    return EditUserParameters([&]() { return myParameters.Add(theParameter, theError); });
}

bool Document::SetUserParameterExpression(const std::string& theName,
                                          const std::string& theExpression,
                                          std::string&       theError)
{
    return EditUserParameters(
        [&]() { return myParameters.SetExpression(theName, theExpression, theError); });
}

bool Document::SetUserParameterComment(const std::string& theName,
                                       const std::string& theComment,
                                       std::string&       theError)
{
    return EditUserParameters(
        [&]() { return myParameters.SetComment(theName, theComment, theError); });
}

bool Document::SetUserParameterUnit(const std::string& theName, UnitKind theKind,
                                    LengthUnit theLengthUnit, AngleUnit theAngleUnit,
                                    std::string& theError)
{
    return EditUserParameters([&]() {
        return myParameters.SetUnit(theName, theKind, theLengthUnit, theAngleUnit, theError);
    });
}

bool Document::RenameUserParameter(const std::string& theOldName,
                                   const std::string& theNewName,
                                   std::string&       theError)
{
    std::string who;
    if (!SameNameNoCase(theOldName, theNewName) && IsParameterNameTaken(theNewName, who)) {
        theError = "\"" + theNewName + "\" is already " + who;
        return false;
    }
    return EditUserParameters([&]() {
        std::vector<std::string> rewritten;
        if (!myParameters.Rename(theOldName, theNewName, rewritten, theError)) {
            return false;
        }
        for (const FeaturePtr& feature : myFeatures) {
            if (!feature) {
                continue;
            }
            // Copied: SetExpression edits the map being walked.
            const std::map<std::string, std::string> expressions = feature->Expressions();
            for (const auto& entry : expressions) {
                const std::string renamed =
                    RenameExpressionVariable(entry.second, theOldName, theNewName);
                if (renamed != entry.second) {
                    feature->SetExpression(entry.first, renamed);
                }
            }
        }
        return true;
    });
}

bool Document::RemoveUserParameter(const std::string& theName, std::string& theError)
{
    return EditUserParameters([&]() {
        std::vector<std::string> stale;
        return myParameters.Remove(theName, stale, theError);
    });
}

std::vector<std::string> Document::UsersOfUserParameter(const std::string& theName) const
{
    // Everything that depends on it in the resolved table, through any
    // chain of user and model parameters -- then model parameters reported
    // as the features that own them. Only parameters that still exist are
    // rows, so a deleted dimension's leftover expression pins nothing.
    std::vector<std::string> users;
    for (const std::string& dependent : myResolved.Dependents(theName)) {
        if (myParameters.Find(dependent) != nullptr) {
            users.push_back(dependent);
            continue;
        }
        for (const FeaturePtr& feature : myFeatures) {
            if (!feature) {
                continue;
            }
            for (const auto& entry : feature->ModelNames()) {
                if (entry.second == dependent
                    && std::find(users.begin(), users.end(), feature->Name()) == users.end()) {
                    users.push_back(feature->Name());
                }
            }
        }
    }
    return users;
}

bool Document::SetFeatureParameter(const FeaturePtr& theFeature,
                                   const Parameter&  theParameter,
                                   std::string&      theError)
{
    if (!theFeature || IndexOf(theFeature.get()) == npos) {
        theError = "that feature is not part of this design";
        return false;
    }

    Parameter edited = theParameter;
    std::string expression;
    if (edited.IsNumber()) {
        expression = Trimmed(edited.expression);
        if (!expression.empty()) {
            if (IsLiteralOfWrongKind(expression, edited.Kind(), theError)) {
                return false;
            }
            // Tried against the WHOLE design -- user and model parameters,
            // with this one's new expression in place -- so it may read d3,
            // and a loop back to itself through any of them is refused.
            AssignModelNames();
            const std::string model = theFeature->ModelNameOf(edited.name);
            ParameterTable trial;
            trial.Assign(EvaluationRows(theFeature.get(), edited.name, expression));
            const UserParameter* row = model.empty() ? nullptr : trial.Find(model);
            ExpressionResult result = row != nullptr
                                          ? ExpressionResult()
                                          : trial.EvaluateValue(expression, edited.Kind());
            if (row != nullptr) {
                result.ok = row->isValid;
                result.value = row->value;
                result.error = row->error;
            }
            if (!result.ok) {
                theError = result.error;
                return false;
            }
            if (!SetNumber(edited, result.value, theError)) {
                return false;
            }
            if (ExpressionVariables(expression).empty()) {
                expression.clear();
            }
        }
        if (!InRange(edited, edited.Number(), theError)) {
            return false;
        }
    }

    // An edit that changes nothing is not an edit. Every commit pushes an
    // undo step, and an undo step clears Redo -- so pressing Enter in a row
    // nobody touched used to throw the user's redo history away.
    for (const Parameter& current : theFeature->EditableParameters()) {
        if (current.name != edited.name || current.type != edited.type) {
            continue;
        }
        bool same = false;
        switch (edited.type) {
            case Parameter::Type::Double:
                same = SameValue(current.doubleValue, edited.doubleValue)
                       && current.expression == expression;
                break;
            case Parameter::Type::Int:
                same = current.intValue == edited.intValue && current.expression == expression;
                break;
            case Parameter::Type::Bool:
                same = current.boolValue == edited.boolValue;
                break;
            case Parameter::Type::String:
                same = current.stringValue == edited.stringValue;
                break;
        }
        if (same) {
            return true;
        }
        break;
    }

    Snapshot snapshot = TakeSnapshot();
    if (!theFeature->SetParameter(edited)) {
        theError = theFeature->Name() + " does not accept that " + edited.name;
        return false;
    }
    if (edited.IsNumber()) {
        theFeature->SetExpression(edited.name, expression);
    }
    PushSnapshot(std::move(snapshot));
    myIsModified = true;
    Rebuild();
    return true;
}

void Document::SetName(std::string theName)
{
    myName = std::move(theName);
    NotifyChanged();
}

void Document::Clear()
{
    PushUndoSnapshot();
    myFeatures.clear();
    myParameters.Clear();
    myActiveFeature.reset();
    myErrors.clear();
    myRollbackIndex = npos;
    myShape = TopoDS_Shape();
    // Reset the body numbering too: a new document starts at Body1, and
    // Rebuild alone would only empty the table, not rewind the counter.
    myBodies.Clear();
    myIsModified = false;
    Rebuild();
}

bool Document::ReplaceDesign(DesignState theState, std::string& theError)
{
    // The user parameters go through the table's own Add, the path the
    // Change Parameters dialog takes, so a name the dialog would refuse or
    // a loop it would refuse is refused here too.
    ParameterTable parameters;
    for (const UserParameter& row : theState.parameters) {
        std::string error;
        if (!parameters.Add(row, error)) {
            theError = "user parameter \"" + row.name + "\": " + error;
            return false;
        }
    }

    // Every parameter name, user and model, is one namespace an expression
    // reads from. Two rows answering to one name would make "d3 * 2" mean
    // whichever the evaluator met first, so a design claiming that is not
    // one this app could have written.
    std::vector<std::pair<std::string, std::string>> names;   // name, who
    for (const UserParameter& row : parameters.Parameters()) {
        names.emplace_back(row.name, "the user parameter " + row.name);
    }
    for (const FeaturePtr& feature : theState.features) {
        if (!feature) {
            theError = "an empty timeline entry";
            return false;
        }
        for (const auto& entry : feature->ModelNames()) {
            names.emplace_back(entry.second, feature->Name() + "'s " + entry.first);
        }
    }
    for (std::size_t i = 0; i < names.size(); ++i) {
        for (std::size_t j = i + 1; j < names.size(); ++j) {
            if (SameNameNoCase(names[i].first, names[j].first)) {
                theError = "\"" + names[j].first + "\" is both " + names[i].second + " and "
                           + names[j].second;
                return false;
            }
        }
    }

    // Accepted: nothing below can fail. The active feature is dropped
    // through SetActiveFeature so the properties panel lets go of it.
    SetActiveFeature(nullptr);
    myFeatures = std::move(theState.features);
    myParameters = std::move(parameters);
    myRollbackIndex = theState.rollbackIndex;
    myErrors.clear();
    myShape = TopoDS_Shape();
    myBodies.Clear();
    myUndoStack.clear();
    myRedoStack.clear();
    myIsModified = false;

    Evaluate();
    myBodies.AdoptNames(theState.bodies, theState.nextBodyIndex);
    NotifyChanged();
    return true;
}

std::string Document::MakeUniqueName(const std::string& theBaseName) const
{
    for (int suffix = 1; suffix < 10000; ++suffix) {
        const std::string candidate = theBaseName + std::to_string(suffix);
        bool taken = false;
        for (const FeaturePtr& feature : myFeatures) {
            if (feature && feature->Name() == candidate) {
                taken = true;
                break;
            }
        }
        if (!taken) {
            return candidate;
        }
    }
    return theBaseName;
}

void Document::AddObserver(DocumentObserver* theObserver)
{
    if (theObserver == nullptr) {
        return;
    }
    if (std::find(myObservers.begin(), myObservers.end(), theObserver) == myObservers.end()) {
        myObservers.push_back(theObserver);
    }
}

void Document::RemoveObserver(DocumentObserver* theObserver)
{
    myObservers.erase(std::remove(myObservers.begin(), myObservers.end(), theObserver),
                      myObservers.end());
}

void Document::NotifyChanged()
{
    // Guard against an observer triggering another rebuild re-entrantly.
    if (myIsNotifying) {
        return;
    }
    myIsNotifying = true;
    // Copy: an observer may add/remove observers while reacting.
    std::vector<DocumentObserver*> observers = myObservers;
    for (DocumentObserver* observer : observers) {
        observer->OnDocumentChanged(*this);
    }
    myIsNotifying = false;
}

} // namespace lcad
