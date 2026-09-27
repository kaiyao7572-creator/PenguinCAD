#include "core/Expression.h"

#include "core/Units.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace lcad {

namespace {

// How deep parentheses and calls may nest. The parser descends once per
// level and an expression is user input, so a pasted wall of brackets
// would otherwise run the C stack out instead of reporting anything.
// Nothing a person writes by hand comes near this.
constexpr int kMaxDepth = 128;

// The pole of tan, as far as a double can tell. cos(90 degrees) lands on
// 6.1e-17 rather than zero, so tan(90) comes back as 1.6e16: a number,
// finite, and completely meaningless as a dimension. Below this the angle
// IS the pole and saying so beats handing 1.6e16 to the geometry kernel.
constexpr double kTanPole = 1.0e-12;

unsigned char Byte(char theChar)
{
    return static_cast<unsigned char>(theChar);
}

std::string Lowered(const std::string& theText)
{
    std::string out = theText;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// ---------------------------------------------------------------------
// Degrees in, degrees out. THIS IS THE UNIT BOUNDARY OF THE LANGUAGE.
//
// Fusion's expression functions take and return degrees, this document
// stores angles in degrees (core/Units.h), and <cmath> works in radians.
// Every conversion in this file is one of these two calls, routed through
// Units.h so the app has a single definition of what a degree is.
//
// Getting this backwards does not fail loudly -- it quietly builds a
// model off by a factor of 57. tests/parameters_test.cpp pins it down
// with sin(30) == 0.5.
// ---------------------------------------------------------------------
double RadiansOf(double theDegrees)
{
    return FromDegrees(theDegrees, AngleUnit::Radian);
}

double DegreesOf(double theRadians)
{
    return ToDegrees(theRadians, AngleUnit::Radian);
}

// A value that reached the geometry kernel as NaN or infinity would show
// up much later as an empty shape nobody can explain, so every arithmetic
// step is checked where it happens and named in the message.
bool Finite(double theValue, const std::string& theWhat, std::string& theError)
{
    if (std::isnan(theValue)) {
        theError = theWhat + " is not a number";
        return false;
    }
    if (std::isinf(theValue)) {
        theError = theWhat + " overflowed";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------
// Tokens
// ---------------------------------------------------------------------

enum class TokenKind
{
    Number,
    Name,
    Plus,
    Minus,
    Star,
    Slash,
    LParen,
    RParen,
    Comma,
    End
};

struct Token
{
    TokenKind   kind = TokenKind::End;
    double      value = 0.0;   // Number: already in internal units
    bool        hasUnit = false;   // Number: the literal carried a unit suffix
    bool        isAngle = false;   // Number with a unit: an angle, not a length
    std::string text;          // Name, or a literal's source text
    std::size_t start = 0;     // byte range in the source, so a rename can
    std::size_t end = 0;       // put the new name back exactly where the old was
};

bool IsNameStart(unsigned char theChar)
{
    return std::isalpha(theChar) != 0 || theChar == '_';
}

bool IsNameChar(unsigned char theChar)
{
    return std::isalnum(theChar) != 0 || theChar == '_';
}

// What may follow a number and still be part of it: letters for "mm" and
// "deg", the prime and double prime people type for feet and inches, and
// the two bytes of UTF-8 U+00B0 DEGREE SIGN, all of which Units.h reads.
bool IsUnitChar(unsigned char theChar)
{
    return std::isalpha(theChar) != 0 || theChar == '\'' || theChar == '"'
           || theChar == 0xC2 || theChar == 0xB0;
}

bool IsSpace(char theChar)
{
    return theChar == ' ' || theChar == '\t';
}

// Where a unit word written after a SPACE ends ("40 mm", "1/2 in"), or npos
// when what follows thePos is not whitespace and then a unit this document
// knows. "40 mm" is how Fusion writes every value it shows, and how the
// numeric fields already read, so an expression refusing it was refusing
// the most ordinary thing a user types. Only a KNOWN unit joins across the
// space: a number followed by a name was never valid, so "2 x" stays the
// syntax error it always was and nothing that used to parse changes.
std::size_t SpacedUnitEnd(const std::string& theText, std::size_t thePos)
{
    const std::size_t n = theText.size();
    std::size_t scan = thePos;
    while (scan < n && IsSpace(theText[scan])) {
        ++scan;
    }
    if (scan == thePos) {
        return std::string::npos;
    }
    std::size_t end = scan;
    while (end < n && IsUnitChar(Byte(theText[end]))) {
        ++end;
    }
    if (end == scan || (end < n && IsNameChar(Byte(theText[end])))) {
        return std::string::npos;
    }
    const std::string word = theText.substr(scan, end - scan);
    LengthUnit lengthUnit = LengthUnit::Millimeter;
    AngleUnit  angleUnit = AngleUnit::Degree;
    if (!LengthUnitFromText(word, lengthUnit) && !AngleUnitFromText(word, angleUnit)) {
        return std::string::npos;
    }
    return end;
}

std::string Describe(const Token& theToken)
{
    switch (theToken.kind) {
        case TokenKind::Number: return "the number \"" + theToken.text + "\"";
        case TokenKind::Name:   return "\"" + theToken.text + "\"";
        case TokenKind::Plus:   return "\"+\"";
        case TokenKind::Minus:  return "\"-\"";
        case TokenKind::Star:   return "\"*\"";
        case TokenKind::Slash:  return "\"/\"";
        case TokenKind::LParen: return "\"(\"";
        case TokenKind::RParen: return "\")\"";
        case TokenKind::Comma:  return "\",\"";
        case TokenKind::End:    return "the end of the expression";
    }
    return "that";
}

// Read one numeric literal, unit suffix and all, and hand the whole thing
// to core/Units.h. The app must have exactly one parser that knows what
// "1/2in" is worth, and this is not it -- this only decides where the
// literal ENDS, which is a question about the expression grammar.
bool TakeNumber(const std::string& theText, std::size_t& thePos, Token& theToken,
                std::string& theError)
{
    const std::size_t n = theText.size();
    const std::size_t start = thePos;

    std::size_t digits = 0;
    std::size_t dots = 0;
    while (thePos < n && (std::isdigit(Byte(theText[thePos])) != 0 || theText[thePos] == '.')) {
        if (theText[thePos] == '.') {
            ++dots;
        } else {
            ++digits;
        }
        ++thePos;
    }
    if (digits == 0 || dots > 1) {
        // "1.2.3" and a lone "." both slide through strtod as something
        // plausible. They are typos, and a typo that evaluates is the
        // failure this whole file exists to avoid.
        theError = "\"" + theText.substr(start, thePos - start) + "\" is not a number";
        return false;
    }

    // A fraction belongs to the literal only when a unit follows it:
    // "1/2in" is half an inch the way every imperial drawing writes it,
    // while a bare "1/2" is ordinary division -- and division and a
    // fraction come to the same number, so nothing is lost by leaving it
    // to the parser. The unit is what makes them differ, because it
    // multiplies the whole fraction rather than the denominator alone.
    if (thePos < n && theText[thePos] == '/') {
        std::size_t scan = thePos + 1;
        while (scan < n && std::isdigit(Byte(theText[scan])) != 0) {
            ++scan;
        }
        if (scan > thePos + 1
            && ((scan < n && IsUnitChar(Byte(theText[scan])))
                || SpacedUnitEnd(theText, scan) != std::string::npos)) {
            // Units.h reads "1/0in" as a contented inch -- it skips a zero
            // denominator -- so the evaluator has to catch it here, the one
            // place a fraction is handed over whole instead of divided.
            if (theText.find_first_not_of('0', thePos + 1) >= scan) {
                theError = "division by zero in \"" + theText.substr(start, scan - start) + "\"";
                return false;
            }
            thePos = scan;
        }
    }

    std::size_t suffixStart = thePos;
    while (thePos < n && IsUnitChar(Byte(theText[thePos]))) {
        ++thePos;
    }
    if (thePos == suffixStart) {
        const std::size_t spacedEnd = SpacedUnitEnd(theText, thePos);
        if (spacedEnd != std::string::npos) {
            while (IsSpace(theText[suffixStart])) {
                ++suffixStart;
            }
            thePos = spacedEnd;
        }
    }
    const std::string suffix = theText.substr(suffixStart, thePos - suffixStart);

    // Feet and inches written together ("1'6\"") is one measurement, and
    // the only compound form read that way without ambiguity. Anything
    // else glued to a unit ("10mm2") is a typo; lexing it as two literals
    // makes it a syntax error instead of a quietly different number.
    LengthUnit firstUnit = LengthUnit::Millimeter;
    if (!suffix.empty() && LengthUnitFromText(suffix, firstUnit)
        && firstUnit == LengthUnit::Foot) {
        std::size_t inches = thePos;
        while (inches < n && IsSpace(theText[inches])) {
            ++inches;
        }
        if (inches < n && std::isdigit(Byte(theText[inches])) != 0) {
            std::size_t digitsEnd = inches;
            std::size_t inchDots = 0;
            while (digitsEnd < n
                   && (std::isdigit(Byte(theText[digitsEnd])) != 0 || theText[digitsEnd] == '.')) {
                inchDots += theText[digitsEnd] == '.' ? 1 : 0;
                ++digitsEnd;
            }
            // The same typo rule as any other number: strtod would read
            // "3.5.1" as 3.5 and drop the rest without a word.
            if (inchDots > 1) {
                theError = "\"" + theText.substr(inches, digitsEnd - inches) + "\" is not a number";
                return false;
            }
            std::size_t unitEnd = digitsEnd;
            while (unitEnd < n && IsUnitChar(Byte(theText[unitEnd]))) {
                ++unitEnd;
            }
            // Glued ("1'6") the inches may go unmarked, as they always
            // could. Across a space ("1' 6\"") they must say so, or
            // "2' 3" would swallow a number that was meant to stand alone.
            if (inches == thePos || unitEnd > digitsEnd) {
                thePos = unitEnd;
            }
        }
    }

    const std::string literal = theText.substr(start, thePos - start);

    ParsedValue parsed;
    if (suffix.empty()) {
        // Unitless, deliberately: the 4 in "plate_width / 4" is a count,
        // and scaling it by whatever unit a field happens to show would
        // produce geometry 25.4x wrong. Applying a unit to a bare number
        // is the caller's job -- see ParameterTable.
        parsed = ParseValue(literal, UnitKind::Unitless);
    } else {
        AngleUnit angleUnit = AngleUnit::Degree;
        parsed = AngleUnitFromText(suffix, angleUnit)
                     ? ParseValue(literal, UnitKind::Angle)
                     : ParseValue(literal, UnitKind::Length, LengthUnit::Millimeter);
    }

    if (!parsed.ok) {
        if (!suffix.empty() && (suffix[0] == 'e' || suffix[0] == 'E')) {
            // "1e3" reads as a number to almost everyone, and Units.h does
            // not take it, so say which one of us is wrong rather than
            // blaming a unit called "e".
            theError = "\"" + literal + "\": write 1000 rather than 1e3";
        } else if (suffix.empty()) {
            theError = "\"" + literal + "\" is not a number";
        } else {
            theError = "\"" + suffix + "\" is not a unit this document knows, in \"" + literal
                       + "\"";
        }
        return false;
    }

    theToken.kind = TokenKind::Number;
    theToken.value = parsed.value;
    theToken.hasUnit = !suffix.empty();
    {
        AngleUnit angleUnit = AngleUnit::Degree;
        theToken.isAngle = theToken.hasUnit && AngleUnitFromText(suffix, angleUnit);
    }
    theToken.text = literal;
    theToken.start = start;
    theToken.end = thePos;
    return true;
}

// Split theText into tokens. On failure the tokens read so far are kept:
// ExpressionVariables and RenameExpressionVariable still want the names
// out of the part that made sense.
bool Tokenize(const std::string& theText, std::vector<Token>& theTokens, std::string& theError)
{
    const std::size_t n = theText.size();
    std::size_t pos = 0;

    while (pos < n) {
        const char c = theText[pos];
        if (std::isspace(Byte(c)) != 0) {
            ++pos;
            continue;
        }

        Token token;
        token.start = pos;

        if (std::isdigit(Byte(c)) != 0
            || (c == '.' && pos + 1 < n && std::isdigit(Byte(theText[pos + 1])) != 0)) {
            if (!TakeNumber(theText, pos, token, theError)) {
                return false;
            }
            theTokens.push_back(token);
            continue;
        }

        if (IsNameStart(Byte(c))) {
            std::size_t scan = pos;
            while (scan < n && IsNameChar(Byte(theText[scan]))) {
                ++scan;
            }
            token.kind = TokenKind::Name;
            token.text = theText.substr(pos, scan - pos);
            token.end = scan;
            pos = scan;
            theTokens.push_back(token);
            continue;
        }

        switch (c) {
            case '+': token.kind = TokenKind::Plus;   break;
            case '-': token.kind = TokenKind::Minus;  break;
            case '*': token.kind = TokenKind::Star;   break;
            case '/': token.kind = TokenKind::Slash;  break;
            case '(': token.kind = TokenKind::LParen; break;
            case ')': token.kind = TokenKind::RParen; break;
            case ',': token.kind = TokenKind::Comma;  break;
            default:
                theError = std::string("\"") + c + "\" means nothing in an expression";
                return false;
        }
        token.text = std::string(1, c);
        ++pos;
        token.end = pos;
        theTokens.push_back(token);
    }

    Token end;
    end.kind = TokenKind::End;
    end.start = n;
    end.end = n;
    theTokens.push_back(end);
    return true;
}

// ---------------------------------------------------------------------
// Functions
// ---------------------------------------------------------------------

struct FunctionDef
{
    const char* name;
    int         arity;
};

// Exactly the set Fusion offers, so an expression copied from a Fusion
// model means the same thing here.
const FunctionDef kFunctions[] = {
    {"sin", 1},   {"cos", 1},  {"tan", 1},   {"asin", 1},  {"acos", 1},
    {"atan", 1},  {"sqrt", 1}, {"abs", 1},   {"floor", 1}, {"ceil", 1},
    {"round", 1}, {"pow", 2},  {"min", 2},   {"max", 2},
};

const FunctionDef* FindFunction(const std::string& theLoweredName)
{
    for (const FunctionDef& def : kFunctions) {
        if (theLoweredName == def.name) {
            return &def;
        }
    }
    return nullptr;
}

// A value and what it measures, as powers of length and angle. Plain
// numbers are 0/0, and are the one kind that fits anywhere (see Combine).
struct Quantity
{
    double value = 0.0;
    int    length = 0;
    int    angle = 0;

    bool IsPlain() const { return length == 0 && angle == 0; }
};

Quantity Plain(double theValue)
{
    Quantity q;
    q.value = theValue;
    return q;
}

bool ApplyFunction(const std::string& theLoweredName, const std::vector<Quantity>& theArgs,
                   Quantity& theResult, std::string& theError)
{
    const std::string& f = theLoweredName;
    const Quantity& arg = theArgs[0];
    const double a = arg.value;
    Quantity out;

    if (f == "sin" || f == "cos" || f == "tan") {
        // An angle, or a plain number read as degrees -- never a length.
        if (arg.length != 0 || (arg.angle != 0 && arg.angle != 1)) {
            theError = f + "() takes an angle, not " + DescribeDimension(arg.length, arg.angle);
            return false;
        }
        const double radians = RadiansOf(a);
        if (f == "tan" && std::fabs(std::cos(radians)) < kTanPole) {
            theError = "tan is undefined at " + FormatValue(a, UnitKind::Angle);
            return false;
        }
        out.value = f == "sin" ? std::sin(radians) : f == "cos" ? std::cos(radians) : std::tan(radians);
    } else if (f == "asin" || f == "acos" || f == "atan") {
        if (!arg.IsPlain()) {
            theError = f + "() takes a plain number, not " + DescribeDimension(arg.length, arg.angle);
            return false;
        }
        if (f != "atan" && (a < -1.0 || a > 1.0)) {
            theError = f + " is only defined between -1 and 1";
            return false;
        }
        out.value = DegreesOf(f == "asin" ? std::asin(a) : f == "acos" ? std::acos(a) : std::atan(a));
        out.angle = 1;
    } else if (f == "sqrt") {
        if (a < 0.0) {
            theError = "sqrt of a negative number";
            return false;
        }
        if (arg.length % 2 != 0 || arg.angle % 2 != 0) {
            theError = "sqrt() of " + DescribeDimension(arg.length, arg.angle)
                       + " has no unit this document can hold";
            return false;
        }
        out.value = std::sqrt(a);
        out.length = arg.length / 2;
        out.angle = arg.angle / 2;
    } else if (f == "abs" || f == "floor" || f == "ceil" || f == "round") {
        out = arg;
        out.value = f == "abs" ? std::fabs(a) : f == "floor" ? std::floor(a)
                  : f == "ceil" ? std::ceil(a) : std::round(a);
    } else if (f == "pow") {
        const Quantity& exponent = theArgs[1];
        if (!exponent.IsPlain()) {
            theError = "pow() needs a plain number as its exponent";
            return false;
        }
        out.value = std::pow(a, exponent.value);
        if (!arg.IsPlain()) {
            const double whole = std::round(exponent.value);
            if (std::fabs(exponent.value - whole) > 1.0e-9) {
                theError = "pow() of " + DescribeDimension(arg.length, arg.angle)
                           + " needs a whole-number exponent";
                return false;
            }
            out.length = arg.length * static_cast<int>(whole);
            out.angle = arg.angle * static_cast<int>(whole);
        }
    } else if (f == "min" || f == "max") {
        const Quantity& other = theArgs[1];
        if (!arg.IsPlain() && !other.IsPlain()
            && (arg.length != other.length || arg.angle != other.angle)) {
            theError = f + "() of " + DescribeDimension(arg.length, arg.angle) + " and "
                       + DescribeDimension(other.length, other.angle);
            return false;
        }
        out = arg.IsPlain() ? other : arg;
        out.value = f == "min" ? std::min(a, other.value) : std::max(a, other.value);
    } else {
        theError = "\"" + f + "\" is not a function";
        return false;
    }

    // pow(-8, 0.5) and pow(0, -1) are both perfectly ordinary typos that
    // come back as NaN and infinity respectively.
    theResult = out;
    return Finite(theResult.value, f + "()", theError);
}

// ---------------------------------------------------------------------
// Recursive descent
//
//   sum     := product (('+' | '-') product)*
//   product := unary (('*' | '/') unary)*
//   unary   := ('+' | '-') unary | primary
//   primary := number | name | name '(' sum (',' sum)* ')' | '(' sum ')'
// ---------------------------------------------------------------------

class Evaluator
{
public:
    Evaluator(const std::vector<Token>& theTokens, const TypedVariableLookup& theLookup)
        : myTokens(theTokens),
          myLookup(theLookup)
    {
    }

    bool Run(Quantity& theValue)
    {
        if (!Sum(theValue, 0)) {
            return false;
        }
        if (Peek().kind != TokenKind::End) {
            return Fail(Describe(Peek()) + " is not expected here");
        }
        return true;
    }

    const std::string& Error() const { return myError; }

private:
    const Token& Peek() const { return myTokens[myPos]; }

    bool Fail(const std::string& theMessage)
    {
        if (myError.empty()) {
            myError = theMessage;
        }
        return false;
    }

    // The unit rule for + and -: the same unit on both sides, except that
    // a plain number takes the unit of what it is added to -- "plate_t + 2"
    // is plate_t + 2 mm, as anyone typing it means.
    bool MatchUnits(Quantity& theLeft, const Quantity& theRight, bool theAdd)
    {
        if (theLeft.IsPlain()) {
            theLeft.length = theRight.length;
            theLeft.angle = theRight.angle;
            return true;
        }
        if (theRight.IsPlain()
            || (theLeft.length == theRight.length && theLeft.angle == theRight.angle)) {
            return true;
        }
        return Fail(std::string(theAdd ? "cannot add " : "cannot subtract ")
                    + DescribeDimension(theRight.length, theRight.angle)
                    + (theAdd ? " to " : " from ")
                    + DescribeDimension(theLeft.length, theLeft.angle));
    }

    bool Sum(Quantity& theValue, int theDepth)
    {
        if (!Product(theValue, theDepth)) {
            return false;
        }
        while (Peek().kind == TokenKind::Plus || Peek().kind == TokenKind::Minus) {
            const bool add = Peek().kind == TokenKind::Plus;
            ++myPos;
            Quantity rhs;
            if (!Product(rhs, theDepth)) {
                return false;
            }
            if (!MatchUnits(theValue, rhs, add)) {
                return false;
            }
            theValue.value = add ? theValue.value + rhs.value : theValue.value - rhs.value;
            if (!Finite(theValue.value, add ? "addition" : "subtraction", myError)) {
                return false;
            }
        }
        return true;
    }

    bool Product(Quantity& theValue, int theDepth)
    {
        if (!Unary(theValue, theDepth)) {
            return false;
        }
        while (Peek().kind == TokenKind::Star || Peek().kind == TokenKind::Slash) {
            const bool multiply = Peek().kind == TokenKind::Star;
            ++myPos;
            Quantity rhs;
            if (!Unary(rhs, theDepth)) {
                return false;
            }
            if (!multiply && rhs.value == 0.0) {
                // Caught here rather than by the finite check below, so
                // the message says what the user actually did.
                return Fail("division by zero");
            }
            theValue.value = multiply ? theValue.value * rhs.value : theValue.value / rhs.value;
            theValue.length += multiply ? rhs.length : -rhs.length;
            theValue.angle += multiply ? rhs.angle : -rhs.angle;
            if (!Finite(theValue.value, multiply ? "multiplication" : "division", myError)) {
                return false;
            }
        }
        return true;
    }

    bool Unary(Quantity& theValue, int theDepth)
    {
        // Tested here as well as in Primary: a run of leading signs
        // recurses through Unary without ever reaching Primary, so the
        // depth guard would be counted but never read, and the C stack
        // runs out instead of the error this guard exists to report.
        if (theDepth >= kMaxDepth) {
            return Fail("the expression is nested too deeply");
        }

        if (Peek().kind == TokenKind::Minus) {
            ++myPos;
            if (!Unary(theValue, theDepth + 1)) {
                return false;
            }
            theValue.value = -theValue.value;
            return true;
        }
        if (Peek().kind == TokenKind::Plus) {
            ++myPos;
            return Unary(theValue, theDepth + 1);
        }
        return Primary(theValue, theDepth);
    }

    bool Primary(Quantity& theValue, int theDepth)
    {
        if (theDepth >= kMaxDepth) {
            return Fail("the expression is nested too deeply");
        }

        const Token& token = Peek();

        if (token.kind == TokenKind::Number) {
            theValue = Plain(token.value);
            if (token.hasUnit) {
                theValue.length = token.isAngle ? 0 : 1;
                theValue.angle = token.isAngle ? 1 : 0;
            }
            ++myPos;
            return true;
        }

        if (token.kind == TokenKind::LParen) {
            ++myPos;
            if (!Sum(theValue, theDepth + 1)) {
                return false;
            }
            if (Peek().kind != TokenKind::RParen) {
                return Fail("expected \")\" but found " + Describe(Peek()));
            }
            ++myPos;
            return true;
        }

        if (token.kind == TokenKind::Name) {
            return Named(token, theValue, theDepth);
        }

        return Fail("expected a value but found " + Describe(token));
    }

    bool Named(const Token& theToken, Quantity& theValue, int theDepth)
    {
        const std::string name = theToken.text;
        const std::string lowered = Lowered(name);
        ++myPos;

        const bool called = Peek().kind == TokenKind::LParen;
        const FunctionDef* function = FindFunction(lowered);

        if (lowered == "pi") {
            if (called) {
                return Fail("PI is a constant, not a function");
            }
            // 180 degrees in radians IS pi, and taking it that way means
            // PI and the trig functions cannot disagree about its value.
            theValue = Plain(RadiansOf(180.0));
            return true;
        }

        if (function != nullptr && !called) {
            return Fail("\"" + name + "\" is a function; write " + lowered + "(...)");
        }

        if (!called) {
            if (!myLookup) {
                return Fail("there is no parameter named \"" + name + "\"");
            }
            double value = 0.0;
            UnitKind kind = UnitKind::Unitless;
            if (!myLookup(name, value, kind)) {
                return Fail("there is no parameter named \"" + name + "\"");
            }
            if (!Finite(value, "\"" + name + "\"", myError)) {
                return false;
            }
            theValue = Plain(value);
            theValue.length = kind == UnitKind::Length ? 1 : 0;
            theValue.angle = kind == UnitKind::Angle ? 1 : 0;
            return true;
        }

        if (function == nullptr) {
            return Fail("\"" + name + "\" is not a function");
        }

        ++myPos;   // past '('
        std::vector<Quantity> args;
        if (Peek().kind != TokenKind::RParen) {
            for (;;) {
                Quantity arg;
                if (!Sum(arg, theDepth + 1)) {
                    return false;
                }
                args.push_back(arg);
                if (Peek().kind != TokenKind::Comma) {
                    break;
                }
                ++myPos;
            }
        }
        if (Peek().kind != TokenKind::RParen) {
            return Fail("expected \")\" to close " + lowered + "(), but found "
                        + Describe(Peek()));
        }
        ++myPos;

        if (static_cast<int>(args.size()) != function->arity) {
            return Fail(lowered + "() takes " + std::to_string(function->arity)
                        + (function->arity == 1 ? " argument, not " : " arguments, not ")
                        + std::to_string(args.size()));
        }

        return ApplyFunction(lowered, args, theValue, myError);
    }

    const std::vector<Token>&  myTokens;
    const TypedVariableLookup& myLookup;
    std::size_t                myPos = 0;
    std::string                myError;
};

// True when the name at theIndex is being called rather than read.
bool IsCall(const std::vector<Token>& theTokens, std::size_t theIndex)
{
    return theIndex + 1 < theTokens.size() && theTokens[theIndex + 1].kind == TokenKind::LParen;
}

} // namespace

ExpressionResult EvaluateTypedExpression(const std::string&         theText,
                                         const TypedVariableLookup& theLookup)
{
    ExpressionResult result;

    std::vector<Token> tokens;
    if (!Tokenize(theText, tokens, result.error)) {
        return result;
    }
    if (tokens.size() == 1) {   // nothing but the End token
        result.error = "the expression is empty";
        return result;
    }

    Evaluator evaluator(tokens, theLookup);
    Quantity value;
    if (!evaluator.Run(value)) {
        result.error = evaluator.Error();
        if (result.error.empty()) {
            result.error = "the expression could not be read";
        }
        return result;
    }

    if (!Finite(value.value, "the expression", result.error)) {
        return result;
    }

    result.ok = true;
    result.value = value.value;
    result.lengthPower = value.length;
    result.anglePower = value.angle;
    return result;
}

ExpressionResult EvaluateExpression(const std::string& theText, const VariableLookup& theLookup)
{
    // Names from an untyped lookup are plain numbers, which fit anywhere,
    // so a caller that never cared about units sees exactly what it did.
    TypedVariableLookup typed;
    if (theLookup) {
        typed = [&theLookup](const std::string& theName, double& theValue, UnitKind& theKind) {
            theKind = UnitKind::Unitless;
            return theLookup(theName, theValue);
        };
    }
    return EvaluateTypedExpression(theText, typed);
}

std::string DescribeDimension(int theLengthPower, int theAnglePower)
{
    if (theAnglePower == 0) {
        switch (theLengthPower) {
            case 0: return "a plain number";
            case 1: return "a length";
            case 2: return "an area";
            case 3: return "a volume";
            default: break;
        }
    } else if (theLengthPower == 0 && theAnglePower == 1) {
        return "an angle";
    }
    std::string unit;
    if (theLengthPower != 0) {
        unit += "mm^" + std::to_string(theLengthPower);
    }
    if (theAnglePower != 0) {
        unit += (unit.empty() ? "" : " ") + std::string("deg^") + std::to_string(theAnglePower);
    }
    return "a quantity in " + unit;
}

bool FitsKind(const ExpressionResult& theResult, UnitKind theKind)
{
    if (theResult.lengthPower == 0 && theResult.anglePower == 0) {
        return true;
    }
    switch (theKind) {
        case UnitKind::Length:
            return theResult.lengthPower == 1 && theResult.anglePower == 0;
        case UnitKind::Angle:
            return theResult.lengthPower == 0 && theResult.anglePower == 1;
        case UnitKind::Unitless:
            return false;
    }
    return false;
}

std::vector<std::string> ExpressionVariables(const std::string& theText)
{
    std::vector<Token> tokens;
    std::string        ignored;
    Tokenize(theText, tokens, ignored);   // best effort: a half-read expression still
                                          // names the parameters it did mention

    std::vector<std::string> names;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i].kind != TokenKind::Name || IsCall(tokens, i)
            || IsReservedExpressionName(tokens[i].text)) {
            continue;
        }
        if (std::find(names.begin(), names.end(), tokens[i].text) == names.end()) {
            names.push_back(tokens[i].text);
        }
    }
    return names;
}

bool IsLiteralOfWrongKind(const std::string& theText, UnitKind theKind, std::string& theError)
{
    if (ParseValue(theText, theKind).ok) {
        return false;
    }
    const ParsedValue asLength = ParseValue(theText, UnitKind::Length);
    const ParsedValue asAngle = ParseValue(theText, UnitKind::Angle);
    const char* what = nullptr;
    if (theKind != UnitKind::Length && asLength.ok && asLength.hasExplicitUnit) {
        what = "a length";
    } else if (theKind != UnitKind::Angle && asAngle.ok && asAngle.hasExplicitUnit) {
        what = "an angle";
    }
    if (what == nullptr) {
        return false;
    }
    const char* wanted = theKind == UnitKind::Angle    ? "an angle"
                         : theKind == UnitKind::Length ? "a length"
                                                       : "a plain number";
    theError = "\"" + theText + "\" is " + what + ", and this needs " + wanted;
    return true;
}

bool IsUnitlessExpression(const std::string& theText)
{
    std::vector<Token> tokens;
    std::string        error;
    if (!Tokenize(theText, tokens, error)) {
        // An expression that cannot be read will not be evaluated either.
        // Saying no here means the caller never scales something it did
        // not understand.
        return false;
    }

    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i].kind == TokenKind::Number && tokens[i].hasUnit) {
            return false;
        }
        // sin() and PI carry no dimension; a parameter does, and its
        // value is already in internal units.
        if (tokens[i].kind == TokenKind::Name && !IsReservedExpressionName(tokens[i].text)) {
            return false;
        }
    }
    return true;
}

std::string RenameExpressionVariable(const std::string& theText, const std::string& theOldName,
                                     const std::string& theNewName)
{
    if (theOldName.empty() || theOldName == theNewName) {
        return theText;
    }

    std::vector<Token> tokens;
    std::string        ignored;
    Tokenize(theText, tokens, ignored);

    std::string out;
    std::size_t copied = 0;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i].kind != TokenKind::Name || tokens[i].text != theOldName
            || IsCall(tokens, i)) {
            continue;
        }
        out.append(theText, copied, tokens[i].start - copied);
        out += theNewName;
        copied = tokens[i].end;
    }
    out.append(theText, copied, std::string::npos);
    return out;
}

bool IsExpressionIdentifier(const std::string& theText)
{
    if (theText.empty() || !IsNameStart(Byte(theText[0]))) {
        return false;
    }
    for (char c : theText) {
        if (!IsNameChar(Byte(c))) {
            return false;
        }
    }
    return true;
}

bool IsReservedExpressionName(const std::string& theName)
{
    const std::string lowered = Lowered(theName);
    if (lowered == "pi") {
        return true;
    }
    return FindFunction(lowered) != nullptr;
}

const std::vector<std::string>& ReservedExpressionNames()
{
    static const std::vector<std::string> names = {
        "PI",   "abs",   "acos",  "asin", "atan", "ceil", "cos", "floor",
        "max",  "min",   "pow",   "round", "sin", "sqrt", "tan"};
    return names;
}

} // namespace lcad
