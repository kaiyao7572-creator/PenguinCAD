#include "core/ParameterTable.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace lcad {

namespace {

bool EqualNoCase(const std::string& theLeft, const std::string& theRight)
{
    if (theLeft.size() != theRight.size()) {
        return false;
    }
    for (std::size_t i = 0; i < theLeft.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(theLeft[i]))
            != std::tolower(static_cast<unsigned char>(theRight[i]))) {
            return false;
        }
    }
    return true;
}

std::string Quoted(const std::string& theName)
{
    return "\"" + theName + "\"";
}

// One entry of the dependency walk: a row, and how many of its edges have
// been followed.
struct Frame
{
    std::size_t node = 0;
    std::size_t next = 0;
};

// How many names a cycle message spells out before it gives up and counts
// them instead. A loop of four is something to read; a loop of four
// hundred is a wall of text with the useful part -- that it is a loop, and
// where it starts -- buried in it.
constexpr std::size_t kMaxNamedInCycle = 8;

// "circular reference: a -> b -> c -> a", from the part of the walk that
// is going round.
std::string CycleMessage(const std::vector<UserParameter>& theParameters,
                         const std::vector<Frame>& theStack, std::size_t theBegin,
                         std::size_t theClosing)
{
    const std::size_t length = theStack.size() - theBegin;

    std::string message = "circular reference: ";
    for (std::size_t i = theBegin; i < theStack.size(); ++i) {
        if (i - theBegin == kMaxNamedInCycle) {
            message += "... -> ";
            break;
        }
        message += theParameters[theStack[i].node].name + " -> ";
    }
    message += theParameters[theClosing].name;

    if (length > kMaxNamedInCycle) {
        message += " (" + std::to_string(length) + " parameters)";
    }
    return message;
}

} // namespace

std::string UserParameter::DisplayText(int theMaxDecimals) const
{
    // A row that did not resolve has no value to show. Printing the zero
    // it was reset to would read as a real dimension.
    if (!isValid) {
        return std::string();
    }
    return FormatValue(value, kind, lengthUnit, angleUnit, theMaxDecimals);
}

// ---------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------

int ParameterTable::IndexOf(const std::string& theName) const
{
    for (std::size_t i = 0; i < myParameters.size(); ++i) {
        if (myParameters[i].name == theName) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

const UserParameter* ParameterTable::Find(const std::string& theName) const
{
    const int index = IndexOf(theName);
    return index < 0 ? nullptr : &myParameters[static_cast<std::size_t>(index)];
}

std::vector<std::string> ParameterTable::InvalidParameters() const
{
    std::vector<std::string> names;
    for (const UserParameter& parameter : myParameters) {
        if (!parameter.isValid) {
            names.push_back(parameter.name);
        }
    }
    return names;
}

std::vector<std::vector<std::size_t>> ParameterTable::BuildGraph() const
{
    // One map for the whole pass. IndexOf is a linear scan, and looking
    // every name up that way turns building the graph into n-squared work
    // on its own -- multiplied again by resolving after every edit.
    std::unordered_map<std::string, std::size_t> byName;
    byName.reserve(myParameters.size() * 2);
    for (std::size_t i = 0; i < myParameters.size(); ++i) {
        byName.emplace(myParameters[i].name, i);
    }

    std::vector<std::vector<std::size_t>> graph(myParameters.size());
    for (std::size_t i = 0; i < myParameters.size(); ++i) {
        for (const std::string& name : ExpressionVariables(myParameters[i].expression)) {
            const auto found = byName.find(name);
            if (found != byName.end()) {
                graph[i].push_back(found->second);
            }
        }
    }
    return graph;
}

std::vector<std::string> ParameterTable::Dependents(const std::string& theName) const
{
    const int start = IndexOf(theName);
    if (start < 0) {
        return {};
    }

    const std::vector<std::vector<std::size_t>> graph = BuildGraph();
    const std::size_t root = static_cast<std::size_t>(start);

    std::vector<bool>        found(myParameters.size(), false);
    std::vector<std::size_t> pending{root};
    while (!pending.empty()) {
        const std::size_t target = pending.back();
        pending.pop_back();
        for (std::size_t i = 0; i < graph.size(); ++i) {
            if (found[i] || i == root) {
                continue;
            }
            if (std::find(graph[i].begin(), graph[i].end(), target) != graph[i].end()) {
                found[i] = true;
                pending.push_back(i);
            }
        }
    }

    std::vector<std::string> names;
    for (std::size_t i = 0; i < found.size(); ++i) {
        if (found[i]) {
            names.push_back(myParameters[i].name);
        }
    }
    return names;
}

// ---------------------------------------------------------------------
// Ordering
// ---------------------------------------------------------------------

bool ParameterTable::Order(const std::vector<std::vector<std::size_t>>& theGraph,
                           std::vector<std::size_t>&                    theOrder,
                           std::vector<std::string>&                    theCycleErrors) const
{
    const std::size_t n = myParameters.size();

    theOrder.clear();
    theOrder.reserve(n);
    theCycleErrors.assign(n, std::string());

    enum Colour
    {
        White = 0,   // not visited
        Grey,        // on the stack -- an edge back to one of these is a loop
        Black        // finished
    };
    std::vector<int> colour(n, White);

    // Depth-first with an explicit stack rather than recursion. A chain of
    // several thousand parameters is a strange model but not an illegal
    // one, and running the C stack out is not an error message.
    std::vector<Frame> stack;

    bool acyclic = true;

    for (std::size_t root = 0; root < n; ++root) {
        if (colour[root] != White) {
            continue;
        }
        colour[root] = Grey;
        stack.push_back(Frame{root, 0});

        while (!stack.empty()) {
            const std::size_t node = stack.back().node;

            if (stack.back().next < theGraph[node].size()) {
                const std::size_t child = theGraph[node][stack.back().next++];

                if (colour[child] == Grey) {
                    acyclic = false;

                    // The loop is the part of the stack from the node we
                    // came back to, through the one we are on, and round.
                    // Naming it is the whole point: "circular reference"
                    // on its own leaves the user to work out which three
                    // of forty parameters are involved.
                    std::size_t begin = 0;
                    while (begin < stack.size() && stack[begin].node != child) {
                        ++begin;
                    }

                    const std::string message = CycleMessage(myParameters, stack, begin, child);
                    for (std::size_t i = begin; i < stack.size(); ++i) {
                        if (theCycleErrors[stack[i].node].empty()) {
                            theCycleErrors[stack[i].node] = message;
                        }
                    }
                    // The edge itself is not followed: the rows in the
                    // loop are already marked, and walking round again
                    // would only find the same loop.
                } else if (colour[child] == White) {
                    colour[child] = Grey;
                    stack.push_back(Frame{child, 0});
                }
                continue;
            }

            colour[node] = Black;
            theOrder.push_back(node);
            stack.pop_back();
        }
    }

    return acyclic;
}

// ---------------------------------------------------------------------
// Evaluating
// ---------------------------------------------------------------------

TypedVariableLookup ParameterTable::Lookup() const
{
    const ParameterTable* table = this;
    return [table](const std::string& theName, double& theValue, UnitKind& theKind) {
        const UserParameter* parameter = table->Find(theName);
        if (parameter == nullptr || !parameter->isValid) {
            return false;
        }
        theValue = parameter->value;
        theKind = parameter->kind;
        return true;
    };
}

ExpressionResult ParameterTable::EvaluateValue(const std::string& theText, UnitKind theKind,
                                               LengthUnit theLengthUnit,
                                               AngleUnit  theAngleUnit) const
{
    ExpressionResult result = EvaluateValueWith(theText, theKind, theLengthUnit, theAngleUnit,
                                                Lookup());
    if (!result.ok) {
        // Lookup() answers only for rows that resolved, so a row that exists
        // but is broken reaches the evaluator as a name it has never heard
        // of. Say what is really wrong, the way Resolve() does for rows --
        // "there is no parameter named hole_dia" sends the user looking for
        // a parameter that is sitting right there in the table.
        for (const std::string& name : ExpressionVariables(theText)) {
            const UserParameter* row = Find(name);
            if (row != nullptr && !row->isValid) {
                result.error = Quoted(name) + " has an error: " + row->error;
                break;
            }
        }
    }
    return result;
}

ExpressionResult ParameterTable::EvaluateValueWith(const std::string& theText, UnitKind theKind,
                                                   LengthUnit theLengthUnit,
                                                   AngleUnit  theAngleUnit,
                                                   const TypedVariableLookup& theLookup)
{
    // Always the evaluator, never a short cut through Units.h's own
    // fraction reading: ParseValue("1/0") comes back as a contented 1,
    // and a division by zero that answers is exactly the kind of quiet
    // wrongness this engine is supposed to make impossible.
    ExpressionResult result = EvaluateTypedExpression(theText, theLookup);
    if (!result.ok) {
        return result;
    }

    // The unit check Fusion makes: a length cannot drive an angle, and a
    // count cannot be 4 mm of anything.
    if (!FitsKind(result, theKind)) {
        const std::string what = DescribeDimension(result.lengthPower, result.anglePower);
        const std::string wanted = theKind == UnitKind::Length  ? "a length"
                                   : theKind == UnitKind::Angle ? "an angle"
                                                                : "a plain number";
        result.ok = false;
        result.value = 0.0;
        result.error = Quoted(theText) + " is " + what + ", and this needs " + wanted;
        return result;
    }

    // The unit rule, stated in the header: a dimensionless expression is
    // read in the field's own unit, anything else is already internal.
    //
    // A field already showing the internal unit converts by 1, so there
    // is nothing to decide and nothing to read the expression twice for.
    // That is most rows, and resolving runs on every document change.
    const bool converts = (theKind == UnitKind::Length && theLengthUnit != LengthUnit::Millimeter)
                          || (theKind == UnitKind::Angle && theAngleUnit != AngleUnit::Degree);
    if (converts && IsUnitlessExpression(theText)) {
        result.value = (theKind == UnitKind::Length) ? ToMillimeters(result.value, theLengthUnit)
                                                     : ToDegrees(result.value, theAngleUnit);
    }
    return result;
}

ExpressionResult ParameterTable::Evaluate(const std::string& theText) const
{
    return EvaluateValue(theText, UnitKind::Unitless);
}

bool ParameterTable::Resolve()
{
    const std::vector<std::vector<std::size_t>> graph = BuildGraph();

    std::vector<std::size_t> order;
    std::vector<std::string> cycleErrors;
    Order(graph, order, cycleErrors);

    return ResolveWith(graph, order, cycleErrors);
}

bool ParameterTable::ResolveWith(const std::vector<std::vector<std::size_t>>& theGraph,
                                 const std::vector<std::size_t>&              theOrder,
                                 const std::vector<std::string>&              theCycleErrors)
{
    for (std::size_t i = 0; i < myParameters.size(); ++i) {
        myParameters[i].value = 0.0;
        myParameters[i].isValid = false;
        myParameters[i].error = theCycleErrors[i];
    }

    const TypedVariableLookup lookup = Lookup();

    bool allResolved = true;

    for (std::size_t index : theOrder) {
        UserParameter& parameter = myParameters[index];
        if (!parameter.error.empty()) {   // caught in a loop; there is no order for it
            allResolved = false;
            continue;
        }

        // Inputs first. A row whose input is broken must say so rather
        // than report that input's NAME as unknown: it is not unknown, it
        // is broken, and those send the user to two different places.
        for (std::size_t other : theGraph[index]) {
            if (!myParameters[other].isValid) {
                parameter.error =
                    "depends on " + Quoted(myParameters[other].name) + ", which has an error";
                break;
            }
        }
        if (!parameter.error.empty()) {
            allResolved = false;
            continue;
        }

        const ExpressionResult result =
            EvaluateValueWith(parameter.expression, parameter.kind, parameter.lengthUnit,
                              parameter.angleUnit, lookup);
        if (!result.ok) {
            parameter.error = result.error;
            allResolved = false;
            continue;
        }

        parameter.value = result.value;
        parameter.isValid = true;
    }

    return allResolved;
}

// ---------------------------------------------------------------------
// Editing
// ---------------------------------------------------------------------

bool ParameterTable::IsNameAvailable(const std::string& theName, int theIgnoredIndex,
                                     std::string& theError) const
{
    if (theName.empty()) {
        theError = "a parameter needs a name";
        return false;
    }
    if (!IsExpressionIdentifier(theName)) {
        theError = Quoted(theName)
                   + " cannot be a parameter name: start with a letter or _, then letters,"
                     " digits or _";
        return false;
    }
    if (IsReservedExpressionName(theName)) {
        theError = Quoted(theName) + " is already part of the expression language";
        return false;
    }
    for (std::size_t i = 0; i < myParameters.size(); ++i) {
        if (static_cast<int>(i) == theIgnoredIndex) {
            continue;
        }
        // Without case, because two parameters differing only in case are
        // a trap for whoever reads the model next -- and the evaluator
        // matches exactly, so only one of them would ever be reachable by
        // someone who misremembered which it was.
        if (EqualNoCase(myParameters[i].name, theName)) {
            theError = "there is already a parameter named " + Quoted(myParameters[i].name);
            return false;
        }
    }
    theError.clear();
    return true;
}

bool ParameterTable::FinishEdit(const std::vector<UserParameter>& theSnapshot,
                                std::string&                      theError)
{
    const std::vector<std::vector<std::size_t>> graph = BuildGraph();

    std::vector<std::size_t> order;
    std::vector<std::string> cycleErrors;
    if (!Order(graph, order, cycleErrors)) {
        // The table had no loop before this edit -- that is invariant 1 --
        // so this edit is what made one. Put it back untouched and say so.
        for (const std::string& cycle : cycleErrors) {
            if (!cycle.empty()) {
                theError = cycle;
                break;
            }
        }
        myParameters = theSnapshot;
        return false;
    }

    theError.clear();
    ResolveWith(graph, order, cycleErrors);
    return true;
}

bool ParameterTable::Add(const UserParameter& theParameter, std::string& theError)
{
    if (!IsNameAvailable(theParameter.name, -1, theError)) {
        return false;
    }

    const std::vector<UserParameter> snapshot = myParameters;

    // Resolve() owns the three result fields. A caller that filled them in
    // would have its guess stand until something else forced a rebuild.
    UserParameter added = theParameter;
    added.value = 0.0;
    added.isValid = false;
    added.error.clear();
    myParameters.push_back(added);

    return FinishEdit(snapshot, theError);
}

bool ParameterTable::Add(const std::string& theName, const std::string& theExpression,
                         std::string& theError)
{
    UserParameter parameter;
    parameter.name = theName;
    parameter.expression = theExpression;
    return Add(parameter, theError);
}

bool ParameterTable::SetExpression(const std::string& theName, const std::string& theExpression,
                                   std::string& theError)
{
    const int index = IndexOf(theName);
    if (index < 0) {
        theError = "there is no parameter named " + Quoted(theName);
        return false;
    }

    const std::vector<UserParameter> snapshot = myParameters;
    myParameters[static_cast<std::size_t>(index)].expression = theExpression;
    return FinishEdit(snapshot, theError);
}

bool ParameterTable::SetComment(const std::string& theName, const std::string& theComment,
                                std::string& theError)
{
    const int index = IndexOf(theName);
    if (index < 0) {
        theError = "there is no parameter named " + Quoted(theName);
        return false;
    }
    myParameters[static_cast<std::size_t>(index)].comment = theComment;
    theError.clear();
    return true;
}

bool ParameterTable::SetUnit(const std::string& theName, UnitKind theKind,
                             LengthUnit theLengthUnit, AngleUnit theAngleUnit,
                             std::string& theError)
{
    const int index = IndexOf(theName);
    if (index < 0) {
        theError = "there is no parameter named " + Quoted(theName);
        return false;
    }

    const std::vector<UserParameter> snapshot = myParameters;
    UserParameter& parameter = myParameters[static_cast<std::size_t>(index)];
    parameter.kind = theKind;
    parameter.lengthUnit = theLengthUnit;
    parameter.angleUnit = theAngleUnit;
    return FinishEdit(snapshot, theError);
}

bool ParameterTable::Rename(const std::string& theOldName, const std::string& theNewName,
                            std::vector<std::string>& theRewritten, std::string& theError)
{
    theRewritten.clear();

    const int index = IndexOf(theOldName);
    if (index < 0) {
        theError = "there is no parameter named " + Quoted(theOldName);
        return false;
    }
    if (theOldName == theNewName) {
        theError.clear();
        return true;
    }
    if (!IsNameAvailable(theNewName, index, theError)) {
        return false;
    }

    const std::vector<UserParameter> snapshot = myParameters;

    myParameters[static_cast<std::size_t>(index)].name = theNewName;

    // Rewriting the expressions is what makes a rename safe: Fusion does
    // the same, and the alternative is a model that breaks because a
    // parameter got a better name.
    for (UserParameter& parameter : myParameters) {
        const std::string rewritten =
            RenameExpressionVariable(parameter.expression, theOldName, theNewName);
        if (rewritten != parameter.expression) {
            parameter.expression = rewritten;
            theRewritten.push_back(parameter.name);
        }
    }

    // A rename can still close a loop: a row referring to a name nothing
    // defined yet starts referring to this one the moment it takes that
    // name.
    if (!FinishEdit(snapshot, theError)) {
        theRewritten.clear();
        return false;
    }
    return true;
}

bool ParameterTable::Remove(const std::string& theName, std::vector<std::string>& theStale,
                            std::string& theError)
{
    theStale.clear();

    const int index = IndexOf(theName);
    if (index < 0) {
        theError = "there is no parameter named " + Quoted(theName);
        return false;
    }

    std::vector<std::string> wasValid;
    for (const UserParameter& parameter : myParameters) {
        if (parameter.isValid && parameter.name != theName) {
            wasValid.push_back(parameter.name);
        }
    }

    myParameters.erase(myParameters.begin() + index);

    // No snapshot dance: taking edges away cannot create a loop. What it
    // can do is leave rows pointing at a name that is gone, and those are
    // meant to stay in the table saying so -- refusing the delete instead
    // would mean a model could never be taken apart.
    Resolve();

    for (const std::string& name : wasValid) {
        const UserParameter* parameter = Find(name);
        if (parameter != nullptr && !parameter->isValid) {
            theStale.push_back(name);
        }
    }

    theError.clear();
    return true;
}

void ParameterTable::Clear()
{
    myParameters.clear();
}

} // namespace lcad
