# The .pcad file format

A `.pcad` file is a PenguinCAD **design**: the timeline, the sketches, the
user parameters and the names things are known by. It is the recipe, not the
result. STEP carries the geometry a design evaluates to; this carries what
makes it parametric, so a design reopens and keeps being edited the way it was.

The code is `src/io/NativeFormat.{h,cpp}` (no widgets, no viewer);
`tests/saveopen_test.cpp` round-trips every feature type through it.

## In one paragraph

UTF-8 JSON, indented, written by Qt's `QJsonDocument`. The top level says what
the file is and which version wrote it, then lists the user parameters and the
features in timeline order. Each feature is its **stable type name**, every
parameter it reflects through `Parameters()`, its expressions and d# model
names, and an `extra` block for whatever the reflection does not carry
exactly. Opening builds each feature through the same constructors and setters
the app's own commands use and then rebuilds the whole timeline once: **nothing
the model evaluated to is stored or trusted**, except an imported shape, which
has no recipe to rerun.

## Top level

```json
{
    "bodies": { "list": [ { "name": "Plate", "visible": true } ], "next": 3 },
    "features": [ ... ],
    "format": "penguincad",
    "parameters": [
        { "comment": "overall width", "expression": "40 mm", "name": "plate_w", "unit": "mm" },
        { "comment": "", "expression": "plate_w / 8", "name": "plate_t", "unit": "mm" }
    ],
    "timeline": { "rollbackIndex": null },
    "units": "mm",
    "version": 1
}
```

Qt writes object keys in alphabetical order, so `format` and `version` are not
first; nothing reads the file by position.

| key | what it is |
|---|---|
| `format` | always `"penguincad"`. Anything else is not a design. |
| `version` | a whole number, `1` today. A reader refuses a larger one with "saved by a newer version of PenguinCAD", before looking at anything else. |
| `units` | the design's default length unit (View > Units): `mm`, `cm`, `m`, `in` or `ft`. What a bare number typed into a field means. |
| `parameters` | the user parameters, in Change Parameters order: `name`, `expression` exactly as typed, `unit` (`mm` `cm` `m` `in` `ft` for a length, `deg` `rad` for an angle, `""` for a plain number) and `comment`. Values are not stored; they are evaluated. |
| `features` | the timeline, first feature first. See below. |
| `timeline` | `rollbackIndex`: where the timeline marker sits, as the index of the first feature it rolls back, or `null` for "at the end". Suppression is per feature. |
| `bodies` | `list`: each body's `name` and `visible` (its light bulb), in the order the rebuild produces them; `next`: where body numbering carries on from, so a name freed by a deletion is never handed out again. The names are applied only when the rebuild produces exactly as many bodies as the list has; a design that rebuilds differently keeps fresh names rather than attach a name to the wrong body. |
| `view` | optional camera: `eye`, `centre`, `up` (each `[x, y, z]`), `scale`, `projection` (`"orthographic"` or `"perspective"`) and `fieldOfView` in degrees. This version writes none -- opening a design fits the view -- but reads one. |

Not stored: undo history (opening starts a fresh one), the selection, the
window layout, and anything a rebuild computes.

## A feature

```json
{
    "expressions": { "Distance": "plate_t" },
    "extra": {
        "profiles": [ { "boundary": [1, 2, 3, 4, 5], "seed": [10, 10] } ]
    },
    "modelNames": { "Distance": "thickness" },
    "name": "Extrude1",
    "parameters": {
        "Distance": 5,
        "Operation": "New Body",
        "Profiles": "1,2,3,4,5@10,10",
        "Reversed": false,
        "Sketch": "Sketch1",
        "Symmetric": false
    },
    "suppressed": false,
    "type": "Extrude"
}
```

| key | what it is |
|---|---|
| `type` | the stable type name, from the table below. Never a C++ class name. |
| `name` | the timeline name. Features refer to each other by it (`"Sketch": "Sketch1"`). |
| `suppressed` | `true` for a suppressed feature. |
| `parameters` | every row the feature reflects, by row name: a number, a whole number, `true`/`false` or a string. Numbers are in internal units -- millimetres, and degrees for any row whose unit is `deg`. A row driven by an expression holds the number it last evaluated to; the expression wins on reopen. |
| `expressions` | parameter name to expression text, for the driven ones. |
| `modelNames` | parameter name to its model parameter name (`d3`, or whatever it was renamed to in Change Parameters). Kept even for a parameter that no longer exists, so a retired name stays retired. |
| `extra` | per type, below. `{}` when the reflection carries everything. |

Some reflected string rows spell a pick as text rounded for the properties
panel (`"Profiles"`, `"Target"`, `"Bodies"`...). They are written, to be read
and diffed, but on opening the exact pick comes from `extra` and those rows are
not applied.

String rows that choose from a list (`"Operation": "New Body"`, a revolve's
`"Axis": "World Z"`, a construction plane's `"Type"`) hold the label the
properties panel shows. Those labels are part of the format: renaming one
means accepting the old spelling too, or a new version number.

### Types

| `type` | class | `extra` |
|---|---|---|
| `Sketch` | `SketchFeature` | the sketch itself -- see below |
| `Extrude` | `ExtrudeFeature` | `profiles`: the regions picked, `[]` for the whole sketch |
| `Revolve` | `RevolveFeature` | `profiles` |
| `Sweep` | `SweepFeature` | `profiles` |
| `Loft` | `LoftFeature` | `sections`: `[{ "sketch": "Sketch1", "profile": {...} }]` in blend order; `profile` only when the section names one region |
| `Box`, `Cylinder`, `Sphere`, `Cone`, `Torus` | the primitives | none |
| `Fillet`, `Chamfer`, `Shell` | the modify features | none |
| `PressPull` | `PressPullFeature` | `faces`: `[{ "type": "BRepFace", "body", "point": [x,y,z], "measure" }]` -- a face by owning body, centre and area |
| `Combine` | `CombineFeature` | `target` and `tools`: body picks |
| `RectangularPattern`, `CircularPattern`, `Mirror` | the pattern features | `bodies`: body picks |
| `Move` | `TransformFeature` | `body`: a body pick; a null pick (empty name, zero volume) means the whole model |
| `ConstructionPlane`, `ConstructionAxis`, `ConstructionPoint` | the construction features | none |
| `Shape` | `ShapeFeature` | `label`: what the timeline calls it (`"Import"`); `brep`: the shape as OCCT BRep text |

A **profile pick** is `{ "boundary": [curve ids, ascending], "seed": [x, y] }`
-- the `ProfileRef` of `docs/ARCHITECTURE.md`, resolved against the sketch as it
rebuilds and failing loudly when the region is gone.

A **body pick** is `{ "name", "centre": [x, y, z], "volume" }` -- a
`CombineBodyRef`. The name is for messages; the volume and centre are what find
the body again in the feature's input shape.

### A sketch's `extra`

```json
"extra": {
    "plane": {
        "origin": [0, 0, 0], "normal": [0, 0, 1],
        "xDirection": [1, 0, 0], "yDirection": [0, 1, 0]
    },
    "entities": [
        { "id": 1, "kind": "Line", "second": [40, 0] },
        { "construction": true, "id": 6, "kind": "Line", "second": [40, 20] },
        { "endAngle": 6.283185307179586, "first": [20, 10], "id": 5, "kind": "Circle", "radius": 5 }
    ],
    "constraints": [
        { "a": { "entity": 1, "role": "End" }, "b": { "entity": 2, "role": "Start" },
          "id": 1, "type": "Coincident" },
        { "a": { "entity": 1, "role": "Start" }, "b": { "entity": 1, "role": "End" },
          "id": 10, "label": "d1", "labelPosition": [20, -6], "type": "Distance", "value": 40 }
    ],
    "nextConstraintId": 12,
    "nextDimension": 3,
    "nextEntityId": 7
}
```

- `plane` is the plane as the sketch was started on it, before the `Offset`
  row pushes it along its normal. All four vectors are stored and put back bit
  for bit: rebuilding a frame from a normal and an X direction moves it in the
  last digit, and a sketch on an angled face would then change the file every
  time it was opened and saved. They must be unit length, at right angles and
  right-handed, or the file is refused.
- `entities` are the curves with their ids (constraints and profile picks name
  curves by id). `kind` is `Line`, `Circle`, `Arc`, `Ellipse`, `Spline`,
  `ControlPointSpline`, `Conic` or `Point`. A field is written only when it
  differs from a fresh curve's, so a line has no `radius`; a missing field
  reads back as that default. Fields: `first`, `second` (`[x, y]` in sketch
  coordinates), `radius`, `minorRadius`, `rotation`, `startAngle`, `endAngle`
  (radians), `points` (a spline's `[[x, y], ...]`), `rho`, `trimFirst`,
  `trimLast`, and `construction`.
- `constraints`: `type` is one of `Coincident Horizontal Vertical Parallel
  Perpendicular Equal Tangent Midpoint Concentric Collinear Fix Symmetric
  Distance DistanceX DistanceY Radius Diameter Angle`; the operands `a`, `b`,
  `c` are `{ "entity": id, "role": "Whole" | "Start" | "End" | "Centre" }`,
  written only when used. A dimension has a `value` -- millimetres, or
  **radians** for an angle, the solver's number rather than the degrees its
  parameter row shows -- a `label` (its parameter name, `d1`...) and where
  its text sits, `labelPosition`.
- The `next...` counters are where numbering carries on from. They are stored
  rather than worked out from the ids in use because numbering never rewinds:
  a deleted curve's id, which a profile pick may still name, must not be
  handed to a new curve after a reopen.

The curves ARE the sketch's definition: the solver only ever moves them the
least it can from where they were, so there is nothing else to rebuild them
from. What is not trusted is that they satisfy their constraints. A sketch
that does is left exactly as saved; one that does not (a user parameter edited
by hand, say) is solved on the rebuild, like any edit in the app.

### An imported shape's `brep`

`BRepTools::Write` text, format version 2, **without triangulation**: the
viewer meshes a displayed shape in place, so with it the text would change the
first time the part was drawn. Imports can run to megabytes; the text of the
last few shapes written is cached, since the window writes the whole design
after every edit to tell whether it has unsaved changes.

## Reading

In order, stopping at the first problem with a message that says where
(`features[3] (Extrude1): parameters: "Distance" must be a number`):

1. The text must be JSON with `"format": "penguincad"`.
2. `version` must be 1 or more and no more than this build reads.
3. **Unknown keys are refused**, at every level. They were written by
   something that meant them, and dropping them would reopen a different
   design from the one that was saved.
4. Each feature: its `type` must be known; the object is made the way its
   command makes it; `parameters` are pushed through `SetParameter` in the
   order the feature itself lists them, repeating until nothing new applies
   (naming a profile's sketch clears its picks; a construction plane's `Type`
   decides which rows it has); then `extra`; then the rows `extra` made
   possible (a sketch's dimensions). A value the feature already holds is not
   pushed again -- re-setting a sketch dimension marks the sketch for a solve.
   A row the feature does not have, or refuses, is an error.
5. `Document::ReplaceDesign` checks what only the whole design can: user
   parameters go through `ParameterTable::Add`, so a bad name or a loop is
   refused, and no two parameters, user or model, may share a name.
6. Only then is anything replaced. The timeline is evaluated once, the body
   names applied, undo cleared. A feature that fails to rebuild is part of the
   design as saved and shows red in the timeline, as it would have before
   saving; that is not a refusal.

Any refusal leaves the open design exactly as it was, undo history included.

## Writing

- Numbers are Qt's: the shortest text that reads back to the same double.
  Nothing formats a double by hand. A number that is not finite refuses the
  save rather than write a file that cannot be read.
- The write runs under the C numeric locale, so the picks some features spell
  with `printf` (`"Body2|10.5,5,5|1000"`) are the same bytes under a German
  desktop, where `printf` would write `10,5`.
- A feature class with no row in the format refuses the save by name ("... is
  a Hole, which this version of PenguinCAD cannot save yet") rather than write
  a file that silently lacks it.
- `QSaveFile`: the new file is written beside the old one and renamed over it
  only when complete, so a crash or a full disk mid-save leaves the last good
  save.
- Save, open, save again gives the same bytes. The test holds every feature
  type to that.

## Changing the format

- **A new feature class** needs a row in `Formats()` in
  `src/io/NativeFormat.cpp`, with an `extra` writer and reader if anything it
  keeps is not in `Parameters()` exactly. `tests/saveopen_test.cpp` checks that
  every row was round-tripped; add a case for the new type there.
- **A new parameter row** on an existing feature needs nothing: it is written,
  and read back through `SetParameter`. An older file simply lacks it and the
  feature keeps its default. An older PenguinCAD reading a newer file refuses
  the row by name.
- Anything an older reader would misread rather than refuse takes a new
  `version`, and the reader keeps accepting every older one.
