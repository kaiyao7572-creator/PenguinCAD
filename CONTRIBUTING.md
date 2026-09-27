# Contributing to PenguinCAD

PenguinCAD is early: one maintainer, a working modelling core, and a long
list of things Fusion 360 users will miss. That list is the point of this
file. Everything below is real, unclaimed work, and you don't need to know
OpenCASCADE to pick most of it up.

## Before you start

1. **Build it and run the tests.**

   ```bash
   sudo dnf install opencascade-devel qt6-qtbase-devel cmake gcc-c++ git   # Fedora
   cmake -B build && cmake --build build -j$(nproc)
   ./tests/run_tests.sh      # should end with "All test suites passed."
   ```

2. **Read [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) in full.** It is
   the contract rather than a tour. It covers the `Feature` lifecycle,
   references by name, and why identity resolution refuses to guess. It also
   documents the viewport selection traps and the house style.
3. **Claim something.** Open an issue, or comment on an existing one, saying
   what you'll take, so two people don't build the same thing.
4. **Send a pull request with a test.** Model-level changes get a case in
   one of the suites under `tests/`. UI changes can come with evidence: the
   app replays scripted input and photographs itself
   (`penguincad --script`, documented in `src/InputScript.h`).

House style, briefly: members are `myFoo` in core classes and `m_foo` in Qt
widget classes, parameters are `theFoo`, comments explain *why* rather than
what, and commit messages say what changed and why it matters.

AI-assisted contributions are welcome, and much of the existing code was
written with Claude Code. If you use a tool, review and test what it wrote
as if you had written it yourself, and say so in the pull request.

## Good first issues

Small, visible, and mostly Qt. Each one shows up in the screenshot on the
project website.

- **The properties panel clips its own fields.** Values are cut off, so
  "mm" reads as "nm". `src/ui/PropertiesPanel.cpp`.
- **The browser truncates names.** "Sketch1" becomes "Sket…" next to its
  type column. `src/ui/BrowserPanel.cpp`.
- **The timeline is bare text buttons.** Fusion's shows a feature icon per
  step and a rollback marker you can drag. `src/ui/TimelinePanel.cpp`.
- **The ribbon shows a stray horizontal scrollbar** under the command
  buttons. The ribbon is built in `src/MainWindow.cpp`.
- **The test runner assumes Fedora's paths** for Qt headers, `moc` and
  `/usr/lib64`. Detecting them, or taking them from CMake, would let the
  suites run on other distributions. `tests/run_tests.sh`.
- **Change Parameters is plain.** It works, but it is a bare tree. Fusion's
  dialog adds filtering, favourites and inline errors.
  `src/ui/ParametersDialog.cpp`.

## Modelling gaps

Medium-sized, and they mean learning a little OpenCASCADE.

- **Fillet and chamfer on picked edges.** Today both apply to every edge
  of the body. `src/features/ModifyFeatures.*` and `FilletCommand` in
  `src/features/FeatureCommands.cpp`. Edge references have to survive a
  rebuild, which `docs/ARCHITECTURE.md` covers under references.
- **A Hole command.** The marking menu already reserves a greyed Hole
  wedge for it.
- **Sketch on a construction plane.** Construction planes are drawn and
  parametric, but `sketch.create` can't be clicked onto one yet
  (`docs/HANDOFF.md` §7.3).
- **Sweep along an open path.** Sweep takes only closed path profiles and
  says so. `src/features/SweepFeature.*`.
- **Patterns and mirror that copy features**, not only bodies, and that use
  any axis or plane rather than the world ones.

## Big projects

Each of these is a document-model change. Start with a design discussion in
an issue before writing code; `docs/HANDOFF.md` §7.6 has notes.

- **Assemblies and joints.** Components, occurrences, and Fusion's joint
  types.
- **2D drawings.** Projected views, dimensions and a sheet that a workshop
  can build from.
- **CAM.** Probably by integrating an existing open-source toolpath library
  rather than writing one.

## Packaging and CI

There are no packages yet, so trying PenguinCAD means building it.

- **Flatpak**, **AppImage**, an **AUR** package, a **COPR** repository.
- **CI**: a GitHub Actions job that builds the app and runs
  `tests/run_tests.sh` on every pull request. A Fedora container has every
  dependency packaged.
- **Other distributions and platforms**: a working build recipe for your
  distribution belongs in the README. Nobody has tried Windows or macOS;
  Qt and OpenCASCADE both run there.

## Design and docs

No C++ needed.

- **Icons.** An SVG set is being drawn; it will need a designer's eye.
- **A light theme**, and a pass over spacing and typography in the panels.
- **A first-model tutorial** for someone coming from Fusion.
- **The website.** Edit `website/src/page.html`, run
  `python3 website/build.py`, and commit both it and the generated
  `website/index.html`. It includes a comparison table with other CAD tools;
  corrections are welcome.
- **[`docs/FUSION360_COMPARISON.md`](docs/FUSION360_COMPARISON.md)**, the
  running audit of where PenguinCAD falls short of Fusion.

## Reporting a problem

Open an issue with what you did, what you expected, and what happened. If
you know Fusion, "Fusion does X here, PenguinCAD does Y" is the most useful
report there is. A screenshot helps; a `--script` that reproduces it is
even better.

## License

PenguinCAD is licensed under the GNU General Public License v3.0 or later.
By contributing you agree that your contributions are licensed the same way.
