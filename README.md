# citadel-dock

The desktop dock for [AspisOS](https://github.com/AspisOS/AspisOS) — a
capability-based, no-ambient-authority x86-64 operating system built on the
from-scratch [Aegis](https://github.com/AspisOS/Aegis) kernel.

citadel-dock is the persistent launcher bar pinned to the bottom-centre of the
desktop. It is a standalone binary (installed at `/bin/citadel-dock`) and a
client of the [lumen](https://github.com/AspisOS/lumen) compositor: it asks
lumen for a panel, draws a row of application icons into the shared buffer lumen
hands back, and — when an icon is clicked — asks lumen to launch the
corresponding app. It is distributed as a [herald](https://github.com/AspisOS/AspisOS)
system package and is started automatically on a graphical boot.

The dock was originally an overlay rendered inside libcitadel and linked into
the compositor itself; it has been peeled out into this standalone client that
speaks the same external window protocol every other GUI app uses.

## Where it fits in AspisOS

AspisOS is decomposed into independent repositories:

| Repo | Role |
|------|------|
| `AspisOS/Aegis` | The kernel: framebuffer, `AF_UNIX` sockets, the capability model, the syscalls everything graphical runs on. |
| `AspisOS/lumen` | The compositor/display server. Owns the screen; every GUI process connects to `/run/lumen.sock` for a window. |
| `AspisOS/glyph` | The GUI toolkit. Provides drawing primitives, fonts, procedural icons, and the client side of lumen's window protocol (`lumen_client.h`, `lumen_proto.h`). |
| `AspisOS/citadel-dock` | **This repo.** A lumen client that draws the dock and brokers app launches through the compositor. |

The dock holds no display authority of its own — it does not touch the
framebuffer or input devices. It talks to lumen, which composites its panel and
forwards it input events. Everything graphical declares `depends=lumen`.

## What it does

`src/main.c` is a single-file lumen client:

- **Connect.** On start it loops on `lumen_connect()` indefinitely, sleeping
  200 ms between attempts. lumen is started early under a graphical boot but does
  not bind `/run/lumen.sock` until [bastion](https://github.com/AspisOS/bastion)
  authenticates a user — possibly minutes later — so the dock treats
  `ECONNREFUSED` as "not ready yet" and waits rather than exiting (which would
  burn vigil restart credits). Any other connect error is fatal.
- **Create a panel.** It calls `lumen_panel_create()` (`LUMEN_OP_CREATE_PANEL`),
  sized to its icon row. lumen places panels at bottom-centre and returns the
  panel's screen position in the create reply.
- **Render.** `render_dock()` fills the panel with the compositor frost key
  (`C_TERM_BG`) so lumen blurs and tints the desktop behind the bar — the dock
  reads as frosted glass — with a 1px translucent top highlight for a glassy
  edge. Icons are drawn opaque on top via glyph's shared `glyph_icon_draw()`; the
  hovered icon gets a translucent rounded highlight.
- **Items.** A fixed set of launchers — applications, settings, files, terminal,
  calculator, editor — plus an installer icon appended at runtime **only on a
  live boot**. The live check is belt-and-braces: the kernel cmdline must carry
  the `aegis_live` marker *and* `/apps/gui-installer/gui-installer` must exist, so
  a half-cleaned installed disk never shows a dead icon.
- **Launch.** A left-click hit-tests the icon row and sends `lumen_invoke()`
  (`LUMEN_OP_INVOKE`) with the item's invoke name; lumen resolves it against the
  `/apps` bundle registry and spawns it. The invoke names deliberately differ
  from the dock's internal item keys (e.g. `files` → `filemanager`, `installer` →
  `gui-installer`) to match glyph's known icon ids.
- **Unkillable.** It ignores `LUMEN_EV_CLOSE_REQUEST` — the dock cannot be closed
  from the UI.
- **Test hook.** On startup and panel placement it emits `[DOCK]` lines to
  `/dev/console` carrying each icon's screen-space centre, which the
  `dock_click_test` harness parses to drive synthetic clicks.

## Capabilities

citadel-dock ships a cap policy at `pkg/etc/aegis/caps.d/citadel-dock` that
grants only the baseline service profile — no extra capabilities:

```
service
```

It needs none: it draws through the compositor rather than the raw framebuffer,
and it launches apps by asking lumen to invoke them rather than spawning them
itself. It holds no ambient authority.

The herald package id (`citadel-dock`) is a distribution name; because the
package installs a `/bin` binary, a cap policy, and a vigil service across
`/bin`, `/etc/aegis`, and `/etc/vigil`, it is a `class=system` package:
first-party and signature-trusted, installed verbatim by herald.

## Building

citadel-dock builds with a musl cross-compiler against a pinned
[glyph](https://github.com/AspisOS/glyph) toolkit artifact, then packs a signed
herald package.

```sh
make MUSL_CC=/path/to/musl-gcc HERALD_KEY=/path/to/signing.key
```

The `Makefile` fetches the toolkit, compiles `src/*.c` against it, and packs the
`.hpkg`:

- `GLYPH_VERSION` pins the glyph release fetched by `tools/fetch-glyph.sh` (local
  `vendor/` cache first, otherwise the GitHub release). Links
  `-lcitadel -laudio -lauth -lglyph` — a static archive only contributes the
  objects actually referenced.
- `MUSL_CC` is the musl cross-compiler (the only toolchain assumption; defaults
  to `musl-gcc` on `PATH`). Point it at an Aegis-native `cc` to build on-device
  in the future.
- `HERALD_KEY` signs the package (ECDSA P-256).

Output: `citadel-dock.hpkg` (a `class=system` herald package) plus its detached
`citadel-dock.hpkg.sig`.

## Package payload

`citadel-dock.hpkg` is a manifest-first, uncompressed POSIX `ustar` archive with
a detached ECDSA-P256/SHA-256 signature. herald installs the payload tree
verbatim at these paths:

```
/bin/citadel-dock                      the dock binary (stripped)
/etc/aegis/caps.d/citadel-dock         capability policy (baseline service)
/etc/vigil/services/citadel-dock/      vigil service: mode=graphical, respawn, max_restarts=5
```

The vigil service runs the dock as `root` on a graphical boot and respawns it
(up to 5 restarts) if it exits; on a text/server boot it is never started.

## Repository layout

```
src/main.c        the dock client (connect, panel, render, hit-test, invoke)
pkg/              install-tree skeleton shipped verbatim (caps.d + vigil service)
tools/
  fetch-glyph.sh  fetch + unpack the pinned glyph toolkit artifact
  pack.sh         build + sign citadel-dock.hpkg
Makefile          fetch toolkit → build → pack
VERSION           this component's version
GLYPH_VERSION     the pinned glyph toolkit version it builds against
```

## Dependencies

`depends=lumen` — the dock is a lumen client and launches apps through the
compositor, so installing it pulls [lumen](https://github.com/AspisOS/lumen)
(which in turn ships the desktop fonts every component inherits).
