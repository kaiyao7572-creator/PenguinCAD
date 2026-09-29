# PenguinCAD's own Flatpak repository

GitHub Pages serves a signed Flatpak repository next to the website:

| URL | What it is |
| --- | --- |
| `https://kaiyao7572-creator.github.io/PenguinCAD/penguincad.flatpakref` | One click: adds the repository and installs PenguinCAD |
| `https://kaiyao7572-creator.github.io/PenguinCAD/penguincad.flatpakrepo` | Adds the repository only |
| `https://kaiyao7572-creator.github.io/PenguinCAD/repo/` | The OSTree repository itself |

Once the repository is added, PenguinCAD shows up in GNOME Software and KDE
Discover search, and every release reaches users as an ordinary update. The
`.flatpak` bundle on the releases page carries the same repository URL, so a
bundle installed by hand updates from here too. The Qt runtime still comes
from Flathub.

## How a release gets here

1. A `v*` tag runs `.github/workflows/flatpak.yml`. Only that release build
   imports the private key, and it refuses to continue if the secret is
   missing or is not the key below. It builds on the `stable` branch, signs
   the commit, the app catalogue (the appstream branches software centres
   search) and the repository summary, then checks the result the way a
   client would: trusting only `penguincad.gpg`, it lists the app and pulls
   the catalogue. Only then does it attach `flatpak-repo.tar.gz`, the bundle
   and its checksum to the release.
2. When that finishes, `.github/workflows/pages.yml` deploys the website with
   the latest release's repository unpacked under `/repo/`. If it cannot
   fetch the repository it fails rather than deploy a site without one,
   which would cut off every installed copy's updates.

Each release publishes only its own commit. Clients update to it all the
same: OSTree fetches what they are missing and never needs the old commit
on the server. GitHub Pages caches files for ten minutes, so a client that
refreshes in the minutes after a release can briefly see a signature
mismatch; it clears up on the next refresh.

Builds are x86-64 only for now.

## The signing key

`penguincad.gpg` is the public key (fingerprint
`4C9E 06E9 2D42 2260 FBB0  0322 950B 95F4 F533 9F56`); both files above
embed it. The private key is the `FLATPAK_GPG_PRIVATE_KEY` Actions secret
and is not in this repository. Builds that are not releases never see it and
publish nothing.

Losing the private key means shipping a new key in new `.flatpakref` and
`.flatpakrepo` files, and every existing user re-adding the repository.
Keep a backup of it somewhere other than GitHub.

## Upgrading from 0.1.0

0.1.0's downloadable bundle predates this repository: it was built on the
`master` branch with no update source, so nothing can reach it. Its users
move over once:

```bash
flatpak uninstall --user io.github.kaiyao7572_creator.PenguinCAD//master
flatpak install --user https://kaiyao7572-creator.github.io/PenguinCAD/penguincad.flatpakref
```
