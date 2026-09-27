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
        "$SRC/core/Expression.cpp" "$SRC/core/ParameterTable.cpp" \
        -lQt6Core -lQt6Gui -lQt6Widgets -lQt6Test -o "$OUT/widget_test" || return 1
    QT_QPA_PLATFORM=offscreen "$OUT/widget_test"
}

# ---- pipeline: sketch -> extrude -> edit -> undo, against real OCCT ----
build_pipeline() {
    g++ -std=c++17 -fPIC -I"$SRC" -I"$OCCT_INC" $QT_INC \
        "$ROOT/tests/pipeline_test.cpp" \
        "$SRC/core/Document.cpp" "$SRC/core/ProfileProvider.cpp" "$SRC/core/ShapeFeature.cpp" \
        "$SRC/core/Expression.cpp" "$SRC/core/ParameterTable.cpp" "$SRC/core/Units.cpp" \
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
        "$SRC/core/Expression.cpp" "$SRC/core/ParameterTable.cpp" "$SRC/core/Units.cpp" \
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
        "$SRC/core/Expression.cpp" "$SRC/core/ParameterTable.cpp" \
        "$SRC/core/GeometryRef.cpp" "$SRC/core/GeometrySelection.cpp" \
        "$SRC/core/Units.cpp" \
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
        "$SRC/core/Expression.cpp" "$SRC/core/ParameterTable.cpp" "$SRC/core/Units.cpp" \
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

# ---- press/pull gizmo: where the drag arrow sits and how far a drag went ----
build_gizmo() {
    g++ -std=c++17 -fPIC -I"$SRC" -I"$OCCT_INC" $QT_INC \
        "$ROOT/tests/gizmo_test.cpp" \
        "$SRC/core/Document.cpp" "$SRC/core/Entity.cpp" "$SRC/core/Body.cpp" \
        "$SRC/core/Expression.cpp" "$SRC/core/ParameterTable.cpp" \
        "$SRC/core/GeometryRef.cpp" "$SRC/core/GeometrySelection.cpp" \
        "$SRC/core/Units.cpp" \
        "$SRC/core/ProfileProvider.cpp" \
        "$SRC/features/PressPullFeature.cpp" "$SRC/features/PrimitiveFeatures.cpp" \
        "$SRC/features/FeatureUtils.cpp" "$SRC/gizmos/PressPullGizmo.cpp" \
        -L/usr/lib64 $OCCT_LIBS -lQt6Core -o "$OUT/gizmo_test" || return 1
    "$OUT/gizmo_test"
}

# ---- typed sketch input: the value-box state machine and its maths ----
build_sketchinput() {
    g++ -std=c++17 -fPIC -I"$SRC" -I"$OCCT_INC" $QT_INC \
        "$ROOT/tests/sketchinput_test.cpp" \
        "$SRC/core/Units.cpp" "$SRC/core/ProfileProvider.cpp" "$SRC/core/Origin.cpp" \
        "$SRC/sketch/SketchInput.cpp" "$SRC/sketch/SketchAnnotations.cpp" \
        "$SRC/sketch/SketchFeature.cpp" "$SRC/sketch/SketchEntity.cpp" \
        "$SRC/sketch/SketchGeometry.cpp" "$SRC/sketch/SketchConstraints.cpp" \
        "$SRC/sketch/SketchProfiles.cpp" \
        -L/usr/lib64 $OCCT_LIBS -lQt6Core -o "$OUT/sketchinput_test" || return 1
    "$OUT/sketchinput_test"
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

# ---- identity: references that refuse to guess when they cannot be sure ----
build_identity() {
    g++ -std=c++17 -fPIC -I"$SRC" -I"$OCCT_INC" $QT_INC \
        "$ROOT/tests/identity_test.cpp" \
        "$SRC/core/ProfileProvider.cpp" "$SRC/core/ProfileSelection.cpp" \
        "$SRC/core/Origin.cpp" "$SRC/core/Entity.cpp" "$SRC/core/Body.cpp" \
        "$SRC/core/GeometryRef.cpp" \
        "$SRC/sketch/SketchFeature.cpp" "$SRC/sketch/SketchEntity.cpp" \
        "$SRC/sketch/SketchGeometry.cpp" "$SRC/sketch/SketchConstraints.cpp" \
        "$SRC/sketch/SketchProfiles.cpp" \
        "$SRC/features/PrimitiveFeatures.cpp" "$SRC/features/FeatureUtils.cpp" \
        -L/usr/lib64 $OCCT_LIBS -lQt6Core -o "$OUT/identity_test" || return 1
    "$OUT/identity_test"
}

# ---- parameters: named values and the expressions between them ----
build_parameters() {
    g++ -std=c++17 -Wall -Wextra -I"$SRC" \
        "$ROOT/tests/parameters_test.cpp" \
        "$SRC/core/Expression.cpp" "$SRC/core/ParameterTable.cpp" "$SRC/core/Units.cpp" \
        -o "$OUT/parameters_test" || return 1
    "$OUT/parameters_test"
}

# ---- sweep: a profile driven along a path, against closed-form volumes ----
build_sweep() {
    g++ -std=c++17 -fPIC -I"$SRC" -I"$OCCT_INC" $QT_INC \
        "$ROOT/tests/sweep_test.cpp" \
        "$SRC/core/Document.cpp" "$SRC/core/ProfileProvider.cpp" "$SRC/core/Origin.cpp" \
        "$SRC/core/Expression.cpp" "$SRC/core/ParameterTable.cpp" "$SRC/core/Units.cpp" \
        "$SRC/core/Entity.cpp" "$SRC/core/Body.cpp" \
        "$SRC/sketch/SketchFeature.cpp" "$SRC/sketch/SketchEntity.cpp" \
        "$SRC/sketch/SketchGeometry.cpp" "$SRC/sketch/SketchConstraints.cpp" \
        "$SRC/sketch/SketchProfiles.cpp" \
        "$SRC/features/SweepFeature.cpp" "$SRC/features/ProfileFeatures.cpp" \
        "$SRC/features/FeatureUtils.cpp" \
        -L/usr/lib64 $OCCT_LIBS -lQt6Core -o "$OUT/sweep_test" || return 1
    "$OUT/sweep_test"
}

# ---- selection: what can be picked, and what a rebuild does to a pick ----
build_selection() {
    g++ -std=c++17 -fPIC -I"$SRC" -I"$OCCT_INC" $QT_INC \
        "$ROOT/tests/selection_test.cpp" \
        "$SRC/core/Document.cpp" "$SRC/core/Entity.cpp" "$SRC/core/Body.cpp" \
        "$SRC/core/Expression.cpp" "$SRC/core/ParameterTable.cpp" \
        "$SRC/core/GeometryRef.cpp" "$SRC/core/GeometrySelection.cpp" \
        "$SRC/core/ProfileProvider.cpp" "$SRC/core/ProfileSelection.cpp" \
        "$SRC/core/Units.cpp" \
        "$SRC/features/PrimitiveFeatures.cpp" "$SRC/features/FeatureUtils.cpp" \
        -L/usr/lib64 $OCCT_LIBS -lQt6Core -o "$OUT/selection_test" || return 1
    "$OUT/selection_test"
}

# ---- transform: does a gizmo drag move the picked body, or all of them ----
build_transform() {
    g++ -std=c++17 -fPIC -I"$SRC" -I"$OCCT_INC" $QT_INC \
        "$ROOT/tests/transform_test.cpp" \
        "$SRC/core/Document.cpp" "$SRC/core/ShapeFeature.cpp" "$SRC/core/Entity.cpp" \
        "$SRC/core/Expression.cpp" "$SRC/core/ParameterTable.cpp" \
        "$SRC/core/Body.cpp" "$SRC/core/ProfileProvider.cpp" "$SRC/core/Units.cpp" \
        "$SRC/gizmos/TransformFeature.cpp" \
        "$SRC/features/CombineFeature.cpp" "$SRC/features/FeatureUtils.cpp" \
        -L/usr/lib64 $OCCT_LIBS -lQt6Core -o "$OUT/transform_test" || return 1
    "$OUT/transform_test"
}

# ---- rotate/scale gizmos: a mouse drag, pixels in, angle or factor out ----
#
# TKService for Graphic3d_Camera: the screen these drags happen on is the
# app's own home view, stood up with no window.
build_rotscale_gizmo() {
    g++ -std=c++17 -fPIC -I"$SRC" -I"$OCCT_INC" \
        "$ROOT/tests/rotscale_gizmo_test.cpp" \
        "$SRC/gizmos/RotateScaleGizmo.cpp" "$SRC/gizmos/PressPullGizmo.cpp" \
        -L/usr/lib64 $OCCT_LIBS -lTKService -o "$OUT/rotscale_gizmo_test" || return 1
    "$OUT/rotscale_gizmo_test"
}

# ---- document parameters: named values driving features' numbers ----
build_docparams() {
    g++ -std=c++17 -fPIC -I"$SRC" -I"$OCCT_INC" $QT_INC \
        "$ROOT/tests/docparams_test.cpp" \
        "$SRC/core/Document.cpp" "$SRC/core/ProfileProvider.cpp" "$SRC/core/Origin.cpp" \
        "$SRC/core/Expression.cpp" "$SRC/core/ParameterTable.cpp" "$SRC/core/Units.cpp" \
        "$SRC/core/Entity.cpp" "$SRC/core/Body.cpp" \
        "$SRC/sketch/SketchFeature.cpp" "$SRC/sketch/SketchEntity.cpp" \
        "$SRC/sketch/SketchGeometry.cpp" "$SRC/sketch/SketchConstraints.cpp" \
        "$SRC/sketch/SketchProfiles.cpp" \
        "$SRC/features/ProfileFeatures.cpp" "$SRC/features/PrimitiveFeatures.cpp" \
        "$SRC/features/FeatureUtils.cpp" \
        -L/usr/lib64 $OCCT_LIBS -lQt6Core -o "$OUT/docparams_test" || return 1
    "$OUT/docparams_test"
}

# ---- marking menu: the ring's geometry, no window needed ----
build_markingmenu() {
    g++ -std=c++17 -Wall -Wextra -I"$SRC" \
        "$ROOT/tests/markingmenu_test.cpp" -o "$OUT/markingmenu_test" || return 1
    "$OUT/markingmenu_test"
}

# ---- marking gestures: a right click vs a flick vs a hold, no window needed ----
build_markinggesture() {
    g++ -std=c++17 -Wall -Wextra -I"$SRC" \
        "$ROOT/tests/markinggesture_test.cpp" -o "$OUT/markinggesture_test" || return 1
    "$OUT/markinggesture_test"
}

# ---- view: standard views, view cube clicks and what Fit frames ----
build_view() {
    g++ -std=c++17 -fPIC -I"$SRC" -I"$OCCT_INC" \
        "$ROOT/tests/view_test.cpp" "$SRC/view/ViewOrientation.cpp" \
        -L/usr/lib64 $OCCT_LIBS -lTKV3d -lTKService -lTKHLR -o "$OUT/view_test" || return 1
    "$OUT/view_test"
}

# ---- inspect: what Measure, Model Properties and Section View report ----
build_inspect() {
    g++ -std=c++17 -fPIC -I"$SRC" -I"$OCCT_INC" \
        "$ROOT/tests/inspect_test.cpp" \
        "$SRC/inspect/InspectGeometry.cpp" "$SRC/core/Units.cpp" \
        -L/usr/lib64 $OCCT_LIBS -o "$OUT/inspect_test" || return 1
    "$OUT/inspect_test"
}

# ---- shortcuts: asks the real command registry, so it needs the app ----
#
# Qt fires neither action on a key bound twice. A scan of the source missed
# half the commands (the ones built from tables), so this runs the app's
# own --check-shortcuts, which registers everything with no window. It
# says so loudly rather than pass quietly when the app is missing or older
# than the source it would be checking.
check_shortcuts() {
    local app="$ROOT/build/linuxcad"
    if [ ! -x "$app" ]; then
        echo "  SKIPPED: build/linuxcad does not exist -- run cmake --build build first"
        return 0
    fi
    if [ -n "$(find "$SRC" \( -name '*.cpp' -o -name '*.h' \) -newer "$app" | head -1)" ]; then
        echo "  SKIPPED: build/linuxcad is older than src/ -- rebuild, or this checks old code"
        return 0
    fi
    QT_QPA_PLATFORM=offscreen "$app" --check-shortcuts
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
run "Press/Pull: the drag gizmo" build_gizmo
run "Sketch: typed input and live dimensions" build_sketchinput
run "Identity: references that refuse to guess" build_identity
run "Parameters: named values and expressions" build_parameters
run "Sweep: profile along a path" build_sweep
run "Selection: what can be picked, and what survives a rebuild" build_selection
run "Transform: a gizmo drag moves the picked body" build_transform
run "Rotate/Scale: dragging the handles" build_rotscale_gizmo
run "Document parameters: expressions drive the model" build_docparams
run "Marking menu: which wedge the pointer is in" build_markingmenu
run "Marking menu: a right click, a flick or a hold" build_markinggesture
run "View: standard views, cube clicks and Fit" build_view
run "Inspect: measure, model properties and section" build_inspect
run "Shortcuts: no key bound to two commands" check_shortcuts

if [ "$failed" -eq 0 ]; then
    echo "All test suites passed."
else
    echo "One or more test suites FAILED."
fi
exit "$failed"
