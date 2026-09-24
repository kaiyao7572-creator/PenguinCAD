# linuxCAD vs Fusion 360 — honest audit

Written 2026-09-20, re-verified against the tree on 2026-09-21 by reading the
actual code (`src/*`, `docs/ARCHITECTURE.md`, registered `Command` IDs, and
`tests/run_tests.sh` run to completion) against Fusion 360's real feature set.
Not marketing copy — this is meant to drive what gets built next, so the weak
parts are called out as weak, not softened.

### Revision note (2026-09-21)

This document drove a round of work, and that work landed. Six of the items
below were wrong or are now stale; they have been rewritten in place rather
than left standing as an accusation the code has already answered.

1. **"No boolean combine" was wrong twice over, and it was the headline
   claim.** Booleans were never missing. `src/features/FeatureUtils.h` defines
   `BooleanOp{NewBody,Join,Cut,Intersect}` and `ApplyBooleanOperation()`, and
   it has done since `baceea9` — the commit that created `src/features/` in the
   first place, and still the only commit that has ever touched that file.
   Seventeen files use it, and every primitive and profile feature has always
   carried the operation dropdown. The first audit missed this by reading the
   command list instead of the headers. What was genuinely missing was the
   narrower thing: Fusion's standalone MODIFY > COMBINE, acting on two bodies
   that already exist. That now exists too (`src/features/CombineFeature.*`,
   `"modify.combine"`). See 3.1.
2. **Sweep and loft now exist** — `SweepFeature` (`"solid.sweep"`) and
   `LoftFeature` (`"solid.loft"`), both with real limits worth knowing. See 3.1.
3. **Solid-level pattern and mirror now exist** —
   `"solid.pattern.rectangular"`, `"solid.pattern.circular"`, `"solid.mirror"`
   in `src/features/PatternFeatures.*`. See 3.1.
4. **STEP and OBJ export now exist** — `src/io/StepExport.*` and
   `src/io/ObjExport.*`, commands `"export.step"` and `"export.obj"`. See 3.4.
5. **The test-suite description was wrong.** `tests/run_tests.sh` runs
   *fourteen* suites, not three. The criticism has been re-scoped to what is
   genuinely untested, which is still plenty. See 3.5.
6. **The README has been rewritten** and is now accurate. See 3.6.

The direction of the original errors is the durable lesson: every one of them
understated the app. Auditing from `Command` IDs finds what has a button and
misses what is wired into a feature's parameters. **Read the headers.**

Everything else below still stands, and re-verification turned up two things
the first pass missed: construction geometry is unreachable (3.1) and the move
gizmo silently moves every body (3.3). The largest gap is unchanged — there is
still no way to save a design.

---

## 1. Similarities (the parts that are genuinely good)

These aren't superficial — the object model *quotes* Fusion's, on purpose:

- **Linear feature timeline.** Document = ordered list of features, each
  rebuilt from the shape upstream of it. Same mental model as Fusion's
  timeline, including rollback and suppress.
- **Origin is geometry, not a feature.** Three planes, three axes, one point,
  present before anything is drawn, can be hidden but never deleted or
  reordered — matches Fusion's "intrinsic to a component" behavior exactly.
- **Profiles as regions, not whole sketches.** A rectangle around a circle
  correctly resolves to two pickable regions (ring + disc); extrude targets a
  *region*, the same granularity Fusion gives you. This is a nontrivial
  feature (general fuse + double-orientation face building) and it's done
  right, including refusing to silently guess when a region's identity is
  ambiguous after an edit — a trap Fusion itself is careful about.
- **Fusion's own B-Rep vocabulary**, not an approximation of it — down to the
  "8 vertices / 12 edges on a box" counting convention, which most hand-rolled
  OCCT wrappers get wrong (naive `TopExp_Explorer` gives you 24/48).
- **References that refuse to guess, consistently.** `ProfileRef`,
  `GeometryRef` and `CombineBodyRef` are three different problems with one
  answer: carry a geometric signature, resolve against the shape actually in
  hand, and fail loudly on no match *or* on a tie. `CombineFeature.h` spells
  out why the obvious alternative is a trap — a feature computing mid-timeline
  sees only its input shape, and the body table is derived from the *finished*
  document, so resolving a body by name would work exactly once.
- **Press/Pull** implemented as its own gizmo and tool, matching Fusion's
  signature direct-manipulation gesture rather than just being extrude with
  extra steps.
- **Browser vs. Timeline kept as two separate questions** ("what does the
  design contain" vs. "how was it built"), same split Fusion makes.
- **Visual polish chasing Fusion specifically** — recent commits target
  Fusion's exact dimension-text rendering and constraint-glyph coloring, not
  "a" CAD look.

**Bottom line on this section:** the parts that exist are architecturally
sound and not a toy imitation. The problem is almost entirely *how much
doesn't exist yet* and *how untested the existing parts are outside the
happy path* — see below.

---

## 2. Differences (structural, not fixable by "adding a feature")

- **Kernel:** OpenCASCADE (open, LGPL) vs. Autodesk Shape Manager (closed).
  OCCT is respectable but is known to be slower and less robust on
  degenerate/dirty geometry than ASM — this will bite on real-world imported
  STEP files eventually, and there is no fallback/healing strategy visible
  beyond `TKShHealing` being linked.
- **Delivery:** one native binary vs. Autodesk's cloud-hybrid SaaS. This is a
  real advantage for linuxCAD (offline, no account, no telemetry) but also
  means linuxCAD gets none of what the cloud buys Fusion — cross-device
  continuity, server-side compute for render/sim/generative, team
  collaboration.
- **Cost / platform:** free and Linux-native vs. paid and Linux-absent. Also
  a genuine advantage, and probably the actual reason to keep building this
  instead of just running Fusion in a VM.
- **Scope:** one document, one body-centric model, no assemblies at all vs.
  a full suite (CAM, sim, render, generative, PCB). This isn't a gap to be
  offended by — no solo/small project should expect to match a 15-year,
  hundreds-of-engineers product on breadth. The comparison is useful for
  prioritization, not as a scoreboard.

---

## 3. Brutally honest weak points (what to actually fix)

### 3.1 Feature coverage gaps that block basic real-world use

- **There is no way to save a design.** No native document format, no
  `File > Save`, no `File > Open` of anything the app itself wrote. The File
  menu is `Open STEP…`, `Export STL…`, `Quit` and nothing else. A design
  exists only while the process is running. Everything else in this section is
  a smaller problem than this one, because every other gap costs you a feature
  and this one costs you the work.
- **Booleans and the four core solid-creation tools are all present now.**
  Extrude, revolve, sweep and loft exist; so do standalone Combine, solid
  pattern and mirror. This bullet used to be three separate accusations and
  they have all been answered. What remains are the *edges* of the new
  features, which are real and should be known:
  - **Sweep only accepts a closed path.** `ProfileProvider` hands over closed
    wires and nothing else, so the open chain that is the usual Fusion sweep
    path never reaches the feature. `SweepFeature` says so instead of guessing,
    which is the right call, but it means the common case is the unsupported
    one. Widening it is a change to `ProfileProvider`, not to the feature.
  - **Loft has no guide rails and no centerline.** Sections in order, plus
    Closed and Ruled. That is `BRepOffsetAPI_ThruSections` and not much more.
  - **Pattern and mirror copy *bodies*, not features or faces.** Fusion
    patterns a feature, so a patterned hole stays a hole you can edit once.
    Here you get copied solids. And the axes are restricted to world X/Y/Z,
    the mirror planes to world XY/XZ/YZ — deliberately, because a typed-in
    direction can be zero-length and a zero-length direction stacks every
    instance on the first with no error. Defensible, still a limit.
- **Construction geometry is built, filed, and then unreachable.** Eight
  `construct.*` commands produce fully parametric planes, axes and points, and
  they show up in the browser's Construction folder. Nothing ever *draws*
  them — there is no AIS object for a construction plane anywhere in the app —
  and nothing outside `ConstructionFeatures.cpp` consumes them: grep
  `ResolvePlaneByName`, and the only callers are other construction features.
  So you cannot sketch on a construction plane (`SketchPlanePicker` offers the
  three origin rectangles and planar faces of displayed bodies; a construction
  plane is neither), you cannot revolve about a construction axis, you cannot
  mirror across a construction plane. A whole CONSTRUCT menu that feeds
  nothing. This is the cheapest large win on the board after save.
- **Single-body document model, no assemblies.** Every real design beyond a
  single bracket needs more than one component. Combine and Pattern mean the
  document now genuinely holds several *bodies*, which makes the absence of
  *components* — with their own origins, transforms and joints — more visible,
  not less.
- **No 2D drawings.** Without dimensioned output, the tool can produce a
  model but not a deliverable a shop can act on. For a "get something made"
  workflow this is arguably a higher-priority gap than assemblies.

### 3.2 Parametrics are shallower than they look

- **The named-parameter engine exists and nothing uses it.**
  `src/core/Expression.{h,cpp}` and `src/core/ParameterTable.{h,cpp}` are
  written, careful and tested: `hole_dia = plate_width / 4` parses, resolves in
  dependency order, refuses cycles, reports errors as values rather than
  throwing, keeps trig in degrees the way the rest of the document does, and
  refuses to apply a unit twice. `tests/parameters_test.cpp` passes.
  **It is not wired into anything.** Grep both headers across `src/` and the
  only file that includes them is the test. `Document` does not own a
  `ParameterTable`; no feature's `SetParameter` evaluates an expression; there
  is no Change Parameters dialog. So from a user's seat the original criticism
  still stands unchanged — you cannot write an equation and have it propagate —
  and half-done is not done. Saying otherwise here would be a worse error than
  the one this document was written to fix.
- **What `UnitLineEdit`/`Units.cpp` give you is unit-aware numeric *entry*,
  not equations.** That part has always worked and is not the gap.
- No configurations / design variants.

### 3.3 Interaction is incomplete, not just "smaller"

- **The move gizmo moves every body, not the one you grabbed.**
  `MoveGizmoTool` runs an `AIS_Manipulator` with `AIS_MM_Translation` and
  `AIS_MM_Rotation` enabled and `AIS_MM_Scaling` explicitly switched off, so
  there is no scale handle. More seriously: it attaches to the *selected*
  body, but what a drag commits is a `TransformFeature` carrying no target,
  and an untargeted one runs `BRepBuilderAPI_Transform` over the **whole
  upstream shape**. Select one body, drag it, and everything in the document
  moves with it. The numeric Move/Rotate dialog admits this in a code comment;
  the gizmo admits it nowhere a user can see. `TransformFeature` has since
  grown the ability to transform a single body by reference — no caller sets
  it, so the defect stands, and until one does this is half a fix rather than
  a fix. Now that Combine, Pattern and Mirror routinely produce multi-body
  documents, it is a silent-wrongness bug rather than a missing feature, and
  it is the kind this codebase is otherwise good at refusing to ship.
- No marking menu / context radial menu — right-click falls back to whatever
  Qt gives you by default, which is a worse experience than either Fusion or
  most competing hobbyist CAD tools (e.g. FreeCAD's context menus).
- Selection/gizmo code has several documented historical footguns
  (`docs/ARCHITECTURE.md`'s "four traps" section: unloaded objects reporting
  as armed, decorative geometry stealing clicks, degenerate-rectangle
  handling). The fact that these had to be written down as permanent
  warnings suggests the selection subsystem is fragile enough that future
  changes are likely to reintroduce them — this deserves regression tests,
  not just documentation. It still has none: see 3.5.

### 3.4 Import/export is a narrower pipe than it should be, but no longer a straw

- **Out is now STEP, OBJ and STL.** `ExportShapeToStep` writes AP214 in
  millimetres with one STEP product per body; `ExportShapeToObj` writes a
  triangle mesh with one group per body, at the same deflection STL uses. Both
  carry the browser's body names through. The "you can bring a model in but
  can't send your own work back out" complaint is answered.
- **In is still STEP and only STEP**, landing as a single `ShapeFeature` — an
  opaque lump on the timeline with no feature history, which is what import
  means anywhere, but worth stating.
- No IGES, no Parasolid, no SAT, no DXF, no 3MF, no STL *import*.
- The three exports are also scattered: STL hangs off the File menu while
  STEP and OBJ are commands in a **Utilities** ribbon tab. Two places to look
  for one idea.

### 3.5 Testing and robustness

- `tests/run_tests.sh` runs **fourteen** suites — 915 assertions, all passing
  as of this revision. They compile the real sources directly rather than
  linking the app, so they need no window server. Covered today: unit parsing
  and formatting; the unit-aware input widget; a sketch→extrude→edit→undo
  pipeline against real OCCT volumes; sketch axis inference and snapping;
  profile regions and reference resolution; the body/entity taxonomy;
  construction planes, axes and points; the Fusion sketch-curve taxonomy
  (including conics and control-point splines); press/pull face selection and
  offset; the press/pull gizmo's drag geometry; typed sketch input with live
  dimensions; reference identity and its refusal to guess; the parameter
  engine; and sweep, checked against the closed-form volume of a torus.
- **That is a real safety net, and it has three specific holes:**
  - **The constraint solver has no property or fuzz testing — and barely any
    testing at all.** Twelve relations and six driving dimensions go through a
    Levenberg-Marquardt solve with hand-rolled Gaussian elimination, and the
    only test that touches a constraint is `sketchinput_test.cpp`, which
    constructs three dimensions and checks their stored type and value. Nothing
    asserts that the solver *converges*, that it converges to the right
    geometry, or that it behaves on redundant, contradictory or
    nearly-degenerate systems. This is the single most likely source of
    silently wrong output in the app.
  - **The viewport selection traps have no regression test that could catch
    them.** `ARCHITECTURE.md` names four, and three only manifest against a
    live `AIS_InteractiveContext` and a window server: an unloaded object
    reporting a selection mode as active while building no primitives,
    decorative geometry displayed with the default mode stealing clicks, and a
    press that arrives without a preceding `MoveTo` selecting nothing. Every
    suite here is headless by design — none compiles `OcctViewport.cpp` — so no
    headless test can reach them, and the screenshot harness that could is not
    run automatically. The gap is not "write a unit test"; it is deciding which
    side of the window-server line this subsystem gets verified on, and then
    actually running that side.
  - **Combine, Loft, Pattern and Mirror shipped without suites.** Sweep got
    one, and it is a good one (a torus checked against 2·π²·R·r²). The other
    four features do booleans on multi-body compounds and compose transforms,
    which is precisely where a wrong answer looks plausible.
- **The screenshot/`--script` harness is still not wired into CI.** It is a
  genuinely good idea — few hobbyist CAD projects visually verify UI state —
  but it is a *manual* step someone has to remember to run. There is in fact no
  CI at all: no `.github/`, no pipeline config anywhere in the tree. Nothing
  stops a future change from silently breaking sketch rendering the way the
  "readable numbers" and "dimension that reads like a dimension" commits imply
  had already happened at least twice.
- No stated crash-recovery or autosave story — and with no save format, no
  possible one. Undo is a full timeline clone per edit: fine at hobby-project
  scale, unclear how it behaves on a document with hundreds of features
  (Fusion has had its own well-known timeline-performance problems at scale,
  so this isn't hypothetical).

### 3.6 UI polish and product-level rough edges

- Command icons are literally **one emoji per command** (`Icon()` returns a
  string, house style says "ONE emoji"). This will look like a prototype
  next to Fusion's icon set regardless of how correct the geometry engine
  is — icon/visual design is currently the weakest first impression the app
  gives.
- **README.md: fixed.** It has been rewritten and now describes the app that
  actually exists — the sketcher, the solid features, the timeline, and the
  gaps — instead of the "early skeleton" that could only load STEP and write
  STL. Read against the code on 2026-09-21 it is accurate. Keeping it accurate
  is now the job; it drifted once because the app moved faster than its prose,
  and it will do that again.
- No stated error-recovery UX for a failed feature beyond "the error lands
  in `doc.Errors()`" — whether that's surfaced clearly to a user in the UI
  isn't described here and is worth verifying by hand.

### 3.7 Platform ceiling

- Linux-only was presumably the point of the project, so this isn't a flaw
  exactly — but it does mean the addressable user base is small by
  construction, and OCCT's window integration wraps an X11 window handle
  (`Xw_Window`), which is why `main.cpp` forces `QT_QPA_PLATFORM=xcb` when the
  user hasn't set it. Even Wayland support is XWayland-mediated, not native.
  Worth knowing that ceiling exists rather than assuming it'll be free to lift
  later.

---

## 4. If you can only fix five things next

Ranked by how much they unblock, not by ease. The previous edition's list is
gone because most of it has been answered: booleans were never missing, the
README is rewritten, STEP export shipped, and `identity_test.cpp` now pins the
profile/reference identity rules as assertions. Two of its items were only
half-answered and come back below — the selection traps that test was also
meant to cover still have nothing, and named parameters got an engine with no
wiring.

1. **A native save/open format.** Nothing else on this list matters to a user
   who cannot keep the result. It is also the prerequisite for autosave,
   crash recovery and any notion of a project.
2. **Wire the parameter engine in.** The hard part is written and tested and
   is sitting there inert. `Document` needs to own a `ParameterTable`, feature
   `Parameter` slots need to hold expressions instead of doubles, and the
   Change Parameters dialog needs to exist. This is the largest capability
   currently available for the least new code.
3. **Make construction geometry reachable** — draw it in the viewport, let
   `SketchPlanePicker` offer it, and let revolve/pattern/mirror name it. Eight
   commands already build it correctly and nothing can use it.
4. **Tests for the constraint solver and the selection traps, and CI to run
   them.** The two least-tested subsystems are the two whose failures are
   invisible. Cheap relative to the cost of shipping a quietly wrong sketch.
5. **Assemblies / components.** The remaining structural ceiling on what can
   be modelled at all — and the point past which "one document, one body
   table" stops being a simplification and starts being the thing in the way.
