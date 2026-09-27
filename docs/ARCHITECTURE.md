# PenguinCAD architecture & contribution contract

A Fusion-360-style parametric CAD app: Qt6 UI, OpenCASCADE (OCCT) 7.9.3
geometry kernel. This document is the contract every subsystem builds
against. **Read it fully before writing code.**

## Ground rules for parallel work

Multiple subsystems are developed simultaneously. To avoid conflicts:

1. **Only edit files inside the directory you own.** Never edit
   `CMakeLists.txt`, `src/MainWindow.*`, `src/OcctViewport.*`, `src/main.cpp`,
   or anything in `src/core/`. Those are shared.
2. **`CMakeLists.txt` globs `src/**/*.cpp` automatically** — new files in
   your directory are picked up with no build-file edit. If you need an
   OCCT library that isn't linked yet (see the list at the bottom), say so
   in your final report; do **not** edit `CMakeLists.txt` yourself.
3. **Your registration function already exists as a stub.** Replace its
   body in place; don't create a second definition elsewhere (duplicate
   symbol at link time).
4. Verify your work compiles with the syntax-check command below. Do not
   run `cmake --build` — other subsystems are mid-flight and their
   half-written files would break your build through no fault of yours.

### Syntax check (run this on every file you write)

```
g++ -fsyntax-only -std=c++17 -fPIC -Isrc \
  -I/usr/include/opencascade \
  -I/usr/include/qt6 -I/usr/include/qt6/QtCore -I/usr/include/qt6/QtGui \
  -I/usr/include/qt6/QtWidgets -I/usr/include/qt6/QtOpenGL \
  -I/usr/include/qt6/QtOpenGLWidgets \
  src/<yourdir>/*.cpp
```

Q_OBJECT classes are fine under `-fsyntax-only` (moc runs at real build
time). Everything must pass cleanly before you report done.

## Core contracts

Everything lives in `namespace lcad`. Include paths are rooted at `src/`,
so use `#include "core/Command.h"`.

### `core/Feature.h` — one timeline entry

The document is a **linear timeline** of features, exactly like Fusion's.
Each is evaluated in order, receiving the shape produced by everything
upstream and returning the new shape.

```cpp
class Feature {
  virtual std::string TypeName() const = 0;           // "Extrude"
  virtual bool Compute(const ComputeContext& theContext,
                       const TopoDS_Shape& theInput,
                       TopoDS_Shape& theOutput,
                       std::string& theError) = 0;
  virtual std::unique_ptr<Feature> Clone() const = 0; // for undo
  virtual std::vector<Parameter> Parameters() const;  // for properties panel
  virtual bool SetParameter(const Parameter&);
  // plus Name/SetName, IsSuppressed/SetSuppressed, ResultShape(), LastError()
};
using FeaturePtr = std::shared_ptr<Feature>;
```

**Referencing other features:** store the other feature's **name**
(`std::string`), never a pointer — undo/redo clones the entire timeline,
so pointers go stale. Resolve at compute time:
```cpp
Feature* sketch = theContext.FindFeature(mySketchName);   // upstream only
```

- A feature that produces no solid (a sketch, a construction plane) just
  copies `theInput` to `theOutput` and returns true.
- `Clone()` **must** deep-copy and call `CopyBaseTo(*copy)` to carry
  name/suppressed across. Undo is snapshot-based and depends on this.
- `Parameters()` / `SetParameter()` are how the properties panel edits
  your feature without knowing its type. Implement them for anything with
  a numeric input — that's what makes a feature *parametric* rather than
  a one-shot.
- Never throw out of `Compute()`. OCCT operations throw `Standard_Failure`;
  catch it and turn it into `theError`:
  ```cpp
  try { /* OCCT work */ } catch (const Standard_Failure& e) {
      theError = e.GetMessageString() ? e.GetMessageString() : "OCCT error";
      return false;
  }
  ```

`core/ShapeFeature.h` is a ready-made feature wrapping a fixed shape
(used by STEP import); a good reference implementation.

### Parameters and expressions — you get them for free

The document owns a `ParameterTable` of **user parameters** (Fusion's
MODIFY > Change Parameters), and any `Double` parameter of any feature can
be **driven by an expression** over them (`plate_t * 2`). A feature does
nothing to support this:

- Expressions live on the `Feature` **base**, keyed by parameter name, and
  `CopyBaseTo` carries them — which every `Clone` already calls.
- `Document::Rebuild` evaluates each one and pushes the number through
  your `SetParameter` **before** your `Compute`, and only when the value
  really changed (a sketch re-solves whenever a dimension is set, so a
  no-op push every rebuild would make it drift).
- An expression that no longer evaluates, or gives a value outside your
  `minimum`/`maximum`, or that your `SetParameter` refuses, fails your
  feature on rebuild with a message naming the parameter — it never builds
  on the last number it gave.

What you must do: implement `Parameters()`/`SetParameter()` for numbers,
and give each `Double` the right `unit` — `"mm"` (the default) for a
length, `"deg"` for an angle, `""` for a count or ratio. `Parameter::Kind()`
reads it, and the unit is what makes `90 deg` and `1/2 in` mean the right
thing. **Do not store expressions yourself.**

Edits go through the document, never straight into a feature:
`Document::SetFeatureParameter(feature, parameter, error)` checks an
expression evaluates **now** (refusing it otherwise), keeps it only if it
names a parameter (`3 * 4` is just 12), makes the edit undoable and
rebuilds. The user-parameter mutators (`AddUserParameter`,
`RenameUserParameter`, …) are undoable too, and undo restores the table
together with the timeline. Rename rewrites every feature expression that
used the old name.

Any `UnitLineEdit` takes arithmetic; give it the document's table with
`SetParameterTable` and it takes expressions too. A feature dialog does
that when its command passes `ParametersOf(theContext)` to
`ShowFeatureDialog` and calls `ApplyFieldExpressions` on the new feature —
which matches each Number field to the parameter **with the same name as
its label**, so only opt in a dialog whose labels are its parameter names.

### `core/Document.h` — the model

```cpp
doc.AddFeature(featurePtr);        // appends + rebuilds + snapshots undo
doc.Features();                    // const std::vector<FeaturePtr>&
doc.Shape();                       // final TopoDS_Shape (may be null)
doc.ActiveFeature() / SetActiveFeature();
doc.SetRollbackIndex(i);           // timeline marker; features >= i skipped
doc.Undo() / Redo() / CanUndo() / CanRedo();
doc.AddObserver(this);             // implement DocumentObserver
doc.NotifyChanged();               // after editing a feature in place
```

`Rebuild()` is called automatically by every mutating method. A feature
that fails does **not** wipe the model — the upstream shape carries
forward and the error lands in `doc.Errors()`.

**Displaying the result is not your job.** `MainWindow` observes the
document and redisplays `doc.Shape()` automatically. Add your feature to
the document and the viewport updates itself.

### `core/ProfileProvider.h` — the sketch → solid seam

This is the **contract between the sketch subsystem and the solid-feature
subsystem**. Neither knows the other's concrete types.

```cpp
class ProfileProvider {
  virtual gp_Pln ProfilePlane() const = 0;
  virtual std::vector<TopoDS_Wire> ProfileWires() const = 0;  // closed wires
  virtual std::vector<TopoDS_Face> ProfileFaces() const;      // default impl provided
};
ProfileProvider* AsProfileProvider(Feature*);   // safe dynamic_cast
```

A sketch is also split into **profiles** -- the minimal closed regions the
user can point at, exactly as Fusion means the word. A rectangle drawn
around a circle is TWO regions (the ring and the disc), two overlapping
circles are three, a line across a rectangle splits it into two.

```cpp
struct ProfileRef {                 // durable name for ONE region
    std::vector<int> boundary;      // sketch entity ids, ascending
    gp_Pnt2d         seed;          // a point inside it
};
struct ProfileRegion { TopoDS_Face face; ProfileRef ref; double area; };

virtual std::vector<ProfileRegion> ProfileRegions() const;
bool FindProfile(const ProfileRef&, TopoDS_Face&) const;   // resolve a ref
int  ProfileRegionAt(const std::vector<ProfileRegion>&, const gp_Pnt2d&);
```

`ComputeProfileRegions` (`src/sketch/SketchProfiles.cpp`) builds the planar
arrangement. Two things about it are load-bearing and non-obvious:

1. A general fuse (`BOPAlgo_Builder`) runs over the edges first. It splits
   curves where they cross -- without it two overlapping circles share no
   vertex and have no lens between them -- and its history maps every
   fragment back to the entity it came from.
2. Each fragment is then fed to `BOPAlgo_BuilderFace` **twice, once in each
   orientation**. It only walks an edge in the direction it is handed, so
   with one orientation apiece it finds the cells on one side of a curve
   and silently drops the others. The unbounded cell comes back with a
   NEGATIVE signed area, which is how it is told from the real regions.

**Identity.** A `ProfileRef` resolves by its bounding entity ids; the seed
only breaks ties between regions sharing a boundary (all three regions of
two overlapping circles are bounded by the same two curves). If no region
has those bounding curves any more, `FindProfile` **fails** rather than
falling back to whatever now contains the seed. That fallback was tried and
removed: pick the ring of a rectangle-around-a-circle, delete the circle,
and the plain rectangle shares four of the ring's five curves *and*
swallows its seed -- so an extrude of the ring quietly became an extrude of
the whole rectangle. Breaking loudly costs one re-pick; guessing wrong
costs a wrong model nobody notices. **Do not loosen this** without a test
covering that case (`tests/profile_test.cpp`, section 11b).

Profiles the user has clicked live in `core/ProfileSelection.h`. It is in
core rather than with the sketch code because it is the hand-off between
two subsystems that deliberately know nothing about each other: the sketch
fills it in, `Extrude` reads it, and `src/features` never includes a sketch
header.

- **Sketch side:** your sketch feature inherits `Feature` *and*
  `ProfileProvider`, returning its closed wires and its regions.
- **Solid side:** store the sketch's name; at compute time do
  ```cpp
  Feature* f = theContext.FindFeature(mySketchName);
  ProfileProvider* profile = AsProfileProvider(f);
  if (profile == nullptr) { theError = "sketch not found"; return false; }
  auto faces = profile->ProfileFaces();
  ```
  If `ProfileFaces()` is empty, fail with a clear message ("sketch has no
  closed profile") rather than producing a degenerate solid.

  To act on chosen regions instead of the whole sketch, hold a
  `std::vector<ProfileRef>` and resolve each through `FindProfile`. An
  empty list means the whole sketch. If **any** stored ref fails to
  resolve, fail the whole compute -- building from the ones that survived
  changes the model silently, which is the failure this design exists to
  prevent. `ProfileFeature` already does all of this; inherit it.

### `core/Entity.h` — the object model

Fusion's own vocabulary, not an approximation of it. `EntityType` covers
the B-Rep chain in containment order (`BRepBody > BRepLump > BRepShell >
BRepFace > BRepLoop > BRepCoEdge > BRepEdge > BRepVertex`), the nine
`SketchCurve` subtypes, construction planes/axes/points, and the browser
folders in the order Fusion draws them. `IsSelectable()` records which
types are structural — a loop and a co-edge are real parts of the model
that Fusion never lets you pick, and saying so once stops each consumer
guessing.

`core/Body.h` is `BRepBody`: kind (solid/surface/wire), visibility,
volume, area, centroid, and the sub-entity collections. **Sub-shapes come
from an indexed map, never `TopExp_Explorer` directly** — the explorer
visits once per *use*, so a box yields 48 vertices and 24 edges where
Fusion says 8 and 12.

`Document::Bodies()` is rebuilt after every evaluation. `BodyTable`
carries names across by matching each new body to a previous one of the
same kind with a close enough size and position, exact survivors claiming
their names first. Numbering never rewinds: a recycled name would attach
old references to new geometry.

`core/GeometryRef.h` names one face, edge or vertex by what it IS —
owning body, kind, size, centre — not by an index an upstream fillet
invalidates. Resolving **refuses when two candidates match equally well**
rather than picking one. Same rule as `ProfileRef`, same reason.

`core/Origin.h` is the seven origin entities (three planes, three axes, a
point), defined once — `SketchFeature::PlaneXY()` and friends read them
back. They are deliberately **not** Features: in Fusion they are
intrinsic to a component, exist before anything is drawn, and can only be
hidden.

`core/ConstructionGeometry.h` is the seam for construction planes, axes
and points, mirroring `ProfileProvider`. Lookup is by NAME, and **origin
names always win** — a user who names a plane "XY" must not silently
redefine what every earlier sketch was drawn on.

### `core/Command.h` — a toolbar/menu tool

```cpp
class Command {
  virtual std::string Id() const = 0;       // "solid.extrude" (unique)
  virtual std::string Title() const = 0;    // "Extrude"
  virtual std::string Group() const = 0;    // ribbon tab name
  virtual std::string Description() const;  // tooltip
  virtual std::string Icon() const;         // ONE emoji, e.g. "📦"
  virtual std::string Shortcut() const;     // "E", "Ctrl+Return"
  virtual bool IsEnabled(const CommandContext&) const;
  virtual bool IsCheckable() const;         // for toggles
  virtual bool IsChecked(const CommandContext&) const;
  virtual void Execute(CommandContext&) = 0;
};
```

`CommandContext` gives you `document`, `viewport`, `parent` (for dialogs),
plus `AisContext()`, `View()` and `Redraw()` convenience accessors.

Register in your `Register*Commands(CommandRegistry&)` function:
```cpp
theRegistry.Add(std::make_unique<ExtrudeCommand>());
```
Group names become ribbon tabs automatically, in registration order. Every
`Register*Commands` is called from **one** list, `RegisterAllCommands` in
`src/Registration.cpp` — the window registers from it, and so does
`penguincad --check-shortcuts`, which fails the test suite if any key is
bound to two commands (Qt fires neither on an ambiguous shortcut). Ids
sharing their first two dotted parts (`select.priority.face`,
`select.priority.edge`) become one ribbon button with a flyout.

`IsEnabled()` should return false when the command can't run (e.g. needs a
selection or an existing body) — it's re-queried on every document change,
so buttons grey out correctly.

### `core/ViewportInteraction.h` — taking over the mouse

For tools that need to pick points in the viewport (sketch tools, gizmo
dragging, measurement):

```cpp
class MyTool : public lcad::ViewportInteraction {
  bool OnMousePress(const Graphic3d_Vec2i& thePos, Qt::MouseButton,
                    Qt::KeyboardModifiers) override;   // return true = consumed
  bool OnMouseMove(...), OnMouseRelease(...), OnMouseDoubleClick(...);
  bool OnKeyPress(int theKey, Qt::KeyboardModifiers);  // Escape should cancel
  void OnDeactivated();                                // clean up previews
};
ctx.viewport->PushInteraction(tool);   // tool takes over
ctx.viewport->PopInteraction();        // hand control back
```

Positions arrive in **device pixels**, already in OCCT's coordinate space
— pass them straight to `V3d_View::Convert`/`ConvertWithProj`. Returning
false lets normal orbit/pan/select run.

**Lifetime:** the viewport does not own your handler. Keep it alive (a
static/singleton per tool is fine) until after you pop it.

Screen → 3D point on a plane:
```cpp
Standard_Real x, y, z, vx, vy, vz;
view->ConvertWithProj(pos.x(), pos.y(), x, y, z, vx, vy, vz);
gp_Lin ray(gp_Pnt(x, y, z), gp_Dir(vx, vy, vz));
// intersect ray with your sketch plane
```

### `io/NativeFormat.h` — saving and opening a design

A design is saved as a `.pcad` file: indented JSON holding the recipe --
timeline, sketches, user parameters, names -- never the evaluated result.
**`docs/FILE_FORMAT.md` is the reference.** What it asks of a feature:

- Everything a feature needs to rebuild must be either a reflected
  `Parameter` or in its `extra` block. A reflected row is saved and read back
  through `SetParameter` with no extra work, which is one more reason to
  reflect every input.
- A **new Feature class needs a row** in `Formats()` in
  `src/io/NativeFormat.cpp`, under a stable type name. A class without one
  refuses the save by name rather than be dropped from the file; the row is
  matched on the exact class, so a subclass never passes as its base.
  `tests/saveopen_test.cpp` fails if a known type was not round-tripped --
  add yours to it (build, save, open, rebuild, compare, save again: the
  same bytes).
- Opening builds each feature the way its command does and calls
  `Document::ReplaceDesign`, which validates the whole design first, then
  swaps it in, rebuilds once and clears undo. A refused file leaves the
  open design untouched.

The window (`MainWindow`) owns the file: New / Open / Open Recent / Save /
Save As, the `name[*] — PenguinCAD` title, and the Save / Don't Save /
Cancel question. It decides "unsaved changes" by comparing the design as it
would be written now against what was last saved, so an edit made in place
(a sketch tool, a rename in the browser) needs no dirty flag of its own.

## Where things are shown

The **browser** lists what the design CONTAINS — Origin, Bodies,
Sketches, Construction — exactly as Fusion's does. The **timeline** lists
how it was made, one button per feature, and owns feature rename /
suppress / delete. Those are two different questions and Fusion answers
them in two different places; don't merge them back.

`MainWindow` displays **one AIS object per body**, not one for the whole
document, which is what makes a body hideable and individually pickable.

## Viewport selection, and four traps in it

Bodies are displayed with selection mode -1 and then **`Load()`**-ed into
the selection manager; `applySelectionFilters()` activates the modes the
filter allows. The `Load` is not optional: `Activate()` on an unloaded
object reports the mode as active while never building the selection
primitives, so the body looks armed and is completely unpickable.

`mousePressEvent` does a `MoveTo` at the press point before handing the
click on, because OCCT selects whatever the last MoveTo detected and a
press is not a move. Without it, any click that arrives without the cursor
having travelled there first selects nothing.

`SelectInViewer` treats a degenerate rectangle as a point pick. A click is
a zero-area rectangle, and `SelectRectangle` on one can never match with
window semantics.

`applySelectionFilters()` activates `GeometrySelection::PickableTypes()`,
not the filters directly. The filters are independent checkboxes — in
Fusion too — so switching Faces off leaves edges pickable. "Only faces" is
a different Fusion tool, **Selection Priority** (Body / Face / Edge): one
type becomes the only thing a click can pick until it is turned off.
`PickableTypes()` is the priority alone when one is set, else the filters.

Anything decorative must be displayed with selection mode -1 explicitly.
The two-argument `Display` overload activates the default mode, which is
how the origin axis lines ended up stealing clicks from bodies.

## Existing viewport behavior (don't re-implement)

`OcctViewport` already handles: left-click select, left-drag rubber-band
box select (left-to-right = enclose-only, right-to-left = crossing,
colored blue/green respectively), Ctrl+click multi-select, Fusion's
navigation (middle-drag pan, Shift+middle-drag orbit, cursor-anchored
scroll zoom), an XY ground grid, and red/green/blue origin axis lines.
The RIGHT button belongs to the marking menu alone: a click opens the
ring, a flick past 25 px runs the wedge it points at without the ring, a
350 ms hold opens the ring under the button and the release picks
(`ui/MarkingMenuGesture.h` classifies, tested headless). Tools that let
navigation pass through must pass middle and Shift+middle, and let a
right drag reach the viewport.

Finished sketches' profiles are pickable from the model view
(`sketch/ModelProfilePicker`, a permanent background interaction): hover
lights a region, a click fills `core/ProfileSelection`, which Extrude and
Revolve read. A body in front of the sketch plane still wins the click.

## House style

- OCCT handles: `Handle(AIS_Shape) x = new AIS_Shape(...)`, check
  `.IsNull()` before use. Never `delete` a handle.
- Member variables `myFoo` in `lcad::` core-style classes, `m_foo` in Qt
  widget classes. Match whatever the file you're in already does.
- Parameters in core classes are prefixed `theFoo`.
- Comments explain *why*, not *what*. Keep density similar to existing code.
- No new third-party dependencies without asking.

## OCCT libraries already linked

`TKernel TKMath TKBRep TKGeomBase TKGeomAlgo TKG3d TKG2d TKShHealing
TKTopAlgo TKPrim TKBO TKBool TKFillet TKOffset TKFeat TKMesh TKService
TKV3d TKOpenGl TKDESTEP TKDESTL TKXSBase TKCAF TKLCAF TKVCAF`

Note OCCT 7.9 renamed data-exchange libs (`TKSTEP` → `TKDESTEP`,
`TKStl` → `TKDESTL`). If you need another lib, report it — don't edit
the build file.

## Seeing your own work (REQUIRED — do not work blind)

This app can drive itself and photograph the result. Sketching is all
*feel*, and feel cannot be judged by reading source. Every UI or
interaction change must be verified with a screenshot before you report
it done.

### Flags

```
penguincad --screenshot out.png [--screenshot-delay 2500] [--screenshot-tab Solid]
penguincad --run-command sketch.create --screenshot out.png
penguincad --script path/to/script.txt
penguincad part.pcad [--script ...]    # open a design first
```

`--screenshot` writes two files: `out.png` (the Qt UI — ribbon, panels,
menus) and `out-viewport.png` (the 3D view, dumped by OCCT, because the
GL viewport never appears in a Qt widget grab). **The viewport file is
the one that shows your geometry.**

### Input scripts

`--script` replays synthetic mouse and key events (see `src/InputScript.h`
for the full command list):

```
run sketch.create      # invoke a command by id
arm 900 accept         # answer the next modal dialog (arm BEFORE the run
                       # that opens it -- the script parks inside the
                       # dialog's own event loop, so only a timer gets in)
wait 500
click 450 300          # viewport-LOCAL logical pixels; (450,300) is about
                       # the centre of the viewport and hits the origin planes
wait 900
run sketch.line
click 350 250
move  550 250          # hover, so the rubber band updates
shot  rubberband.png
click 550 250
key   Escape
shot  done.png
```

Beyond that list, verified this session:

```
hotkey F6                      # a real shortcut, through Qt's shortcut map
menu File > Export...          # a menu-bar item by its text
menu File > Open Recent > part.pcad   # ... and down through submenus
save /tmp/part.pcad            # File > Save As to a path, no file chooser
arm 900 click Don't Save       # (open asks about unsaved changes first)
open /tmp/part.pcad            # File > Open of a path, no file chooser
design                         # print file, saved/modified, title, units,
                               # timeline + expressions, bodies + volumes,
                               # user parameters
arm 900 type Distance = plate_t * 2   # type into a modal dialog's field
arm 900 click OK               # press a dialog button by its text
arm 900 cell plate_t / Expression = 10 mm   # edit a tree cell
arm 900 dump                   # print fields, tree rows, labels, buttons
arm 900 shot dialog.png        # photograph the open dialog
```

The desktop's file chooser (the xdg portal) is another process a script
cannot type into, which is what `save` and `open` are for. To drive the Save
As dialog itself, run with `QT_QPA_PLATFORMTHEME=generic`: Qt then shows its
own chooser, which `arm 1200 type File name: = part` and `arm 1700 click
Save` reach. A scripted run keeps File > Open Recent in a temporary settings
file, not the user's.

`key` goes straight to the GL window and **bypasses the shortcut map**;
use `hotkey` to prove a binding. Qt also ignores every shortcut while no
window of the app is active, and with nobody at the machine the window
manager often never focuses a new app — `hotkey` asks for focus and warns
when it was refused. Reading geometry back without eyes: `arm 800 dump`,
`arm 1000 accept`, `run inspect.model_properties` prints volume, area,
centre and bounding box.

Coordinates are relative to the viewport's top-left, not the window's.
The origin planes are small: clicking far from centre misses them and
silently creates nothing — check the Browser panel or the presence of the
contextual Sketch tab to confirm a sketch actually opened.

### Running it

```
DISPLAY=:0 QT_QPA_PLATFORM=xcb QT_QPA_PLATFORMTHEME=xdgdesktopportal \
  ./build/penguincad --script yourscript.txt
```

Then crop/zoom the result to inspect it:

```
magick out-viewport.png -crop 1100x700+420+150 +repage -resize 760x zoom.png
```

Give crops in **pixels**. `-crop 45%x3` is 45% wide and three PIXELS
tall, which reads as an empty status bar; the status bar of a 3000x1826
window grab is about `-crop 1400x56+0+1770`.

### Tests

```
./tests/run_tests.sh
```

Covers unit parsing, the unit-aware input widget, a
sketch -> extrude -> edit -> undo pipeline against real OCCT volumes, and a
save -> open -> save round trip of every feature type. Add to these when you
add model-level behaviour.
