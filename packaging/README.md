# Packaging PenguinCAD

Everything a Linux desktop needs to find, show and launch PenguinCAD lives
here, keyed by its application ID, `io.github.kaiyao7572_creator.PenguinCAD`:

| File | What it does |
| --- | --- |
| `*.desktop` | Puts PenguinCAD in the application menu and search |
| `*.metainfo.xml` | Describes it to software centres (GNOME Software, Discover, Flathub) |
| `*.svg` | The app icon |
| `*.yml` | The Flatpak manifest |
| `screenshots/` | The screenshots the metainfo points software centres at |

`cmake --install build` installs the first three alongside the binary, so a
plain `sudo cmake --install build` on any distribution also gives you a menu
entry.

## The Flatpak

The manifest builds OpenCASCADE 7.9.3 from source (no Flatpak runtime ships
it) and then PenguinCAD from this checkout, on the KDE 6.11 runtime. The
first build takes a while, mostly OpenCASCADE; later builds reuse it.

```bash
flatpak install --user flathub org.flatpak.Builder
flatpak run org.flatpak.Builder --user --install --install-deps-from=flathub \
    --force-clean build-flatpak packaging/io.github.kaiyao7572_creator.PenguinCAD.yml
flatpak run io.github.kaiyao7572_creator.PenguinCAD
```

After installing, PenguinCAD shows up in the application grid and in search
like any other app.

### The one-file download

CI (`.github/workflows/flatpak.yml`) builds `PenguinCAD-x86_64.flatpak` on
every push. Pushing a version tag attaches it to that GitHub release:

```bash
git tag -a v0.1.0 -m "PenguinCAD 0.1.0" && git push origin v0.1.0
```

Someone who downloads that file can double-click it to install it through
GNOME Software or KDE Discover, or run:

```bash
flatpak install --user PenguinCAD-x86_64.flatpak
```

The bundle knows to fetch the KDE runtime from Flathub, so it works on any
distribution with Flatpak and Flathub set up. A bundle does not update itself:
a new version means downloading the new file.

### Flathub

Flathub is what makes PenguinCAD searchable and installable from inside
GNOME Software and Discover, with automatic updates. Submitting is a pull
request to [flathub/flathub](https://github.com/flathub/flathub) following
[its submission guide](https://docs.flathub.org/docs/for-app-authors/submission).
For that copy of the manifest, replace the last module's `dir` source with the
tagged release:

```yaml
    sources:
      - type: git
        url: https://github.com/kaiyao7572-creator/PenguinCAD.git
        tag: v0.1.0
        commit: <the tag's full commit hash>
```

Flathub's reviewers will ask why the app needs `--socket=x11` rather than
Wayland: OpenCASCADE's viewer draws into an X11 window (`Xw_Window`), and
PenguinCAD has no native Wayland path yet.

When releasing, add a `<release>` entry to the metainfo first; software
centres show those as the changelog.
