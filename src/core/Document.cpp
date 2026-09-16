#include "core/Document.h"

#include <Standard_Failure.hxx>

#include <algorithm>
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
}

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
            ok = feature->Compute(computeContext, current, output, error);
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

void Document::RestoreTimeline(std::vector<FeaturePtr> theTimeline)
{
    myFeatures = std::move(theTimeline);
    myActiveFeature.reset();
    myIsModified = true;
    Rebuild();
}

void Document::PushUndoSnapshot()
{
    myUndoStack.push_back(CloneTimeline());
    if (myUndoStack.size() > kMaxUndoDepth) {
        myUndoStack.erase(myUndoStack.begin());
    }
    myRedoStack.clear();
}

void Document::Undo()
{
    if (myUndoStack.empty()) {
        return;
    }
    myRedoStack.push_back(CloneTimeline());
    std::vector<FeaturePtr> previous = std::move(myUndoStack.back());
    myUndoStack.pop_back();
    RestoreTimeline(std::move(previous));
}

void Document::Redo()
{
    if (myRedoStack.empty()) {
        return;
    }
    myUndoStack.push_back(CloneTimeline());
    std::vector<FeaturePtr> next = std::move(myRedoStack.back());
    myRedoStack.pop_back();
    RestoreTimeline(std::move(next));
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
    myActiveFeature.reset();
    myErrors.clear();
    myRollbackIndex = npos;
    myShape = TopoDS_Shape();
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
