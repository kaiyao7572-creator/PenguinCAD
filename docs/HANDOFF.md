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

**Added 2026-09-20/23** — the modelling-breadth gap is largely closed:

| Command | File | Proof it is correct |
|---|---|---|
| `solid.sweep` | `SweepFeature.*`, `SweepCommands.cpp` | a circle swept along a circle is a torus: **3553.06mm³** vs closed-form 2π²Rr² = 3553.06 |
| `solid.loft` | `LoftFeature.*`, `LoftCommands.cpp` | 29 headless assertions; Closed/Ruled both exercised |
| `modify.combine` | `CombineFeature.*`, `CombineCommands.cpp` | 60-check harness: volumes, keep-tools, multi-tool, undo |
| `solid.pattern.rectangular` / `.circular` / `solid.mirror` | `PatternFeatures.*` | 100 checks asserting **positions as well as volumes** |
| `export.step` / `export.obj` | `src/io/` | STEP round-trips at 6000mm³ and 3 bodies stay 3; OBJ indices 1-based, in range |
| `gizmo.rotate` / `gizmo.scale` | `src/gizmos/RotateScaleGizmo*` | built, registered, **never driven by hand — see 3.9** |
| `gizmo.press_pull` | `GizmoCommands.cpp` | arrow-blue pixels: 0 after a face click, 388 after invoking it |

**Named parameters** (`src/core/Expression.*`, `ParameterTable.*`) — a
degrees-based expression evaluator (`sin(30)` is 0.5, asserted) with
unit-aware literals reusing `Units.h`, plus an ordered parameter table
with **iterative** cycle detection that names the parameters in the loop.
`tests/parameters_test.cpp` passes. **The engine is NOT wired into
`Document` or any feature yet** — nothing but the test includes it. That
wiring is job #1 next session (§7.1).

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

### 3.6 No marking menu

Fusion's right-click radial menu is one of its most distinctive
interactions. Right-click currently does nothing.

### 3.7 Modelling breadth — mostly closed 2026-09-23

**Now present:** Sweep, Loft, Combine, Rectangular/Circular Pattern,
Mirror, Scale (uniform), STEP + OBJ export. See the table in §2.

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

### 3.9 Never verified by anyone

Written, compiles, **never driven by hand**: view cube clicking, Measure
Distance, Section View, `gizmo.rotate`, `gizmo.scale`, and the
`RotateScaleGizmoTool` drag path. Several agents died before testing
these. Treat them as unknown, not working.

The gizmo drag → `TransformFeature` path is **no longer** in this list:
it had a real bug (below) and now has `tests/transform_test.cpp` behind
it. But that test drives the FEATURE, not the mouse — the actual drag
gesture is still unproven.

**A bug that was in this list and is now fixed, as a warning about the
rest of it:** `TransformFeature` grew per-body targeting, but all three
call sites constructed it *without* a target, so `myTarget.IsNull()` was
true and every drag transformed the whole upstream shape. Invisible while
a document held one body — and Combine, patterns and mirror now make a
second body routine. Everything else in this section is the same kind of
risk: plausible-looking code nobody has actually operated.

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

---

## 6. Session log — 2026-09-20 → 2026-09-23

### What was asked

Close the gaps in `docs/FUSION360_COMPARISON.md` using many parallel
agents, so linuxCAD matches Fusion except where the difference is the
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
git status --porcelain          # expect ~38 new/modified, nothing committed
cmake --build build -j$(nproc)  # must be green before you touch anything
./tests/run_tests.sh            # expect "All test suites passed."
```

Read `docs/ARCHITECTURE.md` fully, then §1 of this file. If the build is
NOT green, stop and fix that first — everything below assumes a green
baseline.

**Decide with the user before writing code:** 38 files are uncommitted.
Offer to commit, either as one commit or split by subsystem (features /
io / gizmos / core-parameters / tests / docs). Do not commit unasked.

### 7.1 Wire the parameter engine in — highest value

`src/core/Expression.*` and `src/core/ParameterTable.*` exist, are
tested, and **nothing uses them**. Until they are wired in, the app is
still only "parametric-looking": you cannot write
`hole_dia = plate_width / 4` and have it propagate.

Concretely:

1. Give `Document` a `ParameterTable` member, plus accessors and a
   `NotifyChanged()` on edit. `Document.h/.cpp` are shared files — you
   own them now, no agents are running.
2. Let a feature store an **expression string** alongside its resolved
   double. Start with `ExtrudeFeature::myDistance` as the pilot: add
   `myDistanceExpr`, resolve it in `Compute()` through the document's
   table, fall back to the literal when the string is empty.
3. Resolve the table **before** the feature loop in `Document::Rebuild()`,
   and surface a cycle or a bad name as a rebuild error, not a crash.
4. Extend the properties panel so a numeric row accepts text. `Parameter`
   already has a `String` type — prefer reusing it over inventing a
   `Type::Expression`.
5. Add a MODIFY > Change Parameters dialog listing name / expression /
   value / unit / comment, matching Fusion's columns.

**Test it end to end:** a document where an extrude's distance is
`plate_t * 2`, then edit `plate_t` and assert the solid's volume changes.
That single assertion is the whole point of the feature.

### 7.2 Fix `select.faces` not restricting picking

Confirmed by hand: with the Faces filter on, a click still returned
`Edge   Length 20 mm` in the status bar. The filter commands
(`SelectionFilterCommand` in `src/features/FeatureCommands.cpp`, applied
by `MainWindow::applySelectionFilters`) look **additive** where Fusion's
are **exclusive**.

Reproduce:

```
arm 1200 accept
run solid.box
wait 1200
run select.faces
wait 400
click 620 250
wait 900
shot sel.png
```

Then read the status bar strip of `sel.png` (crop the bottom ~58px). It
should say `Face   Area 400 mm²`. `(620, 250)` is a known-good face hit
on a default box; `(535, 288)` lands on an edge — useful as the negative
control.

### 7.3 Resolve the two shortcut collisions

`F` → `modify.fillet` **and** `view.fit_all`. `M` → `gizmo.move` **and**
`inspect.measure_distance`. Qt fires neither on an ambiguous binding, so
both are broken. Fusion's own bindings are F = Fillet and M = Move, so
the other two should move. **Ask the user which keys they want** before
rebinding — it is muscle memory, not a technical call.

Detect them again with:

```bash
python3 - <<'EOF'
import re,os,collections
ids={}
for root,_,fs in os.walk('src'):
    for f in fs:
        if not f.endswith('.cpp'): continue
        t=open(os.path.join(root,f),errors='ignore').read()
        for m in re.finditer(r'std::string Id\(\) const override\s*\{\s*return "([^"]+)";', t):
            tail=t[m.end():m.end()+2500]
            sm=re.search(r'std::string Shortcut\(\) const override\s*\{\s*return "([^"]+)";', tail)
            if sm: ids[m.group(1)]=sm.group(1)
d=collections.defaultdict(list)
for cid,sc in ids.items(): d[sc].append(cid)
for sc,cs in sorted(d.items()):
    if len(cs)>1: print(f"{sc!r} -> {', '.join(sorted(cs))}")
EOF
```

### 7.4 Drive the never-verified UI by hand (§3.9)

`gizmo.rotate`, `gizmo.scale`, the `RotateScaleGizmoTool` drag, view cube
clicking, Measure Distance, Section View. All compile; none has been
operated. Expect real bugs — the `TransformFeature` multi-body bug came
out of exactly this category.

For the rotate/scale gizmos specifically, assert the things a compile
cannot: rotating about a centroid leaves the centroid **fixed**, a
uniform scale of 2 multiplies volume by exactly **8**, and scale 0 or
negative is **refused**.

### 7.5 Reconcile where export lives

`export.step` and `export.obj` landed in a new **Utilities** ribbon tab;
STL export is in the **File** menu. Users expect all three together.
Fusion puts export under File. Moving them means touching
`MainWindow::buildMenus` — a shared file, fine now that no agents run.

### 7.6 Worth doing, lower urgency

- **De-duplicate the body resolver.** `ResolveBodyRef` plus its constants
  (`kSameSizeRatio` 0.02, `kSameCentre` 1.0mm, `kAmbiguityMargin` 4.0)
  is file-local in **both** `CombineFeature.cpp` and
  `PatternFeatures.cpp`. They have **not** drifted (verified), but two
  copies of an identity rule will. Lifting it needs `CombineBodyRef` to
  move out of `CombineFeature.h` — `PatternFeatures`, `TransformFeature`
  and `MoveGizmoTool` all include it for that type. Better: write a test
  asserting both resolvers agree on the same input, so drift is caught
  rather than hoped against.
- **Construction geometry is unreachable.** Nothing in `src/` draws a
  construction plane — the only `new AIS_Shape` sites are bodies, sketch
  display, the press/pull gizmo and the picker's origin rectangles. The
  features exist and resolve each other by name, but a user cannot see or
  pick one.
- **Emoji icons.** `Command::Icon()` returns one emoji. It is the app's
  weakest first impression and the fix touches every command file, so do
  it in one deliberate pass, not incidentally.
- **CI.** `tests/run_tests.sh` is green and nothing runs it automatically.
  The screenshot harness is still manual.

### 7.7 The two big ones, when there is room

**Assemblies / components** and **2D drawings**. Both are document-model
projects, not features (§3.7). Each deserves its own session and its own
plan. Do not start either as a side-quest.

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
