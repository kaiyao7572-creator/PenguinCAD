# Packaging PenguinCAD

Everything a Linux desktop needs to find, show and launch PenguinCAD lives
here, keyed by its application ID, `io.github.kaiyao7572_creator.PenguinCAD`:

| File | What it does |
| --- | --- |
| `*.desktop` | Puts PenguinCAD in the application menu and search |
| `*.metainfo.xml` | Describes it to software centres (GNOME Software, Discover, Flathub) |
| `*.svg` | The app icon |
| `*.xml` | The `.pcad` file type, so a double-click opens a design in PenguinCAD |
| `*.yml` | The Flatpak manifest |
| `screenshots/` | The screenshots the metainfo points software centres at |

`cmake --install build` installs the first four alongside the binary, so a
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

### PenguinCAD's own repository

Releases are published as a signed Flatpak repository on the project's GitHub
Pages site; [`repo/README.md`](repo/README.md) covers how, and the signing key.
It is what the website's Install button adds.

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
distribution with Flatpak and Flathub set up, and it names PenguinCAD's own
repository as its origin, so once installed it updates from there.

### Flathub

Flathub is what makes PenguinCAD searchable and installable from inside
GNOME Software and Discover, with automatic updates. Submitting is a pull
request to [flathub/flathub](https://github.com/flathub/flathub) following
[its submission guide](https://docs.flathub.org/docs/for-app-authors/submission).

Read Flathub's [Generative AI policy](https://docs.flathub.org/docs/for-app-authors/requirements)
first. Flathub manifests must not contain AI-generated or AI-assisted
content, and the manifest in this directory was written with an AI tool, so
**do not submit it or a copy of it**: the Flathub manifest has to be written
by hand, from Flathub's own documentation. The same policy forbids
AI-generated commit messages and automated pull requests for the submission,
and requires disclosing AI-generated material in the application itself,
which applies to PenguinCAD.

What this directory does make ready is everything the manifest installs: the
desktop entry, metainfo, icon, MIME type and screenshots. A build made with
Flathub's flags passes `flatpak-builder-lint` with no errors:

```bash
flatpak run org.flatpak.Builder --user --force-clean --compose-url-policy=full \
    --mirror-screenshots-url=https://dl.flathub.org/media --repo=repo build-dir <manifest>
flatpak run --command=flatpak-builder-lint org.flatpak.Builder repo repo
```

Facts a manifest for PenguinCAD needs: it builds with CMake; OpenCASCADE
7.8 or newer must be built alongside it, since no runtime ships it; it uses
Qt 6 Widgets (the KDE runtime); its viewer draws into an X11 window
(`Xw_Window`), so it needs X11 and has no native Wayland path yet; it needs
the GPU for OpenGL; and it needs no filesystem access, because files go
through the portal. Flathub builds x86-64 and aarch64; if aarch64 fails, a
`flathub.json` next to the manifest can limit the build to x86-64.

When releasing, add a `<release>` entry to the metainfo first; software
centres show those as the changelog.
