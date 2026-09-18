# linuxCAD — handoff to the next session

Read this, then `docs/ARCHITECTURE.md`, before writing any code.

The goal is a CAD app on Linux that feels **exactly like Fusion 360**, on
an OpenCASCADE kernel. The user judges this by feel, not by feature
checklists. "It builds" is not the bar; "it behaves the way Fusion does"
is.

---

## 1. Read these before you touch anything

Five hard-won lessons. Ignoring any of them will cost you hours.

### 1.1 Screenshots lied for most of this project's life

`V3d_View::Dump` writes colour channels **rotated**: what lands in the
file is `(in.B, in.R, in.G)`. This is already corrected inside
`lcad::DumpViewportImage` (`src/InputScript.cpp`) — use that, never raw
`Dump`.

This caused two confident, wrong diagnoses that were acted on:

- Sketch curves were "garish yellow-green with a magenta preview". They
  were always the intended light blue. An agent was briefed to fix a
  defect that did not exist.
- "The world is Y-up with the grid on the wrong plane", diagnosed from a
  green vertical axis. Re-measured correctly, the vertical axis is blue:
  Z was already up.

**The lesson that generalises:** a single probe colour is not enough to
pin down a colour bug — pure red looks identical under a channel
*rotation* and an R/B *swap*, and picking the wrong one silently leaves
it broken. Solve against two independent observations. More broadly:
when a measurement surprises you, suspect the instrument before you
rewrite the code.

### 1.2 The display is often unavailable

The user is frequently away with the machine asleep. **Prefer headless
verification.** `./tests/run_tests.sh` links the real sources and needs
no display. Put logic somewhere test-reachable — `SketchGeometry` is
deliberately free of UI, document and AIS for exactly this reason.

Only reach for `--script`/`--screenshot` when the thing under test is
genuinely visual, and expect it to fail harmlessly when nothing is awake.

### 1.2b Check the instrument before believing "no display"

A session concluded the machine was asleep because `xset` and `xdpyinfo`
failed. Neither is installed on this box. The X sockets were accepting
connections the whole time and the app ran fine on `:0`.

Test the display by **running the app** (`--script` with a `shot`), never
by probing with a tool you have not confirmed exists. Same lesson as 1.1
in different clothes: a surprising measurement is more often a broken
instrument than a broken world.

### 1.2c A green suite does not mean it runs

Profile regions passed 48 headless assertions while the app **segfaulted**
on the second click of drawing a rectangle — `AIS_Shape` only materialises
its shading aspect once a colour is set, and nothing without a viewer can
catch that. Drive anything touching AIS on screen before calling it done.

### 1.3 Sessions die mid-task on usage limits

Repeatedly. **Commit working increments as you go.** Several agents were
killed seconds before reporting; the work survived only because it was on
disk and building. Never leave the tree unbuildable across a long step.

### 1.4 Two agents must never share a directory

Ownership is per-directory (`src/sketch/`, `src/ui/`, `src/features/`,
`src/view/`, `src/inspect/`, `src/gizmos/`). Parallel agents in one
directory produce merge chaos. Sequence them instead.

### 1.5 Build gotchas

- `CMakeLists.txt` globs `src/**/*.cpp`. **Never edit it to add files.**
  If you need a new OCCT library, that is the one reason to touch it.
- `Q_OBJECT` classes must live in **headers**. In a `.cpp` they fail to
  link, confusingly.
- Phantom "undefined reference to vtable" after adding a Q_OBJECT class
  usually means a stale autogen cache:
  `rm -rf build/linuxcad_autogen build/CMakeFiles/linuxcad_autogen.dir && cmake -B build`
- OCCT 7.9 renamed data-exchange libs: `TKSTEP` → `TKDESTEP`,
  `TKStl` → `TKDESTL`.

---

## 2. What exists and works

Verified by tests, by driving the app, or both.

**Core** (`src/core/`) — `Document` is a linear feature timeline with
rebuild, rollback marker, snapshot undo/redo and observers. `Feature`
carries a small parameter-reflection system so the properties panel can
edit any feature without knowing its type. `CommandRegistry` builds the
ribbon; groups can be **contextual** (the Sketch tab only exists while a
sketch is open). `ProfileProvider` is the seam letting Extrude consume a
Sketch without either knowing the other's types.

**Critical invariant:** features reference each other **by name**, never
by pointer. Undo restores the timeline from *clones*, so pointers go
stale. A test asserts this.

**Sketch** (`src/sketch/`, ~10k lines) — plane picking in the viewport
(no modal), line/rect/circle/arc/polygon/ellipse/slot/spline/point,
trim/extend/offset/mirror/patterns, 12 constraint types with a solver,
dimensions, axis inference, snapping to endpoints/centres/midpoints/origin.

**Selection** (`src/core/GeometrySelection.h`) — faces, edges, vertices
and bodies are pickable in the viewport, with Fusion's SELECT filter panel
deciding which. Picks become durable `GeometryRef`s that features hold.

**Sketch tools** — line/rect/circle/arc/polygon/ellipse/slot/point plus
all three of Fusion's spline-family tools: fit point spline, control
point spline, and the conic curve (two ends and a point on it, with `[`
and `]` sliding rho between ellipse, parabola and hyperbola).

**Object model** (`src/core/Entity.h`, `Body.h`, `GeometryRef.h`,
`Origin.h`, `ConstructionGeometry.h`) — Fusion's taxonomy by its own
names: bodies with faces/edges/vertices, the Origin folder, construction
planes/axes/points, and durable references to individual sub-shapes.
Sketch curves cover all of Fusion's drawable types including conics and
control point splines.

**Profiles** (`src/sketch/SketchProfiles.*`, `src/core/Profile*`) — a
sketch splits into the minimal closed regions a user can point at, each
selectable in the viewport and referenceable from a feature in a way that
survives undo. ~60 headless assertions in `tests/profile_test.cpp`, every
area computed by hand.

**Features** (`src/features/`) — box/cylinder/sphere/cone/torus, extrude,
revolve, fillet, chamfer, shell. All parametric and re-editable. Verified
against real OCCT volumes (a 40×30×10 extrude is exactly 12000mm³).

**Units** (`src/core/Units.h`) — mm internally, always. Type `12.9in`,
`1/2"`, `1'6"`, `2.5cm` into any numeric field and it converts *and* the
field adopts that unit. 40 parser tests.

**UI/view/inspect** — browser, timeline, properties panels; standard
views, display modes, view cube, model properties, measure, section.

---

## 3. What is broken or not Fusion-like

Roughly highest value first. The user's own words: "theres like a billion
more" — treat this as a starting set, not a complete one.

### 3.1 ~~Sketch profiles are not selectable~~ DONE

Done. A sketch now splits into minimal *profiles*: hover lights the one
under the cursor, a click picks it, and Extrude/Revolve build on what was
picked. Verified on screen end to end — rectangle drawn around a circle,
click the disc, press E, and a cylinder stands inside an untouched
rectangle. The contract and the two non-obvious OCCT details it rests on
are in `docs/ARCHITECTURE.md`.

What is NOT done, in rough order of value:

- Extruding several adjoining profiles at once makes a **compound of
  touching solids** where Fusion makes one body. Fusing the prisms is
  probably ten lines in `ExtrudeFeature::Compute`.
- The Extrude dialog names its target ("Sketch1 (1 profile)") but offers
  no way to change the picked profiles from inside the dialog, and no
  manipulator arrow to drag. Fusion has both.
- Profiles are only pickable while the sketch is open. In Fusion a
  finished sketch's profiles stay pickable from the model view.
- No box selection of several profiles at once.
- Regions are recomputed on every `Refresh()`. Measured fine for
  hand-drawn sketches (~1ms; 25 overlapping circles ~20ms), but a
  pathological sketch would be felt. Measure before adding a cache — a
  cache here must be invalidated by every tool that touches geometry,
  which is a far better source of bugs than of speed.

### 3.2 ~~The document is a single body~~ DONE

Done. `Document::Bodies()` splits the evaluated shape into named bodies
that keep their names across a rebuild, `MainWindow` draws one AIS object
per body, and the browser lists them with a visibility checkbox. Faces,
edges and vertices are enumerated per body and referenceable through
`GeometryRef`. Construction geometry and the Origin folder exist too. See
`docs/ARCHITECTURE.md` for the contract.

Still missing here:

- **Per-face and per-edge SELECTION in the viewport.** The references
  exist and resolve; nothing yet lets a user click a face to make one.
  That is what Press/Pull (3.3) needs, and it is the next thing to build.
- Per-body appearance and material. Bodies have no colour of their own.
- Components and occurrences: `EntityType` names them, nothing creates
  them. No assemblies, no joints.
- Body visibility is wired but was never driven by hand — the checkbox
  path is three lines and obvious, but it is untested by anything.
- Folder light-bulbs (hide every body at once) are deliberately absent:
  they need per-entity visibility wired first, and a checkbox that does
  nothing is worse than none.

### 3.3 ~~Press/Pull is missing~~ DONE

Done, and with it face/edge selection in the viewport. Click a face, press
Q, type a distance: out adds material, in removes it. Verified on screen
as well as headless -- hover outlines the face, clicking selects it, and
the box grows.

Getting there turned up three real bugs that had nothing to do with
Press/Pull and had been there all along:

- **A plain left-click never selected anything.** `SelectInViewer` always
  called `SelectRectangle`, and a click arrives as a ZERO-AREA rectangle;
  with window semantics, which require full enclosure, it can never match.
- **OCCT selects what the last MoveTo detected, and a press is not a
  move.** A click arriving without the cursor having travelled there first
  had nothing detected. `mousePressEvent` now detects at the press point
  before handing the click on.
- **The origin axis lines were pickable.** The comment beside them said
  "never activated for selection" but the two-argument `Display` overload
  activates the default mode, so the axes -- running through the world
  origin -- stole clicks from any body built there.

Also: `Activate()` on an object displayed with selection mode -1 reports
the mode as active while never building the selection primitives. The body
looks armed and is completely unpickable. `Load()` is what actually loads
it into the selection manager, and it is not optional.

What is left here:

- No drag manipulator. Fusion's Q gives an arrow you pull; this asks for a
  distance in a dialog.
- Planar faces only. A cylindrical face should offset radially; it is
  refused with "press/pull needs a flat face" rather than doing something
  wrong.
- No multi-face press/pull. `SoleItem` refuses two faces rather than
  guessing, so the command greys out.
- Offsetting a face that meets its neighbours at an angle just sweeps and
  fuses; Fusion extends the adjacent walls instead. Fine for prismatic
  parts, wrong for a draft.

### 3.4 No typed input while drawing

In Fusion you draw a line, type `25`, press Enter, and it is exactly
25mm. Same for angles and for every dialog-free tool. Currently every
dimension is after-the-fact. Mostly headless-testable.

### 3.5 Sketch feedback gaps

- No glyph showing *why* a point snapped (endpoint vs midpoint vs origin).
- Constraints are not clickable/deletable in the viewport.
- Sketch geometry cannot be dragged to reshape (the solver exists).
- A fully-constrained sketch does not change colour, so you cannot tell.
- Trim does not preview which segment will vanish.
- No Project/Include of existing edges into a sketch.

### 3.6 No marking menu

Fusion's right-click radial menu is one of its most distinctive
interactions. Right-click currently does nothing.

### 3.7 Modelling breadth

Sweep, Loft, Rib, Web, Emboss, Hole, Thread, Draft, Scale, Combine,
Replace/Split Face, Split Body. Extrude also lacks taper angle and
"to object". No components, joints or assemblies at all.

### 3.8 Browser and timeline are half-Fusion

The browser is now Fusion-shaped: Origin / Bodies / Sketches /
Construction, in Fusion's order, each folder appearing only when it has
something in it. Feature rename/suppress/delete moved to the timeline's
right-click menu, because the browser lists what the design CONTAINS and
a feature is not that.

What is left:

- The timeline is still plain buttons rather than a compact draggable
  icon strip, and features cannot be reordered by dragging.
- No icons anywhere in the browser — Fusion leans on them heavily, and
  the Type column here is standing in for them.
- Long names elide in a narrow dock. Cosmetic and font-dependent; the
  dock is resizable. Real fix is icons plus dropping the Type column.
- No Document Settings or Named Views folders. Deliberate: both would be
  inert, and a folder that does nothing is worse than a missing one.

### 3.9 Never verified by anyone

Written, compiles, **never driven**: view cube clicking, Measure
Distance, Section View, and the gizmo drag → `TransformFeature` path.
Three agents died before testing these. Treat them as unknown, not
working.

---

## 4. How to verify

```bash
./tests/run_tests.sh        # units, unit widget, pipeline, sketch inference
cmake --build build -j$(nproc)
```

Visual, only when the logic is genuinely visual:

```bash
DISPLAY=:0 QT_QPA_PLATFORM=xcb QT_QPA_PLATFORMTHEME=xdgdesktopportal \
  ./build/linuxcad --script yourscript.txt
```

A script that reaches a drawn sketch (viewport-local logical pixels;
(450,300) is about centre and hits the origin planes):

```
run sketch.create
wait 500
click 450 300
wait 900
run sketch.line
click 350 250
move  550 250
shot  rubberband.png
click 550 250
key   Escape
shot  done.png
```

`shot` writes two files: the Qt UI, and `-viewport.png` for the 3D view
(a Qt grab cannot see into the GL window). Inspect with:

```bash
magick done-viewport.png -crop 1100x760+380+120 +repage -resize 780x z.png
```

Diagnostics that do not need eyes: the Browser panel lists features, and
the contextual **Sketch tab only exists when a sketch is open** — both
tell you whether an interaction actually did anything.

---

## 5. Working style the user expects

- **Do not walk blindly.** Verify before claiming something works. Say
  plainly when you have not verified something, and what would verify it.
- **Correct yourself out loud.** Two wrong diagnoses in this project were
  caught and retracted; that was welcomed, not penalised.
- **If Fusion does not have a control in that place, it should not be
  there.** A "Show Sketches" ribbon button was removed on exactly this
  reasoning.
- Match the house style: `myFoo` in core classes, `m_foo` in Qt widgets,
  `theFoo` parameters, OCCT `Handle()` conventions. Comments explain
  **why**, not what.
- Never let `Compute()` throw. Catch `Standard_Failure` and report it
  through `theError`.
- Commit in working increments with real commit messages.
