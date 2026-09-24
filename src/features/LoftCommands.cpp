#include "core/Command.h"
#include "core/Document.h"
#include "core/ProfileProvider.h"
#include "features/FeatureDialogs.h"
#include "features/FeatureUtils.h"
#include "features/LoftFeature.h"

#include <TopExp_Explorer.hxx>
#include <TopoDS_Shape.hxx>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include <QMainWindow>
#include <QStatusBar>
#include <QString>
#include <QStringList>

namespace lcad {

namespace {

const char* const kSolidGroup = "Solid";

// Eight dropdowns already make a tall dialog, and a loft through more
// sections than that is easier to type into the properties row than to
// pick from eight combo boxes.
constexpr std::size_t kMaxSectionRows = 8;

void ShowStatus(const CommandContext& theContext, const QString& theText)
{
    if (QMainWindow* window = qobject_cast<QMainWindow*>(theContext.parent)) {
        window->statusBar()->showMessage(theText);
    }
}

QStringList ToChoices(const std::vector<std::string>& theNames)
{
    QStringList choices;
    for (const std::string& name : theNames) {
        choices << QString::fromStdString(name);
    }
    return choices;
}

// Sketches are found by type name, not by class: the solid subsystem
// deliberately knows nothing about SketchFeature's header.
std::vector<std::string> SketchNames(const Document* theDocument)
{
    std::vector<std::string> names;
    if (theDocument == nullptr) {
        return names;
    }
    for (const FeaturePtr& feature : theDocument->Features()) {
        if (feature && feature->TypeName() == "Sketch") {
            names.push_back(feature->Name());
        }
    }
    return names;
}

// One thing a section row can name.
struct SectionChoice
{
    QString     label;
    LoftSection section;
    bool        hasProfile = false;
    bool        singleLoop = true;
};

// A loft section is one closed loop. The ring of a rectangle drawn around
// a circle is a perfectly good profile with a hole in it, which a blend
// cannot use.
bool IsSingleLoop(const TopoDS_Shape& theFace)
{
    int loops = 0;
    for (TopExp_Explorer explorer(theFace, TopAbs_WIRE); explorer.More(); explorer.Next()) {
        ++loops;
    }
    return loops == 1;
}

// Everything the section rows can offer. A sketch with a single closed
// region is named outright -- no region reference, so the loft follows
// that region as the sketch is edited -- while a sketch split into
// several offers one entry per region. That second case is the only way a
// loft can reach a particular region: the sections are named here, not
// clicked in the viewport, and a sketch holding two regions is otherwise
// ambiguous and refused at rebuild time.
std::vector<SectionChoice> SectionChoices(const Document* theDocument)
{
    std::vector<SectionChoice> choices;
    if (theDocument == nullptr) {
        return choices;
    }

    for (const FeaturePtr& feature : theDocument->Features()) {
        if (!feature || feature->TypeName() != "Sketch") {
            continue;
        }

        std::vector<ProfileRegion> regions;
        if (ProfileProvider* provider = AsProfileProvider(feature.get())) {
            regions = provider->ProfileRegions();
        }

        // A region with no reference cannot be stored durably, so a
        // provider offering those is treated as offering none.
        std::vector<const ProfileRegion*> named;
        for (const ProfileRegion& region : regions) {
            if (!region.ref.IsNull()) {
                named.push_back(&region);
            }
        }

        const QString name = QString::fromStdString(feature->Name());

        if (named.size() < 2) {
            SectionChoice choice;
            choice.label = name;
            choice.section.sketch = feature->Name();
            choice.hasProfile = !regions.empty();
            choice.singleLoop = regions.empty() || IsSingleLoop(regions.front().face);
            choices.push_back(std::move(choice));
            continue;
        }

        // Regions arrive largest first, which is what the labels promise
        // and the only handle a user has on which one is which.
        for (std::size_t i = 0; i < named.size(); ++i) {
            SectionChoice choice;
            choice.label = name
                         + QStringLiteral(" (profile %1 of %2)")
                               .arg(static_cast<int>(i) + 1)
                               .arg(static_cast<int>(named.size()));
            choice.section.sketch = feature->Name();
            choice.section.profile = named[i]->ref;
            choice.hasProfile = true;
            choice.singleLoop = IsSingleLoop(named[i]->face);
            choices.push_back(std::move(choice));
        }
    }
    return choices;
}

// Dropdown index of the first entry for a sketch that a loft can actually
// use, its first entry otherwise, or 0 for "(none)". A profile with a hole
// is still offered -- hiding it would be stranger than saying why it
// cannot be blended -- but it is not what a row should open on.
int FirstChoiceFor(const std::vector<SectionChoice>& theChoices, const std::string& theSketch)
{
    int fallback = 0;
    for (std::size_t i = 0; i < theChoices.size(); ++i) {
        if (theChoices[i].section.sketch != theSketch) {
            continue;
        }
        // "(none)" holds index 0, so entry i sits at i + 1.
        const int choice = static_cast<int>(i) + 1;
        if (theChoices[i].singleLoop) {
            return choice;
        }
        if (fallback == 0) {
            fallback = choice;
        }
    }
    return fallback;
}

bool DocumentHasBody(const CommandContext& theContext)
{
    return theContext.document != nullptr && HasSolid(theContext.document->Shape());
}

// Fusion's default: add to the existing body if there is one, otherwise
// start a new one.
int DefaultOperationChoice(const CommandContext& theContext)
{
    return DocumentHasBody(theContext) ? static_cast<int>(BooleanOp::Join)
                                       : static_cast<int>(BooleanOp::NewBody);
}

// Adds the feature and reports what the rebuild made of it. A feature
// that failed stays in the timeline on purpose -- the properties panel is
// where the user fixes the section list that was wrong.
void AddAndReport(CommandContext& theContext, const FeaturePtr& theFeature)
{
    if (theContext.document == nullptr || !theFeature) {
        return;
    }

    theContext.document->AddFeature(theFeature);
    theContext.document->SetActiveFeature(theFeature);

    if (!theFeature->LastError().empty()) {
        // Already carries the feature name, courtesy of Document::Rebuild.
        ShowStatus(theContext, QString::fromStdString(theFeature->LastError()));
        return;
    }
    ShowStatus(theContext, QString::fromStdString(theFeature->Name()) + " created.");
}

// Fusion's CREATE > LOFT, beside Extrude and Revolve.
class LoftCommand : public Command
{
public:
    std::string Id() const override { return "solid.loft"; }
    std::string Title() const override { return "Loft"; }
    std::string Group() const override { return kSolidGroup; }
    std::string Section() const override { return "Create"; }
    std::string Icon() const override { return "🏺"; }
    std::string Shortcut() const override { return "Shift+L"; }
    std::string Description() const override
    {
        return "Blend a solid through two or more sketch profiles, in the order given";
    }

    // Two sketches is the least a loft can be built from, so the button
    // greys out until there are two rather than opening a dialog that
    // cannot be answered.
    bool IsEnabled(const CommandContext& theContext) const override
    {
        return SketchNames(theContext.document).size() >= 2;
    }

    void Execute(CommandContext& theContext) override
    {
        if (theContext.document == nullptr) {
            return;
        }

        const std::vector<std::string> sketches = SketchNames(theContext.document);
        if (sketches.size() < 2) {
            ShowStatus(theContext, "Draw at least two sketches to loft between.");
            return;
        }

        const std::vector<SectionChoice> choices = SectionChoices(theContext.document);
        if (choices.empty()) {
            return;
        }

        QStringList labels;
        labels << QStringLiteral("(none)");
        for (const SectionChoice& choice : choices) {
            labels << choice.label;
        }

        // One dropdown per section, in order, because the order IS the
        // shape: section one blends into section two and so on. The two
        // newest sketches lead, oldest first -- for the usual "draw two
        // profiles, loft them" that is the whole answer -- and the
        // remaining rows start empty.
        const std::size_t rows = std::min(sketches.size(), kMaxSectionRows);
        std::vector<DialogField> fields;
        for (std::size_t i = 0; i < rows; ++i) {
            const int preset =
                i < 2 ? FirstChoiceFor(choices, sketches[sketches.size() - 2 + i]) : 0;
            fields.push_back(DialogField::Choice(
                QStringLiteral("Section %1").arg(static_cast<int>(i) + 1), labels, preset));
        }
        fields.push_back(DialogField::Toggle("Closed", false));
        fields.push_back(DialogField::Toggle("Ruled", false));
        fields.push_back(DialogField::Choice("Operation",
                                             ToChoices(BooleanOpNames()),
                                             DefaultOperationChoice(theContext)));

        QString hint = QStringLiteral("Sections are blended in the order shown. Closed loops "
                                      "the last section back to the first, which needs three.");
        if (choices.size() > sketches.size()) {
            hint += QStringLiteral(" A sketch split into several profiles lists them "
                                   "largest first.");
        }

        if (!ShowFeatureDialog(theContext.parent, "Loft", fields, hint)) {
            return;
        }

        std::vector<LoftSection> sections;
        for (std::size_t i = 0; i < rows; ++i) {
            const int picked = fields[i].choice;
            if (picked <= 0 || picked > static_cast<int>(choices.size())) {
                continue;   // "(none)": the row is unused
            }

            const SectionChoice& choice = choices[static_cast<std::size_t>(picked) - 1];
            if (!choice.hasProfile) {
                ShowStatus(theContext, QString::fromStdString(choice.section.sketch)
                                           + " has no closed profile -- close the loop first.");
                return;
            }
            sections.push_back(choice.section);
        }

        // Checked here as well as in the feature so an answer that cannot
        // build never becomes a timeline entry; the feature still checks
        // because the section list is editable afterwards.
        if (sections.size() < 2) {
            ShowStatus(theContext, "Pick at least two sections to loft through.");
            return;
        }
        const bool closed = fields[rows].toggle;
        if (closed && sections.size() < 3) {
            ShowStatus(theContext, "A closed loft needs at least three sections.");
            return;
        }

        auto loft = std::make_shared<LoftFeature>(std::move(sections));
        loft->SetClosed(closed);
        loft->SetRuled(fields[rows + 1].toggle);
        loft->SetOperation(BooleanOpFromInt(fields[rows + 2].choice));
        AddAndReport(theContext, loft);
    }
};

} // namespace

void RegisterLoftCommands(CommandRegistry& theRegistry)
{
    theRegistry.Add(std::make_unique<LoftCommand>());
}

} // namespace lcad
