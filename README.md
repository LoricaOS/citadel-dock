# citadel-dock

The desktop dock (taskbar) for **AspisOS**, a capability-based,
no-ambient-authority operating system built on the from-scratch
[Aegis](https://github.com/AspisOS/Aegis) kernel.

citadel-dock is the persistent launcher bar at the bottom of the desktop. It is
a standalone `/bin` binary that talks to the [lumen](https://github.com/AspisOS/lumen)
compositor over the external window protocol (`LUMEN_OP_CREATE_PANEL` to place
its panel, `LUMEN_OP_INVOKE` to launch apps), rather than being linked into the
compositor as an overlay. It is a component of the Lumen desktop, distributed as
a [herald](https://github.com/AspisOS/AspisOS) package.

## Role in the system

- Started by the `vigil` service manager **only on a graphical boot** (it ships
  its own `vigil` service definition, `mode=graphical`, `respawn`,
  `max_restarts=5`). It is never started on a text/server boot.
- Renders an opaque dark dock with a fixed set of icons — applications,
  settings, files, terminal, calculator, editor — plus an installer icon that is
  appended at runtime on a live boot only. Clicking an icon asks Lumen to spawn
  the corresponding app by name via `LUMEN_OP_INVOKE`.
- Because it talks to Lumen over the external protocol it cannot read the
  compositor's underlying pixels, so the old frosted-glass blur was dropped in
  favour of an opaque background when the dock was split out of `libcitadel`.

## Capabilities

citadel-dock's cap policy (`pkg/etc/aegis/caps.d/citadel-dock`) holds only the
baseline:

```
service
```

It needs no extra capabilities: it draws through the compositor (not the raw
framebuffer) and launches apps by asking Lumen to invoke them, so it holds no
ambient authority of its own.

Because its herald package id (`citadel-dock`) is a distribution name and it
installs a `/bin` binary plus a cap policy and a vigil service, citadel-dock is a
`class=system` package: first-party and signature-trusted, installed verbatim by
herald.

## Building

citadel-dock fetches a pinned [glyph](https://github.com/AspisOS/glyph) toolkit
artifact (the GUI libraries it links: glyph + libaudio + libauth + libcitadel)
and builds against it, then packs a signed herald package.

```sh
make MUSL_CC=/path/to/musl-gcc HERALD_KEY=/path/to/signing.key
```

- `GLYPH_VERSION` pins the toolkit release fetched by `tools/fetch-glyph.sh`.
- `MUSL_CC` is the musl cross-compiler (the only toolchain assumption — point it
  at an Aegis-native `cc` to build on-device in the future).
- `HERALD_KEY` signs the `.hpkg`.

Output: `citadel-dock.hpkg` (a `class=system` herald package) + `citadel-dock.hpkg.sig`.

## Package payload

```
/bin/citadel-dock                       the dock binary
/etc/aegis/caps.d/citadel-dock          its capability policy (baseline service)
/etc/vigil/services/citadel-dock/       the vigil service (mode=graphical, respawn)
```

## Repository layout

```
src/        citadel-dock source
pkg/        install-tree skeleton shipped verbatim (caps.d + vigil service)
tools/      fetch-glyph.sh (toolkit fetch) + pack.sh (build the signed .hpkg)
Makefile    fetch toolkit -> build -> pack
VERSION         this component's version
GLYPH_VERSION   the pinned glyph toolkit version it builds against
```

## Dependencies

`depends=lumen` — the dock is a Lumen client and launches apps through the
compositor, so installing it pulls [lumen](https://github.com/AspisOS/lumen)
(which in turn provides the desktop fonts).
