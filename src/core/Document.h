#pragma once

#include "core/Feature.h"

#include <TopoDS_Shape.hxx>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace lcad {

class Document;

// Implement this to react to timeline changes (browser tree, timeline
// widget, properties panel, viewport redisplay...).
class DocumentObserver
{
public:
    virtual ~DocumentObserver() = default;

    // Any structural change: feature added/removed/renamed/reordered, or
    // a rebuild finished.
    virtual void OnDocumentChanged(Document& theDocument) = 0;

    // Selection of the "active" timeline item changed.
    virtual void OnActiveFeatureChanged(Document& theDocument) { (void)theDocument; }
};

// The parametric document: an ordered timeline of features plus the shape
// they evaluate to. This is the single source of truth for the model --
// UI panels read it, commands mutate it, the viewport displays its result.
class Document
{
public:
    Document();
    ~Document();

    // ---- timeline ----

    // Append to the end of the timeline and rebuild.
    void AddFeature(const FeaturePtr& theFeature);

    // Insert at a specific index (clamped) and rebuild.
    void InsertFeature(std::size_t theIndex, const FeaturePtr& theFeature);

    void RemoveFeature(const FeaturePtr& theFeature);
    void RemoveFeatureAt(std::size_t theIndex);
    void MoveFeature(std::size_t theFrom, std::size_t theTo);

    const std::vector<FeaturePtr>& Features() const { return myFeatures; }
    std::size_t FeatureCount() const { return myFeatures.size(); }
    FeaturePtr FeatureAt(std::size_t theIndex) const;

    // Index of a feature, or npos.
    std::size_t IndexOf(const Feature* theFeature) const;
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

    // ---- rollback (timeline marker, like Fusion's) ----

    // Features at index >= marker are skipped during rebuild. npos (the
    // default) means "evaluate everything".
    void SetRollbackIndex(std::size_t theIndex);
    std::size_t RollbackIndex() const { return myRollbackIndex; }

    // ---- evaluation ----

    // Re-evaluate the whole timeline. Safe to call often; it's what every
    // mutating operation ends with.
    void Rebuild();

    // Final shape after the last evaluated feature. May be null.
    const TopoDS_Shape& Shape() const { return myShape; }

    // Errors collected during the last rebuild, one per failed feature.
    const std::vector<std::string>& Errors() const { return myErrors; }

    // ---- active feature (what the properties panel edits) ----

    void SetActiveFeature(const FeaturePtr& theFeature);
    FeaturePtr ActiveFeature() const { return myActiveFeature; }

    // ---- undo / redo ----

    // Snapshot the current timeline before a mutation. Commands that
    // change the model should call this first; AddFeature and friends do
    // it automatically.
    void PushUndoSnapshot();
    bool CanUndo() const { return !myUndoStack.empty(); }
    bool CanRedo() const { return !myRedoStack.empty(); }
    void Undo();
    void Redo();

    // ---- document state ----

    const std::string& Name() const { return myName; }
    void SetName(std::string theName);

    bool IsModified() const { return myIsModified; }
    void SetModified(bool theValue) { myIsModified = theValue; }

    void Clear();

    // Generate a unique feature name like "Extrude1", "Extrude2".
    std::string MakeUniqueName(const std::string& theBaseName) const;

    // ---- observers ----

    void AddObserver(DocumentObserver* theObserver);
    void RemoveObserver(DocumentObserver* theObserver);

    // Notify observers without changing anything (e.g. after editing a
    // feature's parameters in place).
    void NotifyChanged();

private:
    std::vector<FeaturePtr> CloneTimeline() const;
    void RestoreTimeline(std::vector<FeaturePtr> theTimeline);

    std::vector<FeaturePtr>        myFeatures;
    std::vector<std::string>       myErrors;
    TopoDS_Shape                   myShape;
    FeaturePtr                     myActiveFeature;
    std::size_t                    myRollbackIndex = npos;
    std::string                    myName = "Untitled";
    bool                           myIsModified = false;

    std::vector<std::vector<FeaturePtr>> myUndoStack;
    std::vector<std::vector<FeaturePtr>> myRedoStack;

    std::vector<DocumentObserver*> myObservers;
    bool myIsNotifying = false;
};

} // namespace lcad
