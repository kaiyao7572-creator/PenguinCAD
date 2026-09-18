#!/usr/bin/env bash
# Builds and runs the standalone test binaries.
#
# These deliberately compile the real source files directly rather than
# linking the app, so a test can exercise model and widget code without a
# window server or the full build.
#
#   ./tests/run_tests.sh
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/src"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

OCCT_INC=/usr/include/opencascade
QT_INC="-I/usr/include/qt6 -I/usr/include/qt6/QtCore -I/usr/include/qt6/QtGui -I/usr/include/qt6/QtWidgets -I/usr/include/qt6/QtTest"
MOC=/usr/lib64/qt6/libexec/moc

OCCT_LIBS="-lTKernel -lTKMath -lTKBRep -lTKGeomBase -lTKGeomAlgo -lTKG3d -lTKG2d \
-lTKTopAlgo -lTKPrim -lTKBO -lTKBool -lTKFillet -lTKOffset -lTKMesh -lTKShHealing"

failed=0
run() {
    local name="$1"; shift
    echo "=============================================================="
    echo "  $name"
    echo "=============================================================="
    if ! "$@"; then
        echo "  >>> $name FAILED"
        failed=1
    fi
    echo
}

# ---- units: pure parsing/formatting, no Qt, no OCCT ----
build_units() {
    g++ -std=c++17 -Wall -Wextra -I"$SRC" \
        "$ROOT/tests/units_test.cpp" "$SRC/core/Units.cpp" -o "$OUT/units_test" || return 1
    "$OUT/units_test"
}

# ---- widget: the unit-aware input field, offscreen ----
build_widget() {
    "$MOC" -I"$SRC" "$SRC/widgets/UnitLineEdit.h" -o "$OUT/moc_UnitLineEdit.cpp" || return 1
    g++ -std=c++17 -fPIC -I"$SRC" $QT_INC \
        "$ROOT/tests/widget_test.cpp" "$OUT/moc_UnitLineEdit.cpp" \
        "$SRC/widgets/UnitLineEdit.cpp" "$SRC/core/Units.cpp" \
        -lQt6Core -lQt6Gui -lQt6Widgets -lQt6Test -o "$OUT/widget_test" || return 1
    QT_QPA_PLATFORM=offscreen "$OUT/widget_test"
}

# ---- pipeline: sketch -> extrude -> edit -> undo, against real OCCT ----
build_pipeline() {
    g++ -std=c++17 -fPIC -I"$SRC" -I"$OCCT_INC" $QT_INC \
        "$ROOT/tests/pipeline_test.cpp" \
        "$SRC/core/Document.cpp" "$SRC/core/ProfileProvider.cpp" "$SRC/core/ShapeFeature.cpp" \
        "$SRC/core/Body.cpp" "$SRC/core/Entity.cpp" \
        "$SRC/sketch/SketchFeature.cpp" "$SRC/sketch/SketchEntity.cpp" \
        "$SRC/sketch/SketchGeometry.cpp" "$SRC/sketch/SketchConstraints.cpp" \
        "$SRC/sketch/SketchProfiles.cpp" "$SRC/core/Origin.cpp" \
        "$SRC/features/ProfileFeatures.cpp" "$SRC/features/PrimitiveFeatures.cpp" \
        "$SRC/features/ModifyFeatures.cpp" "$SRC/features/FeatureUtils.cpp" \
        -L/usr/lib64 $OCCT_LIBS -lQt6Core -o "$OUT/pipeline_test" || return 1
    "$OUT/pipeline_test"
}

# ---- entities: the Fusion object model -- taxonomy, bodies, sub-shape refs ----
build_entities() {
    g++ -std=c++17 -fPIC -I"$SRC" -I"$OCCT_INC" $QT_INC \
        "$ROOT/tests/entity_test.cpp" \
        "$SRC/core/Document.cpp" "$SRC/core/Entity.cpp" "$SRC/core/Body.cpp" \
        "$SRC/core/GeometryRef.cpp" "$SRC/core/ProfileProvider.cpp" \
        "$SRC/features/PrimitiveFeatures.cpp" "$SRC/features/FeatureUtils.cpp" \
        -L/usr/lib64 $OCCT_LIBS -lQt6Core -o "$OUT/entity_test" || return 1
    "$OUT/entity_test"
}

# ---- press/pull: picking a face and moving it ----
build_presspull() {
    g++ -std=c++17 -fPIC -I"$SRC" -I"$OCCT_INC" $QT_INC \
        "$ROOT/tests/presspull_test.cpp" \
        "$SRC/core/Document.cpp" "$SRC/core/Entity.cpp" "$SRC/core/Body.cpp" \
        "$SRC/core/GeometryRef.cpp" "$SRC/core/GeometrySelection.cpp" \
        "$SRC/core/ProfileProvider.cpp" \
        "$SRC/features/PressPullFeature.cpp" "$SRC/features/PrimitiveFeatures.cpp" \
        "$SRC/features/FeatureUtils.cpp" \
        -L/usr/lib64 $OCCT_LIBS -lQt6Core -o "$OUT/presspull_test" || return 1
    "$OUT/presspull_test"
}

# ---- sketch types: the Fusion curve taxonomy, conics and control point splines ----
build_sketchtypes() {
    g++ -std=c++17 -fPIC -I"$SRC" -I"$OCCT_INC" $QT_INC \
        "$ROOT/tests/sketchtypes_test.cpp" \
        "$SRC/core/Entity.cpp" "$SRC/core/Origin.cpp" "$SRC/core/ProfileProvider.cpp" \
        "$SRC/sketch/SketchFeature.cpp" "$SRC/sketch/SketchEntity.cpp" \
        "$SRC/sketch/SketchGeometry.cpp" "$SRC/sketch/SketchConstraints.cpp" \
        "$SRC/sketch/SketchProfiles.cpp" \
        -L/usr/lib64 $OCCT_LIBS -lQt6Core -o "$OUT/sketchtypes_test" || return 1
    "$OUT/sketchtypes_test"
}

# ---- construction: the origin folder, planes, axes and points ----
build_construction() {
    g++ -std=c++17 -fPIC -I"$SRC" -I"$OCCT_INC" $QT_INC \
        "$ROOT/tests/construction_test.cpp" \
        "$SRC/core/Document.cpp" "$SRC/core/Entity.cpp" "$SRC/core/Body.cpp" \
        "$SRC/core/Origin.cpp" "$SRC/core/ConstructionGeometry.cpp" \
        "$SRC/features/ConstructionFeatures.cpp" "$SRC/features/PrimitiveFeatures.cpp" \
        "$SRC/features/FeatureUtils.cpp" \
        -L/usr/lib64 $OCCT_LIBS -lQt6Core -o "$OUT/construction_test" || return 1
    "$OUT/construction_test"
}

# ---- profiles: a sketch split into the regions a user can point at ----
build_profiles() {
    g++ -std=c++17 -fPIC -I"$SRC" -I"$OCCT_INC" $QT_INC \
        "$ROOT/tests/profile_test.cpp" \
        "$SRC/core/ProfileProvider.cpp" \
        "$SRC/sketch/SketchFeature.cpp" "$SRC/sketch/SketchEntity.cpp" \
        "$SRC/sketch/SketchGeometry.cpp" "$SRC/sketch/SketchConstraints.cpp" \
        "$SRC/sketch/SketchProfiles.cpp" "$SRC/core/Origin.cpp" \
        -L/usr/lib64 $OCCT_LIBS -lQt6Core -o "$OUT/profile_test" || return 1
    "$OUT/profile_test"
}

# ---- sketch inference: axis lock + snap candidates, no display needed ----
build_inference() {
    g++ -std=c++17 -fPIC -I"$SRC" -I"$OCCT_INC" $QT_INC \
        "$ROOT/tests/inference_test.cpp" \
        "$SRC/core/ProfileProvider.cpp" \
        "$SRC/sketch/SketchFeature.cpp" "$SRC/sketch/SketchEntity.cpp" \
        "$SRC/sketch/SketchGeometry.cpp" "$SRC/sketch/SketchConstraints.cpp" \
        "$SRC/sketch/SketchProfiles.cpp" "$SRC/core/Origin.cpp" \
        -L/usr/lib64 $OCCT_LIBS -lQt6Core -o "$OUT/inference_test" || return 1
    "$OUT/inference_test"
}

run "Units: parsing and formatting" build_units
run "Widget: unit-aware input field" build_widget
run "Pipeline: sketch -> extrude -> edit -> undo" build_pipeline
run "Sketch: axis inference and snapping" build_inference
run "Sketch: profile regions and references" build_profiles
run "Entities: bodies, faces and the Fusion taxonomy" build_entities
run "Construction: origin folder, planes, axes and points" build_construction
run "Sketch types: the Fusion curve taxonomy" build_sketchtypes
run "Press/Pull: face selection and face offset" build_presspull

if [ "$failed" -eq 0 ]; then
    echo "All test suites passed."
else
    echo "One or more test suites FAILED."
fi
exit "$failed"
