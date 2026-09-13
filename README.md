# linuxCAD

A from-scratch, Fusion 360-style parametric CAD app for Linux, built on
[OpenCASCADE (OCCT)](https://dev.opencascade.org/) for solid modeling and
Qt6 for the UI.

This is an early skeleton: it can load a STEP file, display it in a 3D
viewport (rotate/pan/zoom), and export the loaded shape to STL. No
sketcher or parametric feature tree yet — see the "Next milestones"
section of the project plan for what's coming.

## Build

Dependencies (Fedora package names): `opencascade-devel`, `qt6-qtbase-devel`, `cmake`, `gcc-c++`.

```bash
cmake -B build
cmake --build build -j$(nproc)
```

## Run

```bash
./build/linuxcad
```

The app forces the Qt `xcb` (X11/XWayland) platform plugin, since
OCCT's window integration on Linux currently wraps an X11 window handle
— this works transparently under both X11 and Wayland sessions via
XWayland. Set `QT_QPA_PLATFORM` yourself before launching if you need
different behavior.

## Usage

- **File → Open STEP...** — load a `.step`/`.stp` file.
- **File → Export STL...** — triangulate and export the currently loaded
  shape as an ASCII STL file.
- Mouse: left-drag to rotate, middle-drag to pan, scroll to zoom.
