# linuxCAD architecture & contribution contract

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

- **Sketch side:** your sketch feature inherits `Feature` *and*
  `ProfileProvider`, returning its closed wires. First wire = outer
  boundary, the rest are treated as holes.
- **Solid side:** store the sketch's name; at compute time do
  ```cpp
  Feature* f = theContext.FindFeature(mySketchName);
  ProfileProvider* profile = AsProfileProvider(f);
  if (profile == nullptr) { theError = "sketch not found"; return false; }
  auto faces = profile->ProfileFaces();
  ```
  If `ProfileFaces()` is empty, fail with a clear message ("sketch has no
  closed profile") rather than producing a degenerate solid.

### `core/Command.h` — a toolbar/menu tool

```cpp
class Command {
  virtual std::string Id() const = 0;       // "solid.extrude" (unique)
  virtual std::string Title() const = 0;    // "Extrude"
  virtual std::string Group() const = 0;    // ribbon tab name
  virtual std::string Description() const;  // tooltip
  virtual std::string Icon() const;         // ONE emoji, e.g. "📦"
  virtual std::string Shortcut() const;     // "E", "Ctrl+Shift+S"
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
Group names become ribbon tabs automatically, in registration order.

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

## Existing viewport behavior (don't re-implement)

`OcctViewport` already handles: left-click select, left-drag rubber-band
box select (left-to-right = enclose-only, right-to-left = crossing,
colored blue/green respectively), Ctrl+click multi-select, right-drag
orbit, middle-drag pan, cursor-anchored scroll zoom, an XY ground grid,
and red/green/blue origin axis lines.

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
linuxcad --screenshot out.png [--screenshot-delay 2500] [--screenshot-tab Solid]
linuxcad --run-command sketch.create --screenshot out.png
linuxcad --script path/to/script.txt
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

Coordinates are relative to the viewport's top-left, not the window's.
The origin planes are small: clicking far from centre misses them and
silently creates nothing — check the Browser panel or the presence of the
contextual Sketch tab to confirm a sketch actually opened.

### Running it

```
DISPLAY=:0 QT_QPA_PLATFORM=xcb QT_QPA_PLATFORMTHEME=xdgdesktopportal \
  ./build/linuxcad --script yourscript.txt
```

Then crop/zoom the result to inspect it:

```
magick out-viewport.png -crop 1100x700+420+150 +repage -resize 760x zoom.png
```

### Tests

```
./tests/run_tests.sh
```

Covers unit parsing, the unit-aware input widget, and a
sketch -> extrude -> edit -> undo pipeline against real OCCT volumes. Add
to these when you add model-level behaviour.
