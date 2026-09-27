#include "core/Command.h"
#include "core/Document.h"
#include "sketch/SketchCommandGroups.h"
#include "sketch/SketchDisplay.h"
#include "sketch/SketchFeature.h"
#include "sketch/SketchModifyTools.h"
#include "sketch/SketchSelection.h"
#include "sketch/SketchTools.h"

#include <algorithm>
#include <memory>
#include <vector>

#include <QMainWindow>
#include <QStatusBar>
#include <QString>

namespace lcad {

namespace {

const char* const kSketchGroup = "Sketch";
const char* const kConstraintsSection = "Constraints";

void ShowStatus(const CommandContext& theContext, const QString& theText)
{
    if (QMainWindow* window = qobject_cast<QMainWindow*>(theContext.parent)) {
        window->statusBar()->showMessage(theText);
    }
}

bool HasCentre(SketchEntity::Kind theKind)
{
    return theKind == SketchEntity::Kind::Circle || theKind == SketchEntity::Kind::Arc
        || theKind == SketchEntity::Kind::Ellipse;
}

bool HasRadius(SketchEntity::Kind theKind)
{
    return theKind == SketchEntity::Kind::Circle || theKind == SketchEntity::Kind::Arc;
}

// What each pick can stand for. A constraint is only offered when the
// picks can actually play the parts it needs, which is what greys the
// buttons out the way Fusion's do.
struct PickInfo
{
    const SketchEntity* entity = nullptr;
    bool isLine = false;
    bool isCircular = false;   // has a centre: circle, arc or ellipse
    bool isRadial = false;     // has a radius the solver can equate
    bool isPoint = false;      // a characteristic point, not the curve
};

PickInfo Describe(const SketchFeature& theSketch, const SketchPointRef& theRef)
{
    PickInfo info;
    info.entity = theSketch.FindEntity(theRef.entity);
    if (info.entity == nullptr) {
        return info;
    }

    const bool whole = theRef.role == SketchPointRole::Whole;
    info.isPoint = !whole || info.entity->kind == SketchEntity::Kind::Point;
    info.isLine = whole && info.entity->kind == SketchEntity::Kind::Line;
    info.isCircular = whole && HasCentre(info.entity->kind);
    info.isRadial = whole && HasRadius(info.entity->kind);
    return info;
}

// Turn the current picks into the constraints a given command would add.
// Returns false when the picks don't suit the constraint, which is both
// the enable test and the apply step.
bool BuildConstraints(SketchConstraintType               theType,
                      const SketchFeature&               theSketch,
                      const std::vector<SketchPointRef>& thePicks,
                      std::vector<SketchConstraint>&     theResult)
{
    theResult.clear();

    std::vector<PickInfo> picks;
    picks.reserve(thePicks.size());
    for (const SketchPointRef& ref : thePicks) {
        const PickInfo info = Describe(theSketch, ref);
        if (info.entity == nullptr) {
            return false;
        }
        picks.push_back(info);
    }

    auto make = [theType](const SketchPointRef& theA, const SketchPointRef& theB,
                          const SketchPointRef& theC) {
        SketchConstraint constraint;
        constraint.type = theType;
        constraint.a = theA;
        constraint.b = theB;
        constraint.c = theC;
        return constraint;
    };

    switch (theType) {
        case SketchConstraintType::Fix: {
            // Fix applies to everything picked at once, unlike the
            // relations below, which pair two things up.
            for (const SketchPointRef& ref : thePicks) {
                theResult.push_back(make(SketchPointRef{ref.entity, SketchPointRole::Whole},
                                         SketchPointRef(), SketchPointRef()));
            }
            return !theResult.empty();
        }

        case SketchConstraintType::Coincident: {
            if (picks.size() != 2 || !picks[0].isPoint || !picks[1].isPoint) {
                return false;
            }
            if (thePicks[0] == thePicks[1]) {
                return false;
            }
            theResult.push_back(make(thePicks[0], thePicks[1], SketchPointRef()));
            return true;
        }

        case SketchConstraintType::Horizontal:
        case SketchConstraintType::Vertical: {
            if (picks.size() == 1 && picks[0].isLine) {
                theResult.push_back(make(thePicks[0], SketchPointRef(), SketchPointRef()));
                return true;
            }
            // Two points can be levelled with each other even when no
            // single line joins them.
            if (picks.size() == 2 && picks[0].isPoint && picks[1].isPoint) {
                theResult.push_back(make(thePicks[0], thePicks[1], SketchPointRef()));
                return true;
            }
            return false;
        }

        case SketchConstraintType::Parallel:
        case SketchConstraintType::Perpendicular:
        case SketchConstraintType::Collinear: {
            if (picks.size() != 2 || !picks[0].isLine || !picks[1].isLine) {
                return false;
            }
            theResult.push_back(make(thePicks[0], thePicks[1], SketchPointRef()));
            return true;
        }

        case SketchConstraintType::Equal: {
            if (picks.size() != 2) {
                return false;
            }
            const bool bothLines = picks[0].isLine && picks[1].isLine;
            const bool bothRadial = picks[0].isRadial && picks[1].isRadial;
            if (!bothLines && !bothRadial) {
                return false;
            }
            theResult.push_back(make(thePicks[0], thePicks[1], SketchPointRef()));
            return true;
        }

        case SketchConstraintType::Tangent: {
            if (picks.size() != 2) {
                return false;
            }
            const bool lineAndCircle = (picks[0].isLine && picks[1].isRadial)
                                    || (picks[0].isRadial && picks[1].isLine);
            const bool twoCircles = picks[0].isRadial && picks[1].isRadial;
            if (!lineAndCircle && !twoCircles) {
                return false;
            }
            theResult.push_back(make(thePicks[0], thePicks[1], SketchPointRef()));
            return true;
        }

        case SketchConstraintType::Midpoint: {
            if (picks.size() != 2) {
                return false;
            }
            // Either order is fine to pick in; the constraint always wants
            // the point first.
            if (picks[0].isPoint && picks[1].isLine) {
                theResult.push_back(make(thePicks[0], thePicks[1], SketchPointRef()));
                return true;
            }
            if (picks[1].isPoint && picks[0].isLine) {
                theResult.push_back(make(thePicks[1], thePicks[0], SketchPointRef()));
                return true;
            }
            return false;
        }

        case SketchConstraintType::Concentric: {
            if (picks.size() != 2 || !picks[0].isCircular || !picks[1].isCircular) {
                return false;
            }
            theResult.push_back(make(thePicks[0], thePicks[1], SketchPointRef()));
            return true;
        }

        case SketchConstraintType::Symmetric: {
            // Two things, then the line they straddle -- the order Fusion
            // asks for them in.
            if (picks.size() != 3 || !picks[2].isLine) {
                return false;
            }
            if (!picks[0].isPoint || !picks[1].isPoint) {
                return false;
            }
            theResult.push_back(make(thePicks[0], thePicks[1], thePicks[2]));
            return true;
        }

        default:
            return false;
    }
}

// True when every picked entity is already fixed, which is what turns the
// Fix button into an Unfix button.
bool AllFixed(const SketchFeature& theSketch, const std::vector<SketchPointRef>& thePicks)
{
    if (thePicks.empty()) {
        return false;
    }
    for (const SketchPointRef& ref : thePicks) {
        bool found = false;
        for (const SketchConstraint& constraint : theSketch.Constraints()) {
            if (constraint.type == SketchConstraintType::Fix
             && constraint.a.entity == ref.entity) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

// ---- commands ----

class SketchConstraintCommand : public Command
{
public:
    std::string Group() const override { return kSketchGroup; }
    std::string Section() const override { return kConstraintsSection; }
    std::string Title() const override { return SketchConstraint::TypeName(Type()); }

    bool IsEnabled(const CommandContext& theContext) const override
    {
        SketchFeature* sketch = SketchSession::Instance().ActiveSketch(theContext.document);
        if (sketch == nullptr) {
            return false;
        }
        SketchSelection::Instance().Prune(*sketch);
        std::vector<SketchConstraint> constraints;
        return BuildConstraints(Type(), *sketch, SketchSelection::Instance().Items(),
                                constraints);
    }

    void Execute(CommandContext& theContext) override
    {
        SketchFeature* sketch = SketchSession::Instance().ActiveSketch(theContext.document);
        if (sketch == nullptr || theContext.document == nullptr) {
            ShowStatus(theContext, "Create or re-open a sketch first.");
            return;
        }

        SketchSelection::Instance().Prune(*sketch);
        std::vector<SketchConstraint> constraints;
        if (!BuildConstraints(Type(), *sketch, SketchSelection::Instance().Items(),
                              constraints)) {
            ShowStatus(theContext, QString("%1 needs a different selection.")
                                       .arg(QString::fromLatin1(Title().c_str())));
            return;
        }

        SketchDisplay::Instance().Attach(theContext);
        theContext.document->PushUndoSnapshot();

        // Only an error the addition CAUSED says it over-constrained the
        // sketch. One already there -- a dimension whose expression no longer
        // evaluates stops the solve altogether -- would otherwise be blamed on
        // every constraint the user adds, and each would be deleted.
        const std::string errorBefore = sketch->LastError();
        std::vector<int> added;
        for (const SketchConstraint& constraint : constraints) {
            if (sketch->HasConstraint(constraint)) {
                continue;  // applying the same relation twice can only hurt
            }
            added.push_back(sketch->AddConstraint(constraint));
        }
        if (added.empty()) {
            ShowStatus(theContext, "Already constrained.");
            return;
        }

        theContext.document->Rebuild();

        // An addition the solver can't satisfy is an over-constrained
        // sketch; back it out rather than leaving the geometry stuck.
        if (!sketch->LastError().empty() && sketch->LastError() != errorBefore) {
            for (const int id : added) {
                sketch->RemoveConstraint(id);
            }
            theContext.document->Rebuild();
            ShowStatus(theContext, QString("%1 would over-constrain the sketch.")
                                       .arg(QString::fromLatin1(Title().c_str())));
            return;
        }

        SketchSelection::Instance().Clear();
        SketchDisplay::Instance().Refresh();
        SketchDisplay::Instance().Redraw();
        ShowStatus(theContext, QString("%1 applied.")
                                   .arg(QString::fromLatin1(Title().c_str())));
    }

protected:
    virtual SketchConstraintType Type() const = 0;
};

// Little template-free boilerplate saver: every relation differs only in
// its type and id -- its icon is the SVG named after the id.
#define LCAD_CONSTRAINT_COMMAND(ClassName, TypeValue, IdText)                          \
    class ClassName : public SketchConstraintCommand                                    \
    {                                                                                   \
    public:                                                                             \
        std::string Id() const override { return IdText; }                              \
        std::string Icon() const override { return ":/icons/" IdText ".svg"; }           \
        std::string Description() const override                                        \
        {                                                                               \
            return std::string("Apply a ") + SketchConstraint::TypeName(TypeValue)       \
                 + " constraint to the selected sketch geometry";                       \
        }                                                                               \
                                                                                        \
    protected:                                                                          \
        SketchConstraintType Type() const override { return TypeValue; }                 \
    }

LCAD_CONSTRAINT_COMMAND(CoincidentCommand, SketchConstraintType::Coincident,
                        "sketch.constraint.coincident");
LCAD_CONSTRAINT_COMMAND(HorizontalCommand, SketchConstraintType::Horizontal,
                        "sketch.constraint.horizontal");
LCAD_CONSTRAINT_COMMAND(VerticalCommand, SketchConstraintType::Vertical,
                        "sketch.constraint.vertical");
LCAD_CONSTRAINT_COMMAND(ParallelCommand, SketchConstraintType::Parallel,
                        "sketch.constraint.parallel");
LCAD_CONSTRAINT_COMMAND(PerpendicularCommand, SketchConstraintType::Perpendicular,
                        "sketch.constraint.perpendicular");
LCAD_CONSTRAINT_COMMAND(EqualCommand, SketchConstraintType::Equal,
                        "sketch.constraint.equal");
LCAD_CONSTRAINT_COMMAND(TangentCommand, SketchConstraintType::Tangent,
                        "sketch.constraint.tangent");
LCAD_CONSTRAINT_COMMAND(MidpointCommand, SketchConstraintType::Midpoint,
                        "sketch.constraint.midpoint");
LCAD_CONSTRAINT_COMMAND(ConcentricCommand, SketchConstraintType::Concentric,
                        "sketch.constraint.concentric");
LCAD_CONSTRAINT_COMMAND(CollinearCommand, SketchConstraintType::Collinear,
                        "sketch.constraint.collinear");
LCAD_CONSTRAINT_COMMAND(SymmetricCommand, SketchConstraintType::Symmetric,
                        "sketch.constraint.symmetry");

#undef LCAD_CONSTRAINT_COMMAND

// Fix is the one relation with an obvious opposite, so the single button
// does both jobs depending on what is picked.
class FixCommand : public SketchConstraintCommand
{
public:
    std::string Id() const override { return "sketch.constraint.fix"; }
    std::string Icon() const override { return ":/icons/sketch.constraint.fix.svg"; }

    std::string Title() const override { return "Fix / Unfix"; }

    std::string Description() const override
    {
        return "Lock the selected sketch geometry in place, or release it again";
    }

    bool IsEnabled(const CommandContext& theContext) const override
    {
        SketchFeature* sketch = SketchSession::Instance().ActiveSketch(theContext.document);
        if (sketch == nullptr) {
            return false;
        }
        SketchSelection::Instance().Prune(*sketch);
        return !SketchSelection::Instance().IsEmpty();
    }

    void Execute(CommandContext& theContext) override
    {
        SketchFeature* sketch = SketchSession::Instance().ActiveSketch(theContext.document);
        if (sketch == nullptr || theContext.document == nullptr) {
            return;
        }

        SketchSelection::Instance().Prune(*sketch);
        const std::vector<SketchPointRef>& picks = SketchSelection::Instance().Items();
        if (picks.empty()) {
            return;
        }

        if (!AllFixed(*sketch, picks)) {
            SketchConstraintCommand::Execute(theContext);
            return;
        }

        SketchDisplay::Instance().Attach(theContext);
        theContext.document->PushUndoSnapshot();

        std::vector<int> doomed;
        for (const SketchConstraint& constraint : sketch->Constraints()) {
            if (constraint.type != SketchConstraintType::Fix) {
                continue;
            }
            for (const SketchPointRef& ref : picks) {
                if (constraint.a.entity == ref.entity) {
                    doomed.push_back(constraint.id);
                    break;
                }
            }
        }
        for (const int id : doomed) {
            sketch->RemoveConstraint(id);
        }

        SketchSelection::Instance().Clear();
        theContext.document->Rebuild();
        ShowStatus(theContext, "Unfixed.");
    }

protected:
    SketchConstraintType Type() const override { return SketchConstraintType::Fix; }
};

// ---- Inspect ----

class SketchDimensionCommand : public Command
{
public:
    std::string Id() const override { return "sketch.dimension"; }
    std::string Title() const override { return "Sketch Dimension"; }
    std::string Group() const override { return kSketchGroup; }
    std::string Section() const override { return "Inspect"; }
    std::string Icon() const override { return ":/icons/sketch.dimension.svg"; }
    std::string Shortcut() const override { return "D"; }

    std::string Description() const override
    {
        return "Dimension a length, radius, diameter, distance or angle -- and drive the "
               "geometry from it";
    }

    bool IsEnabled(const CommandContext& theContext) const override
    {
        return SketchSession::Instance().ActiveSketch(theContext.document) != nullptr;
    }

    void Execute(CommandContext& theContext) override
    {
        if (SketchSession::Instance().ActiveSketch(theContext.document) == nullptr) {
            ShowStatus(theContext, "Create or re-open a sketch first.");
            return;
        }
        SketchDisplay::Instance().Attach(theContext);
        SketchDimensionTool().Start(theContext);
    }
};

// Every dimension the sketch carries, cleared in one go -- the escape
// hatch when a sketch has been driven somewhere it shouldn't have gone.
class RemoveDimensionsCommand : public Command
{
public:
    std::string Id() const override { return "sketch.dimension.clear"; }
    std::string Title() const override { return "Delete Constraints"; }
    std::string Group() const override { return kSketchGroup; }
    std::string Section() const override { return "Inspect"; }
    std::string Icon() const override { return ":/icons/sketch.dimension.clear.svg"; }

    std::string Description() const override
    {
        return "Remove every constraint and dimension from the active sketch, leaving the "
               "geometry where it is";
    }

    bool IsEnabled(const CommandContext& theContext) const override
    {
        SketchFeature* sketch = SketchSession::Instance().ActiveSketch(theContext.document);
        return sketch != nullptr && !sketch->Constraints().empty();
    }

    void Execute(CommandContext& theContext) override
    {
        SketchFeature* sketch = SketchSession::Instance().ActiveSketch(theContext.document);
        if (sketch == nullptr || theContext.document == nullptr) {
            return;
        }

        SketchDisplay::Instance().Attach(theContext);
        theContext.document->PushUndoSnapshot();
        sketch->ClearConstraints();
        theContext.document->Rebuild();
        ShowStatus(theContext, "Sketch constraints removed.");
    }
};

} // namespace

void AddSketchConstraintCommands(CommandRegistry& theRegistry)
{
    theRegistry.Add(std::make_unique<CoincidentCommand>());
    theRegistry.Add(std::make_unique<HorizontalCommand>());
    theRegistry.Add(std::make_unique<VerticalCommand>());
    theRegistry.Add(std::make_unique<ParallelCommand>());
    theRegistry.Add(std::make_unique<PerpendicularCommand>());
    theRegistry.Add(std::make_unique<EqualCommand>());
    theRegistry.Add(std::make_unique<TangentCommand>());
    theRegistry.Add(std::make_unique<MidpointCommand>());
    theRegistry.Add(std::make_unique<ConcentricCommand>());
    theRegistry.Add(std::make_unique<CollinearCommand>());
    theRegistry.Add(std::make_unique<FixCommand>());
    theRegistry.Add(std::make_unique<SymmetricCommand>());

    theRegistry.Add(std::make_unique<SketchDimensionCommand>());
    theRegistry.Add(std::make_unique<RemoveDimensionsCommand>());
}

} // namespace lcad
