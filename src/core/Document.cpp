#include "core/Document.h"

#include <Standard_Failure.hxx>

#include <algorithm>
#include <cmath>
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
    myErrors.clear();

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
    NotifyChanged();
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
            if (candidate.name == name && candidate.type == Parameter::Type::Double) {
                parameter = &candidate;
                break;
            }
        }
        if (parameter == nullptr) {
            // The parameter itself has gone -- a sketch dimension that was
            // deleted. Nothing is left for the expression to drive.
            continue;
        }

        const ExpressionResult result = myParameters.EvaluateValue(expression, parameter->Kind());
        if (!result.ok) {
            theError = name + " = " + expression + ": " + result.error;
            return false;
        }
        std::string rangeError;
        if (!InRange(*parameter, result.value, rangeError)) {
            theError = name + " = " + expression + ": " + rangeError;
            return false;
        }
        if (SameValue(result.value, parameter->doubleValue)) {
            continue;
        }
        Parameter edited = *parameter;
        edited.doubleValue = result.value;
        if (!theFeature.SetParameter(edited)) {
            theError = name + " = " + expression + " gives "
                       + FormatValue(result.value, parameter->Kind()) + ", which "
                       + theFeature.TypeName() + " does not accept";
            return false;
        }
    }
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
    if (edited.type == Parameter::Type::Double) {
        expression = Trimmed(edited.expression);
        if (!expression.empty()) {
            const ExpressionResult result = myParameters.EvaluateValue(expression, edited.Kind());
            if (!result.ok) {
                theError = result.error;
                return false;
            }
            edited.doubleValue = result.value;
            if (IsUnitlessExpression(expression)) {
                expression.clear();
            }
        }
        if (!InRange(edited, edited.doubleValue, theError)) {
            return false;
        }
    }

    Snapshot snapshot = TakeSnapshot();
    if (!theFeature->SetParameter(edited)) {
        theError = theFeature->Name() + " does not accept that " + edited.name;
        return false;
    }
    if (edited.type == Parameter::Type::Double) {
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
