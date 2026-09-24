#pragma once

#include "core/ProfileProvider.h"
#include "features/ProfileFeatures.h"

#include <TopoDS_Wire.hxx>
#include <gp_Pln.hxx>

#include <string>
#include <vector>

namespace lcad {

// One section of a loft: the sketch it is drawn in, and -- when that
// sketch holds more than one closed region -- which region to blend
// through.
//
// The sketch is named rather than pointed at for the reason every other
// feature names its inputs: undo clones the whole timeline, so a pointer
// would outlive the feature it pointed at.
struct LoftSection
{
    std::string sketch;

    // Null means "the sketch's only closed region". A sketch holding two
    // of them is ambiguous, and a loft answers that by refusing rather
    // than by taking the bigger one.
    ProfileRef profile;

    // "Sketch1", or "Sketch1:3,4,5,6@12.5,7.25" when a region is named.
    // Rides the plain string Parameter the properties panel already
    // shows and edits, exactly as a ProfileRef does.
    std::string Encode() const;
    static bool Decode(const std::string& theText, LoftSection& theResult);
};

// The ordered list in one row: "Sketch1; Sketch2; Sketch3".
std::string EncodeLoftSections(const std::vector<LoftSection>& theSections);

// All or nothing: one unparseable section refuses the whole list. Keeping
// the segments that did parse would loft through a different set of
// sections than the text in front of the user says.
bool DecodeLoftSections(const std::string& theText, std::vector<LoftSection>& theSections);

// Fusion's CREATE > LOFT: a solid blended through two or more profiles.
//
// This is the one profile feature that needs a LIST of sketches rather
// than one, so the inherited single-sketch field stays empty and the
// sections below stand in for it. What is inherited is the operation
// dropdown, the by-name resolve convention, and the clone bookkeeping
// that undo depends on.
//
// ORDER IS PART OF THE ANSWER. The sections are blended in the order they
// are listed -- section one into section two, and so on -- so swapping
// two of them is a different solid rather than the same one described
// differently. That is why they are held in a vector and surfaced as an
// ordered row the user can retype, instead of being gathered into a set.
class LoftFeature : public ProfileFeature
{
public:
    LoftFeature() = default;
    explicit LoftFeature(std::vector<LoftSection> theSections);

    std::string TypeName() const override { return "Loft"; }

    bool Compute(const ComputeContext& theContext,
                 const TopoDS_Shape&   theInput,
                 TopoDS_Shape&         theOutput,
                 std::string&          theError) override;

    std::unique_ptr<Feature> Clone() const override;

    std::vector<Parameter> Parameters() const override;
    bool SetParameter(const Parameter& theParameter) override;

    const std::vector<LoftSection>& Sections() const { return mySections; }
    void SetSections(std::vector<LoftSection> theSections)
    {
        // Deliberately no de-duplication, unlike ProfileFeature's regions:
        // a loft that leaves a profile and comes back to it is a shape a
        // user can mean, and the pairs that really are degenerate -- the
        // same section twice in a row -- are caught at compute time with a
        // message that says which two.
        mySections = std::move(theSections);
    }

    // Loop the last section back to the first, the way Fusion's Closed
    // checkbox does. Needs three sections: with two, the loop would blend
    // the same pair twice and the second pass would land on the first.
    bool IsClosed() const { return myIsClosed; }
    void SetClosed(bool theValue) { myIsClosed = theValue; }

    // Straight transitions between neighbouring sections instead of one
    // smooth surface through all of them.
    bool IsRuled() const { return myIsRuled; }
    void SetRuled(bool theValue) { myIsRuled = theValue; }

private:
    // One closed wire per section, plus the plane each was drawn on so the
    // degenerate arrangements can be caught before OCCT is handed them.
    bool ResolveSections(const ComputeContext&     theContext,
                         std::vector<TopoDS_Wire>& theWires,
                         std::vector<gp_Pln>&      thePlanes,
                         std::string&              theError) const;

    std::vector<LoftSection> mySections;
    bool                     myIsClosed = false;
    bool                     myIsRuled  = false;
};

} // namespace lcad
