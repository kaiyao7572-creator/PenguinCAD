# PenguinCAD — handoff to the next session

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

### 1.2d Qt ignores every shortcut while no window is active

The first on-screen shortcut test made four working keys look dead. With
nobody at the machine the window manager never focused the freshly
started app, and Qt consults its shortcut map only for an active window.
The script verb `key` has a second trap: it sends straight to the GL
window, which bypasses the shortcut map altogether. Use `hotkey F6` — it
asks for focus first and **warns when focus was refused**, so a failed
hotkey is never read as a dead binding.

### 1.2e More instruments that lied this session

- `magick ... -crop 45%x3` is 45% wide and **three pixels** tall. It read
  back an empty status bar that actually said `Edge  Length 20 mm`. Crop
  in pixels: the status bar of a window grab is `-crop 1400x56+0+1770`.
- `magick compare -metric AE` reported 3.2e9 changed pixels for a
  2.5M-pixel image. Sanity-check any count against the image size.
- A shortcut checker that **scanned the source** saw 56 commands; the
  registry holds 102 (tables of views, filters, priorities). It now asks
  the registry (`penguincad --check-shortcuts`). Check the thing itself,
  not a text picture of it.
- A tool call that was interrupted by the user **had already run**: the
  test block it added appeared twice when it was re-applied. After any
  interruption, grep the file before re-applying an edit.

### 1.3 Sessions die mid-task on usage limits

Repeatedly. **Commit working increments as you go.** Several agents were
killed seconds before reporting; the work survived only because it was on
disk and building. Never leave the tree unbuildable across a long step.

It happened three more times on 2026-09-24/26: whole workflows died with
every agent on "usage limit" — once with 322 tool calls done and nothing
committed. Workflow agents run in **git worktrees**
(`git worktree list` shows them under `.claude/worktrees/`); their
uncommitted work survives there, and a fresh agent told to finish it in
place picked it up. Check `git worktree list` before assuming a dead
agent left nothing, and delete a worktree only after
`git -C <tree> status` shows nothing worth keeping.

Three agents building at once (`-j$(nproc)` each) on this 15 GB machine
got a build killed by memory pressure (exit 144). Brief agents to use
`-j4`.

### 1.4 Two agents must never share a directory

Ownership is per-directory (`src/sketch/`, `src/ui/`, `src/features/`,
`src/view/`, `src/inspect/`, `src/gizmos/`). Parallel agents in one
directory produce merge chaos. Sequence them instead.

### 1.4a A commit nobody made on purpose was pushed

On 2026-09-26 `752f57c "initial Commit"` appeared on `main` -- made in
the main checkout with everything staged while agents worked in
worktrees under `.claude/worktrees/` -- and was pushed to `origin`. It
swept in half-finished work and the worktrees as gitlinks. It was not
rewritten (it is pushed); `18da453` removed the gitlinks and
`.claude/worktrees/` is now ignored. If a commit you did not make shows
up, stop and read it before building on it.

### 1.4b Your own shell can silently skip the edit

A patch was written as `grep ... && python3 - <<'PYEOF'`. The grep found
nothing, returned 1, and `&&` threw the patch away without a word. The
build then "succeeded" because it compiled the *unchanged* file, the
screenshot came back identical, and the conclusion drawn was "the fix
does not work" — when the fix had never been applied.

Fish also aborts a whole command when any glob matches nothing, so
`rm -f a*.png b*.png` deletes neither if only `b*` exists.

**Always confirm an edit landed** (`grep` for the new symbol) before you
measure whether it worked. A measurement of code you did not change is
worse than no measurement, because it looks like evidence.

### 1.4c Symmetry can masquerade as "the renderer is broken"

A box rendered as a flat orange hexagon — one uniform colour, no edge
between its faces. It looked exactly like lighting was off. It was not:
a sphere in the same scene rendered with 435 distinct shades.

`V3d_Viewer::SetDefaultLights()` installs a **headlight**, pointing
straight down the view direction. In an isometric view the three visible
faces of a cube make *equal* angles with that direction, so all three
return identical brightness. The renderer was working perfectly and
reporting a true fact about an unlucky geometry.

The fix is in `OcctNativeWindow::initializeOcctViewer()`: an off-axis
key light plus ambient. **Test a curved body before concluding anything
about shading** — a sphere distinguishes "unlit" from "symmetric" in one
shot.

### 1.5 Build gotchas

- `CMakeLists.txt` globs `src/**/*.cpp`. **Never edit it to add files.**
  If you need a new OCCT library, that is the one reason to touch it.
- `Q_OBJECT` classes must live in **headers**. In a `.cpp` they fail to
  link, confusingly.
- Phantom "undefined reference to vtable" after adding a Q_OBJECT class
  usually means a stale autogen cache:
  `rm -rf build/penguincad_autogen build/CMakeFiles/penguincad_autogen.dir && cmake -B build`
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

**Saving** (`src/io/NativeFormat.*`, `docs/FILE_FORMAT.md`) — a design
is a `.pcad` file: the timeline, sketches, user parameters and names, as
JSON, rebuilt on opening through the features' own setters. File > New
Design / Open / Open Recent / Save / Save As, with unsaved-changes prompts.

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

**Added 2026-09-20/23** — the modelling-breadth gap is largely closed:

| Command | File | Proof it is correct |
|---|---|---|
| `solid.sweep` | `SweepFeature.*`, `SweepCommands.cpp` | a circle swept along a circle is a torus: **3553.06mm³** vs closed-form 2π²Rr² = 3553.06 |
| `solid.loft` | `LoftFeature.*`, `LoftCommands.cpp` | 29 headless assertions; Closed/Ruled both exercised |
| `modify.combine` | `CombineFeature.*`, `CombineCommands.cpp` | 60-check harness: volumes, keep-tools, multi-tool, undo |
| `solid.pattern.rectangular` / `.circular` / `solid.mirror` | `PatternFeatures.*` | 100 checks asserting **positions as well as volumes** |
| STEP / OBJ export (now File > Export…) | `src/io/` | STEP round-trips at 6000mm³ and 3 bodies stay 3; OBJ indices 1-based, in range |
| `gizmo.rotate` / `gizmo.scale` | `src/gizmos/RotateScaleGizmo*` | driven on screen 2026-09-26: 45° exactly about the centroid, scale 2.0025 → volume × 2.0025³ (§3.9) |
| `gizmo.press_pull` | `GizmoCommands.cpp` | arrow-blue pixels: 0 after a face click, 388 after invoking it |

**Named parameters — wired in 2026-09-24.** The document owns a
`ParameterTable`, and **every numeric parameter of every feature**
(Double and Int) can be driven by an expression over it: an extrude's
distance, a box's sides, a sketch's `d1`, a pattern's quantity. The
contract is in `docs/ARCHITECTURE.md` ("Parameters and expressions").
Undo restores the table with the timeline; a rename rewrites the
features that read it; a feature whose expression stops evaluating —
or whose SKETCH's expression does — fails and says why rather than
building on the last number. `tests/docparams_test.cpp` has the
handoff's own example: `plate_width` → `hole_dia = plate_width / 4` →
a sketch diameter → an extrude, 785.40 → 3141.59 mm³ by hand.

Every numeric field takes arithmetic, and expressions where the document's
parameters are passed: the properties panel, the Change Parameters
dialog, and the dialogs of Box, Cylinder, Sphere, Cone, Torus, Extrude,
Revolve, Press Pull, Fillet, Chamfer, Shell, Point, Offset Plane,
Plane at Angle, Rectangular and Circular Pattern and Sweep. An expression naming a missing parameter is
held red and greys OK out; a lone literal of the wrong kind (`2 in` in
an angle field) is refused.

**MODIFY > Change Parameters** (`src/ui/ParametersDialog.*`) — Fusion's
columns (Parameter, Name, Unit, Expression, Value, Comments), User
Parameters with `+`, Model Parameters per feature, edit in place, every
edit its own undo step, Delete refused for a parameter in use. Driven on
screen end to end: `plate_t` edited 5 → 10 mm doubled an extrude from
6576.367 to 13152.734 mm³, and Edit > Undo put it back.

**Also added 2026-09-24/26**, each verified on screen as well as by tests:

| What | Where | Evidence |
|---|---|---|
| Right-click **marking menu**, Fusion's eight in Fusion's places | `src/ui/MarkingMenu*` | Repeat Box re-ran Box (8000 → 16000 mm³), Undo from the ring back to 8000, a right DRAG still orbits |
| **Selection Priority** (Body / Face / Edge) | `GeometrySelection`, `SelectionPriorityCommand` | clicking an edge under Face Priority reports `Face Area 400 mm²` |
| **File > Export…** with STEP/STL/OBJ in one dialog; Utilities tab gone | `src/io/ExportDialog.*` | three formats written; asks before overwriting the file it really writes |
| **Construction geometry is drawn** (planes, axes, points) | `MainWindow::redisplayConstruction` | offset plane at 30 mm, point at (40,0,0), axis at X=40 all appear |
| One body from an extrude of **adjoining** profiles; separate for disjoint | `FuseProfileSolids` | ring + disc → one 6-face body; two circles → two bodies |
| **One** body-pick resolver (there were three) | `FindBodyForRef` in `CombineFeature.h` | rule pinned by 9 assertions |
| Shortcut clashes fail the suite | `penguincad --check-shortcuts` | catches `F` bound twice |
| F6 = Fit All, I = Measure (F = Fillet, M = Move, as Fusion) | view/inspect commands | all four fired on screen |

**Units** (`src/core/Units.h`) — mm internally, always. Type `12.9in`,
`1/2"`, `1'6"`, `2.5cm` into any numeric field and it converts *and* the
field adopts that unit. 40 parser tests.

**UI/view/inspect** — browser, timeline, properties panels; standard
views, display modes, view cube, model properties, measure, section.

**Shading** — the viewer now uses an off-axis key light plus ambient, so
a cube reads as a cube (three faces at 200/167/87 brightness, each about
13.4k pixels, which is the equal projected area isometric demands).
Sphere and cylinder verified too. See §1.4c for why this looked like a
renderer bug and was not.

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

- ~~Extruding several adjoining profiles makes a compound of touching
  solids.~~ Fixed 2026-09-26 for Extrude, Revolve and Sweep
  (`FuseProfileSolids`).
- The Extrude dialog names its target ("Sketch1 (1 profile)") but offers
  no way to change the picked profiles from inside the dialog, and no
  manipulator arrow to drag. Fusion has both.
- Profiles are only pickable while the sketch is open. In Fusion a
  finished sketch's profiles stay pickable from the model view — finish
  the sketch, click a region, press E. **This is the most Fusion-feel
  item left in this area** and it crosses `SketchDisplay` (fills are
  drawn for the active sketch only), `SketchSelection` (an interaction
  pushed only while a sketch is open), the viewport selection in
  `MainWindow` and the Extrude command. Plan it before starting.
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

### 3.4 ~~No typed input while drawing~~ — this entry was stale

Typed input while drawing exists (commits `a236b66`, `d1cadd4`;
`tests/sketchinput_test.cpp`, 144 assertions). Not driven on screen in
the 2026-09-24/26 session.

### 3.5 Sketch feedback gaps

The numbers are now Fusion-like: the live length/angle floats beside the
cursor in filled boxes, the one taking keystrokes highlighted in blue, and
a committed dimension reads as the value alone ("25.4 mm") rather than
"d1 = 25.4 mm" -- the parameter name belongs to the properties panel.
Anything drawing text through AIS must scale the height by the device
pixel ratio; `SetHeight` is in FRAMEBUFFER pixels, and handing it a
logical number silently halves every number on a 2x display.

While a sketch is open, EVERY curve in EVERY sketch carries its own size
beside it -- a line its length, an arc its radius, a circle its diameter,
in the same R/D shorthand a placed dimension uses so the two never
disagree. They are quieter than placed dimensions on purpose: they are
there to be read, not to drive the geometry. Outside sketch mode they go
away, because over a model they would be clutter.

Outside a sketch, picking geometry reports it in the status bar the way
Fusion does: a face's area, an edge's length, a vertex's position, and
totals for several of one kind. `DescribeSelection` in
`core/GeometrySelection.h` does the wording and is tested without a
window. Note that anything else writing to the status bar on a selection
change will overwrite it -- the press/pull arrow used to, and was quieted
for exactly that reason.

Still missing:

- **No fully-constrained indication**, and it is the biggest remaining
  one. Fusion turns the geometry a different colour the moment a sketch
  has zero degrees of freedom. Doing it honestly means DOF = unknowns −
  rank(Jacobian), and the trap is that `SketchSolver`'s system only
  includes entities a constraint MENTIONS (SketchConstraints.cpp ~line
  572). A completely free line contributes no unknowns, so a naive rank
  count calls it fully constrained -- the exact opposite of the truth.
  A correct version needs a parameter vector over EVERY entity and the
  rank taken against that. Do not ship a signal that can lie; a wrong
  "fully constrained" is worse than none.
- No glyph showing WHY a point snapped (endpoint vs midpoint vs origin).
- Constraints are not clickable/deletable in the viewport.
- Sketch geometry cannot be dragged to reshape (the solver exists).
- Trim does not preview which segment will vanish.
- No Project/Include of existing edges into a sketch.

### 3.6 ~~No marking menu~~ DONE (first version)

Right-click the canvas: Repeat, Press Pull, Redo, Hole, Sketch,
Move/Copy, Undo, Delete, clockwise from the top as Autodesk's reference
lists them; Cancel and OK replace Undo and Redo while a tool runs. Hole
and Delete are greyed in their places (the commands do not exist yet).

**Gestures done 2026-09-27**, after the user chose Fusion's navigation:
middle-drag pans, Shift+middle-drag orbits, a right flick runs a wedge
without the ring, a right hold opens it and the release picks. Verified
on screen (a flick up re-ran Box, a flick left undid it; a hold then a
release on Undo took 16000 to 8000 mm³).

Left to do:
- The second-level Sketch ring (hover Sketch for Line, Rectangle...).
- The overflow menu below the ring (Fusion: Pan, Zoom, Orbit, Isolate,
  context commands for what is under the cursor).
- Hole and Delete themselves.

### 3.7 Modelling breadth — mostly closed 2026-09-23

**Now present:** Sweep, Loft, Combine, Rectangular/Circular Pattern,
Mirror, Scale (uniform), STEP + OBJ export. See the table in §2.
Construction planes/axes/points are now **drawn** but not **pickable**:
you cannot yet start a sketch on an offset plane by clicking it.

**Still missing:** Rib, Web, Emboss, Hole, Thread, Draft, Replace/Split
Face, Split Body. Extrude still lacks taper angle and "to object".
Sweep takes only a **closed** path (ProfileProvider hands over closed
wires and nothing else — widening it is a change to that seam, not to
`SweepFeature`). Loft has no rails, no centreline, no tangency
conditions, and refuses annular sections. Patterns work on **bodies
only**, on world axes/planes through the world origin — not on features,
faces, or a picked edge/construction axis.

**No components, joints or assemblies at all.** This is now the single
largest structural gap, and it is a document-model project rather than a
feature: it needs a component tree above the linear timeline, joints, and
per-occurrence transforms. Deliberately not attempted while nine agents
were editing `src/features/` in parallel.

**No 2D drawings.** Second largest. Needs a drawing-view + annotation
subsystem downstream of the model.

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

### 3.10 Parameters — what is not Fusion yet

**Done 2026-09-27:** every numeric parameter of every feature has a
document-wide model name (d1, d2 ...; a sketch's labels are renumbered
when two sketches would share one), user and model parameters resolve
together before the timeline (loops caught, "d11 -> d11"), expressions
and user parameters can read model parameters, Change Parameters shows
and renames them, and units are checked through every expression (a
length cannot drive a count or an angle; "10 mm + 5 deg" is an error).
The first two bullets below are therefore fixed and kept for the record.

- **Model parameters have no names except sketch dimensions.** Fusion
  names every feature dimension (`d1`, `d2`, ...) and lets expressions
  read them (`d3 * 2`). Here only sketch dimensions carry `d#`, and no
  expression can read a model parameter — the lookup is user parameters
  only.
- **No dimensional analysis.** A lone literal of the wrong kind is
  refused, but `n = 4 mm` used as a pattern quantity is accepted as 4,
  and `plate_t` (a length) typed into an angle field is accepted as that
  many degrees. Fusion checks units through the whole expression.
- A user parameter's **unit is fixed** once added (the dialog cell is
  read-only; `Document::SetUserParameterUnit` exists).
- Fusion's Favorites column and filter box are absent.

### 3.9 ~~Never verified by anyone~~ — driven on 2026-09-26

Three agents operated all of it on screen, fixed what was broken, and
proved it with numbers; the highest-risk claims were re-checked by hand.

- **Move / Rotate / Scale gizmos** are now a fixed size on screen (the
  Move gizmo was ~10 px across) and drawn over the body. A Move drag of
  99.85 px moved the body 32.006 mm (predicted 32.06 from 3.114 px/mm),
  and only the picked body; a ring drag turned a box exactly 45° about
  its centroid; a scale drag of 2.0025× gave volume × 2.0025³. Handles
  left on screen after Undo were fixed. `tests/rotscale_gizmo_test.cpp`
  drives the drag maths without a window.
- **View cube** clicks, Ctrl+1..6, Home and F6 land where Fusion's do,
  keep Z up, frame the MODEL (they framed the 360 mm origin axes), swing
  over 0.45 s, and there is Fusion's home icon. `tests/view_test.cpp`.
- **Measure** picks faces, edges and vertices, shows Fusion's panel, and
  reads 20.000 / 28.284 / 34.641 mm on a 20 mm box's edge, face diagonal
  and body diagonal. A face selected BEFORE pressing I becomes the first
  pick, as in Fusion (this looked like a lost click in one check).
- **Section View** cuts with a solid hatched cap (OCCT's hatch was
  see-through, so a solid looked hollow) and leaves the model untouched.

Fixed on the way, found by those agents outside their directories: the
**timeline showed no features at all** (buttons shown by a queued call
were measured as hidden), a STEP import framed the origin axes, a
**moved body was renamed** (Body1 → Body3), OCCT's grid-echo star
followed the cursor, and a script could not photograph a drag midway.

Still open from that work:
- Displays other than 2× scaling were not tried; the gizmos in
  perspective were tested without a window only.
- The Move gizmo's handles are harder to grab than Rotate/Scale's (about
  3–4 px of tolerance on rings, 8 px on arrows, against ~10 px).
- A wheel zoom during a view swing does not stop the swing.
- Clicking the view cube during Measure zooms to fit the selection.
- A scaled body, or one a press/pull grew a lot, still gets a new name.
- **The harness's starting camera is occasionally different** (about 1
  run in 8, before and after these changes alike). Check the first frame
  before trusting hard-coded click positions — the box of a default 20 mm
  `solid.box` has its centroid near logical (586, 257).
- This desktop redraws the viewport about once a second, so the 0.45 s
  swing could only be verified by sampling the camera, not watched.

---

## 4. How to verify

```bash
./tests/run_tests.sh        # units, unit widget, pipeline, sketch inference
cmake --build build -j$(nproc)
```

Visual, only when the logic is genuinely visual:

```bash
DISPLAY=:0 QT_QPA_PLATFORM=xcb QT_QPA_PLATFORMTHEME=xdgdesktopportal \
  ./build/penguincad --script yourscript.txt
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

The harness grew a lot on 2026-09-24/26 — see `src/InputScript.h` and
`docs/ARCHITECTURE.md` for the list: `hotkey`, `menu File > Export...`,
`arm <ms> dump|type|click|cell|shot` to see and drive modal dialogs from
inside, `popup dump|shot|move|click` for the marking menu, and
`drag ... right`. The single most useful one:

```
arm 800 dump
arm 1000 accept
run inspect.model_properties     # prints volume, area, centre, bounding box
```

reads the model back as numbers, which settles most "did it work"
questions without judging a screenshot.

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

---

## 6. Session log — 2026-09-24 → 2026-09-26

### What was asked

Work through the §7 brief the previous session left: commit its 38
files, wire the parameter engine in, fix `select.faces`, resolve the
shortcut collisions, drive the never-verified UI, reconcile export,
de-duplicate the resolver.

### What landed (all on `main`, each commit builds on its own)

- The previous session's work, split into seven commits by subsystem
  (the lighting fix got its own), each built in isolation first.
- **Parameters wired end to end** (§2): document table, expressions on
  every numeric parameter, undoable edits, fields and dialogs that take
  expressions, Change Parameters. The pilot the brief suggested
  (`ExtrudeFeature::myDistanceExpr`) was replaced by a generic mechanism
  on the `Feature` base, so no feature needed code for it.
- **Shortcuts:** F6 Fit All, I Measure (the user chose Fusion's keys);
  a registry-based check fails the suite on any clash.
- **Selection Priority**, **File > Export**, **marking menu**,
  **construction geometry drawn**, **adjoining profiles fused**, **one
  resolver** — see §2.

### Findings the brief had wrong, corrected out loud

- **`select.faces` was not broken.** The filters start as Faces + Edges,
  so the command toggled Faces OFF and the click found an edge. And
  Fusion's filters ARE independent checkboxes (Autodesk's help: "The
  object types that are checked are selected"). What the report wanted
  is a different Fusion tool, Selection Priority — which was added.
- **The resolver had three copies, not two** (Transform had a public
  one). And a test that copies agree was impossible — two were
  file-local — and weaker than one copy.
- **"A unit has to be joined to its number"** was an asserted rule of
  the expression engine. It refused `40 mm`, which is how Fusion writes
  every value and how this app's own fields already read. Reversed.

### Bugs found by verifying rather than trusting

- Revolve's and Plane at Angle's **angle fields were LENGTH fields**
  (`" °"` was not recognised as an angle suffix).
- Properties-panel edits were **never undoable**.
- An adversarial review (three finders, each required to reproduce) found
  **eleven** defects in this session's own parameter work, all
  reproduced, fixed and tested — every new assertion fails against the
  previous code. The worst: a field read `5 m - 1 m` as 6000 mm (Units.h
  reads it as a compound and adds), `2 in` in an angle field became 50.8
  degrees, an extrude kept building on a sketch whose parameter had
  broken, the sketch tools deleted every constraint added to such a
  sketch, and Enter in an untouched row threw away the redo history.
- The **Move gizmo** drew ~10 px across — handed to §3.9's agents.

### State at the end

See §3.9 for the UI the last workflow drove. Build green, all suites
passing (`./tests/run_tests.sh`), 102 registered commands.

## 6b. Session log — 2026-09-20 → 2026-09-23

### What was asked

Close the gaps in `docs/FUSION360_COMPARISON.md` using many parallel
agents, so PenguinCAD matches Fusion except where the difference is the
point (local vs cloud, OCCT vs ASM, free/Linux vs paid/absent).

### How it went

Four `Workflow` runs, 34 agents, ~3.5M subagent tokens. **Every one of
the four runs lost agents to usage limits** (9 killed, then 10, then 5,
then 2). What made the difference was structure, not luck: the first run
fanned out nine agents at once and banked *nothing* when the limit hit. Subsequent runs used sequential
waves of three, so each wave's work landed on disk before the next
started. Combine and Loft survived a limit that way; Pattern, Parameters
and Export survived the next one.

**Agents that died still left usable work.** `SweepFeature.{h,cpp}` was
787 complete, syntax-clean lines from an agent that died before writing
its command — the orchestrator wrote `SweepCommands.cpp` and it worked.
`tests/identity_test.cpp` was complete and passing, just never wired into
the runner. Always check the tree before assuming a dead agent produced
nothing.

### Three bugs found by verifying rather than trusting

1. **Sweep was never verified by anyone** (its agent died before
   reporting). A torus test proved it correct to 5 s.f.
2. **`Expression.cpp` SIGSEGV'd** on a long run of unary signs: depth was
   counted in `Unary` but only *tested* in `Primary`, which a sign run
   never reaches. Confirmed crash at 200k minuses; now refuses 1M cleanly.
3. **A gizmo drag moved every body**, not the picked one (§3.9).

### Two corrections made out loud

- Claimed `CombineFeature` could not resolve reversed-orientation solids,
  from diffing two copies of a resolver and seeing `std::fabs` in one but
  not the other. **Wrong** — Combine applies it one level up in
  `VolumeAndCentre`. Same constants, same behaviour. The duplication is
  real; the drift was not.
- Claimed the viewer's lights were activated before the view existed.
  **Wrong** — see §1.4c. That "fix" was reverted rather than left in.

### A brief the agents corrected

Agents were told to store body references by name and resolve via
`Document::FindBody()`. `CombineFeature`'s agent refused, and was right:
`core/GeometryRef.h` line 66 says a feature computing mid-timeline sees
its INPUT shape and there are no bodies at that point. The brief would
have produced a feature that worked once and broke on every rebuild
after. Later waves were briefed with the correction.

### State at the end

- Build green. **16 suites, 1037 assertions, all passing.**
- **57 registered commands**, 38.1k lines of `src/`, 5.4k of `tests/`.
- **Nothing is committed.** 38 files new or modified, all on `main`.

---

## 7. Next session: do this

Copy this whole section into the next session as the brief.

### 7.0 First, orient (10 minutes, do not skip)

```bash
cd /home/kaidaidk/Documents/linuxCAD
git status --porcelain          # expect clean
git worktree list               # leftover agent worktrees? read §1.3 first
cmake --build build -j$(nproc)  # must be green before you touch anything
./tests/run_tests.sh            # expect "All test suites passed."
```

Read `docs/ARCHITECTURE.md` fully, then §1 of this file.

### ~~7.1 A native save/open format~~ — done 2026-09-27

Designs save as `.pcad` (indented JSON; `docs/FILE_FORMAT.md` is the
reference, `src/io/NativeFormat.*` the code). File > New Design (Ctrl+N),
Open... (Ctrl+O), Open Recent, Save (Ctrl+S) and Save As... (Ctrl+Shift+S,
which Create Sketch gave up); `penguincad part.pcad` opens one. The title is
`name* — PenguinCAD` while there are unsaved changes, and New, Open and
closing the window ask Save / Don't Save / Cancel. STEP moved to File >
Import STEP....

`tests/saveopen_test.cpp` round-trips all 23 feature types (same volume,
body names, expressions, d# names, user parameters; save -> open -> save
byte-identical) and checks the refusals. Driven on screen with the harness:
a user parameter, a sketched rectangle, an extrude by the parameter and a
fillet, saved, New, reopened -- same timeline, parameter and volume
(3822.04 mm³) -- plus Ctrl+S, the Save As chooser, Open Recent, and each
prompt's Cancel and Don't Save.

What is left:
- No autosave or crash recovery yet; this format is what they will write.
- The camera is not saved (the format has room for it); opening fits the view.
- An edit that makes a body grow a lot renames it (§3.9's open item), and a
  saved file records the new name -- so undoing such an edit back to the
  saved state still shows unsaved changes, because the name really changed.
- A design saved by this version with a feature a later version renames or
  removes needs that version to keep reading the old type name.

### ~~7.1b, 7.2, 7.4~~ — done 2026-09-27

Finished-sketch profiles are pickable from the model view; every feature
dimension has a d# name expressions can read and Change Parameters can
rename; units are checked through every expression; Fusion's navigation
and marking-menu gestures are in (see §3.6 and §3.10, and
docs/ARCHITECTURE.md). Each verified on screen with numbers.

### 7.3 Construction geometry pickable

It is drawn now; make a construction plane something `sketch.create` can
be clicked onto, the way the origin planes are.

### 7.4 Marking menu, what is left (§3.6)

The Sketch sub-ring, the overflow menu below the ring, and Hole and
Delete themselves.

### 7.5 The loose ends §3.9 still lists

### 7.6 The two big ones, when there is room

**Assemblies / components** and **2D drawings** (§3.7). Each is a
document-model project with its own session and plan. Not side-quests.

### If you use agents again

- **Sequential waves of three.** Nine at once banked nothing when the
  limit hit; waves of three survived two separate limits.
- Give every agent a **disjoint file list**, and keep shared files
  (`CMakeLists.txt`, `MainWindow.*`, `OcctViewport.*`, `core/Registration.h`,
  `run_tests.sh`) for yourself.
- Have agents define their own `Register*Commands` and wire it yourself.
- **Demand evidence, not "it compiles."** The agents that produced real
  numbers (100 position checks, a divergence-theorem volume) produced
  correct work. Then verify the highest-risk claims yourself anyway: the
  orchestrator's own round-trip and torus tests each caught something.
