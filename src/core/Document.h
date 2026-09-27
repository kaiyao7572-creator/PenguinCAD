#pragma once

#include "core/Body.h"
#include "core/Feature.h"
#include "core/ParameterTable.h"

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

    // ---- bodies ----

    // The shape above is the right model for a timeline and the wrong one
    // for a user: "the model" is not a thing you can name, hide or act on
    // one of. These are, and they keep their names across a rebuild.
    const std::vector<BodyPtr>& Bodies() const { return myBodies.Bodies(); }
    Body* FindBody(const std::string& theName) const { return myBodies.Find(theName); }

    // ---- user parameters (Fusion's MODIFY > Change Parameters) ----
    //
    // Named values any feature's numeric parameter can be driven by. Every
    // edit here is undoable and rebuilds, so changing one parameter reaches
    // every feature that reads it, directly or through other parameters.
    // Each returns false with theError set, and changes nothing, when the
    // edit is refused: a malformed or taken name, or a cycle. An expression
    // that merely fails to evaluate is accepted and its row says why --
    // the table's own rule, see core/ParameterTable.h.
    const ParameterTable& UserParameters() const { return myParameters; }

    // What an expression is evaluated against: the user parameters PLUS one
    // row per model parameter (d1, d2 ... -- every numeric parameter of
    // every feature), resolved together before the timeline runs, as
    // Fusion does. So a field can take "d3 * 2", a user parameter can read
    // a feature's dimension, and a loop through either is caught. Rebuilt
    // on every Rebuild; the object itself lives as long as the document,
    // so a field may hold a pointer to it.
    const ParameterTable& EvaluationTable() const { return myResolved; }

    // Rename a model parameter (Fusion lets you call d3 "wall"), rewriting
    // every expression that read it. Undoable. Refused for a name a user
    // parameter or another model parameter already has.
    bool RenameModelParameter(const FeaturePtr& theFeature, const std::string& theParameter,
                              const std::string& theNewName, std::string& theError);

    bool AddUserParameter(const UserParameter& theParameter, std::string& theError);
    bool SetUserParameterExpression(const std::string& theName,
                                    const std::string& theExpression,
                                    std::string&       theError);
    bool SetUserParameterComment(const std::string& theName,
                                 const std::string& theComment,
                                 std::string&       theError);
    bool SetUserParameterUnit(const std::string& theName, UnitKind theKind,
                              LengthUnit theLengthUnit, AngleUnit theAngleUnit,
                              std::string& theError);

    // Also rewrites every feature expression that named the old one, so
    // the model keeps working -- what Fusion does.
    bool RenameUserParameter(const std::string& theOldName,
                             const std::string& theNewName,
                             std::string&       theError);

    // Allowed even while features read it: refusing would leave a model
    // that cannot be taken apart. Those features fail on the rebuild and
    // say which name went missing, rather than quietly keeping the last
    // number it gave them.
    bool RemoveUserParameter(const std::string& theName, std::string& theError);

    // Everything that reads theName: the user parameters whose expressions
    // use it, directly or through a chain, then the features with a
    // parameter driven by it, each once and in order. Empty means nothing
    // would break without it -- what a UI asks before offering Delete, the
    // way Fusion refuses to delete a parameter that is in use.
    std::vector<std::string> UsersOfUserParameter(const std::string& theName) const;

    // ---- editing a feature's parameters ----

    // Apply one edited parameter to theFeature, undoably, and rebuild.
    //
    // A Double whose `expression` is set is DRIVEN by it from now on. The
    // expression has to evaluate against the user parameters now, or the
    // edit is refused and nothing changes: a field that took
    // "plate_tt * 2" and then failed on every rebuild would be worse than
    // one that went red. A Double with no expression becomes a plain
    // number again.
    //
    // An expression that names no parameter -- "1/2", "10*3", "1 in + 2 mm"
    // -- has nothing that can change, so it is stored as the number it
    // gives and does not stay an expression. A bare "1/2" is read in the
    // parameter's internal unit; a field showing another unit converts
    // before it gets here.
    bool SetFeatureParameter(const FeaturePtr& theFeature,
                             const Parameter&  theParameter,
                             std::string&      theError);

    // ---- active feature (what the properties panel edits) ----

    void SetActiveFeature(const FeaturePtr& theFeature);
    FeaturePtr ActiveFeature() const { return myActiveFeature; }

    // ---- undo / redo ----

    // Snapshot the current timeline and user parameters before a mutation.
    // Commands that change the model should call this first; AddFeature
    // and friends do it automatically.
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
    // Everything undo puts back. The parameters travel with the timeline:
    // undoing a parameter edit must undo its effect on the model too, and
    // a timeline restored without the table it was built against could
    // name parameters that no longer exist.
    struct Snapshot
    {
        std::vector<FeaturePtr> timeline;
        ParameterTable          parameters;
    };

    std::vector<FeaturePtr> CloneTimeline() const;
    Snapshot TakeSnapshot() const;
    void PushSnapshot(Snapshot theSnapshot);
    void RestoreSnapshot(Snapshot theSnapshot);

    // Snapshot, try theEdit on the table, and keep the snapshot and
    // rebuild only if it was accepted -- a refused edit must not leave an
    // undo step that does nothing.
    template <typename Edit>
    bool EditUserParameters(Edit&& theEdit);

    // Push the current value of every expression-driven parameter into
    // theFeature. False, with theError saying which parameter and why,
    // when one does not evaluate or the feature refuses what it gives.
    bool ApplyExpressions(Feature& theFeature, std::string& theError) const;

    // Give every numeric parameter without a model name the next free d#.
    // A sketch dimension keeps its own label unless another feature has it,
    // in which case the sketch renames it -- the label and the model name
    // must never disagree.
    void AssignModelNames();

    // The evaluation table's rows: the user parameters, then one row per
    // model parameter -- its expression if driven, else its exact value.
    // theOverride... substitutes one parameter's expression, which is how
    // an edit is tried against the whole design before it is accepted.
    std::vector<UserParameter> EvaluationRows(const Feature*      theOverrideFeature = nullptr,
                                              const std::string&  theOverrideParameter = {},
                                              const std::string&  theOverrideExpression = {}) const;

    // Rebuild myResolved and hand each user row its result.
    void ResolveParameters();

    // A user or model parameter already called theName (without case), and
    // a phrase saying which, for refusing a clash.
    bool IsParameterNameTaken(const std::string& theName, std::string& theWho,
                              const Feature* theIgnoredFeature = nullptr,
                              const std::string& theIgnoredParameter = {}) const;

    std::vector<FeaturePtr>        myFeatures;
    ParameterTable                 myParameters;
    ParameterTable                 myResolved;
    std::vector<std::string>       myErrors;
    TopoDS_Shape                   myShape;
    BodyTable                      myBodies;
    FeaturePtr                     myActiveFeature;
    std::size_t                    myRollbackIndex = npos;
    std::string                    myName = "Untitled";
    bool                           myIsModified = false;

    std::vector<Snapshot> myUndoStack;
    std::vector<Snapshot> myRedoStack;

    std::vector<DocumentObserver*> myObservers;
    bool myIsNotifying = false;
};

} // namespace lcad
