#pragma once

#include "core/Document.h"
#include "core/Units.h"

#include <QByteArray>
#include <QString>

#include <string>
#include <vector>

namespace lcad {

// PenguinCAD's own file format, .pcad: the design itself -- timeline,
// sketches, parameters -- rather than the geometry it evaluates to, which
// is all STEP can carry. docs/FILE_FORMAT.md is the reference; this is the
// code that reads and writes it, with no widgets and no viewer, so the
// round trip is testable headless (tests/saveopen_test.cpp).
//
// A file is UTF-8 JSON, indented, so a design can be read and diffed. Each
// feature is stored as its stable type name, every parameter it reflects,
// its expressions and model parameter names, and a per-type "extra" block
// for what the reflection does not carry exactly: sketch curves and
// constraints, profile and body picks, an imported shape's BRep.
//
// Reading rebuilds the design through the same constructors and setters
// the app's own commands use, then evaluates it once. Nothing it computed
// is trusted except an imported shape, which has no recipe to rerun.

constexpr int kNativeFormatVersion = 1;

// "pcad" -- without the dot, the way QFileInfo::suffix() reports it.
extern const char* const kNativeFileSuffix;

// The camera a design was saved with, so it reopens looking the way it was
// left. Plain numbers rather than an OCCT camera: this layer has no viewer.
struct SavedView
{
    bool   isSet = false;
    double eye[3]    = {0.0, 0.0, 0.0};
    double centre[3] = {0.0, 0.0, 0.0};
    double up[3]     = {0.0, 0.0, 1.0};
    double scale = 1.0;           // Graphic3d_Camera::Scale
    bool   perspective = false;
    double fieldOfView = 45.0;    // degrees; perspective only
};

// What a file carries besides the model itself.
struct DesignExtras
{
    // The document's default length unit (View > Units): what a bare
    // number typed into a field means.
    LengthUnit units = LengthUnit::Millimeter;

    // Optional. Left unset, nothing about the camera is written.
    SavedView view;
};

// The design as .pcad text. False, with theError, for a design this format
// cannot represent -- a feature class it has no entry for, or a number
// that is not finite -- rather than a file that would lose part of it.
bool WriteNativeText(const Document&     theDocument,
                     const DesignExtras& theExtras,
                     QByteArray&         theText,
                     std::string&        theError);

// Parse .pcad text into a design ready for Document::ReplaceDesign, without
// touching any document. False, with a message a user can act on, for text
// that is not JSON, is not a PenguinCAD design, was written by a newer
// version, or names a feature type or parameter this build does not know.
bool ReadNativeText(const QByteArray&       theText,
                    Document::DesignState&  theDesign,
                    DesignExtras&           theExtras,
                    std::string&            theError);

// Write a file atomically: the old file is replaced only once the new one
// is completely on disk, so a crash mid-save cannot destroy it.
bool SaveNativeFile(const QString&      thePath,
                    const Document&     theDocument,
                    const DesignExtras& theExtras,
                    std::string&        theError);

// Read a file and make it theDocument's design. On any failure theDocument
// is exactly as it was. The default length unit is handed back in
// theExtras, not applied: that is a process-wide setting, and the caller
// decides when it changes.
bool OpenNativeFile(const QString& thePath,
                    Document&      theDocument,
                    DesignExtras&  theExtras,
                    std::string&   theError);

// The file type name of every feature class this format can store, in the
// order a timeline most often meets them. For the tests, which build one
// of each and check a new class was not forgotten.
std::vector<std::string> NativeFeatureTypes();

} // namespace lcad
