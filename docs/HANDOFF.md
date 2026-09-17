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

### 3.1 Sketch profiles are not selectable (the user's example)

**This is the biggest single gap.** In Fusion, closed sketch regions
become *profiles* you hover, highlight and click; Extrude then acts on
the chosen profile. Here, closed regions are shaded (good) but are not
pickable, and `ExtrudeFeature` consumes an entire sketch **by name**.

You cannot model like Fusion without this: a sketch with two closed
regions can only be extruded as a whole. Needs: profile detection into
discrete faces, selectable AIS objects per profile, a stable way to
reference one from a feature that survives edit and undo (by index is
fragile — think about it), and Extrude taking a profile rather than a
sketch.

### 3.2 The document is a single body

`MainWindow::redisplayDocument` displays `document->Shape()` as **one
`AIS_Shape`**. So there is no per-body selection, no Bodies folder that
means anything, and Move/gizmo acts on the whole model. Fusion is
body-centric throughout. This blocks Combine, per-body appearance,
per-body visibility, and assemblies.

### 3.3 Press/Pull is missing

Fusion's most-used tool (`Q`): select a face, drag it. Everything else in
Modify is downstream of it in muscle memory.

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

Per-item eye icons were started and not finished — visibility is still a
single global "show all sketches" toggle, which is not how Fusion works.
The tree is flat rather than nested under Origin/Bodies/Sketches, and the
timeline is plain buttons rather than a compact draggable icon strip.

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
