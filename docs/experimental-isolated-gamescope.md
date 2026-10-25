# Experimental isolated Gamescope sessions

This branch narrows isolated sessions to one goal: keep the host's desktop
usable while a game runs in its own Gamescope session, on its own Hermes-KMS
card and seat, as the same Unix user. Separate desktops and separate files per
client are not the aim here. Input is the MrOz59/libvirtualhid stack from
`experimental/libvirtualhid-input`, merged in.

The setup follows the one reported in [#57](https://github.com/MrOz59/Hermes/issues/57)
(X11 desktop, Gamescope on Wayland per session, AMD GPU, CachyOS).

## What Hermes does

- **Scanout on the session's card.** Gamescope renders on the real GPU and
  scans out on the session's Hermes-KMS card (`-O` picks its connector).
  Upstream cannot be told the two apart: it opens the primary node of the GPU
  it renders with, which is the host's. Hermes runs `gamescope --help` (once
  per binary, again when the file changes) and passes the session's card with
  the option the build lists:
  - `--prefer-drm` - [ValveSoftware/gamescope#2220](https://github.com/ValveSoftware/gamescope/pull/2220)
    ([relil/gamescope-eGPU](https://github.com/relil/gamescope-eGPU)). The card
    is also exported as `GAMESCOPE_PREFER_DRM`, which that patch reads too.
  - `--drm-device` - the downstream patch from #57.

  A build that lists neither is refused, with an error saying why, rather than
  started against the host's display. `HERMES_ISOLATED_GAMESCOPE_DRM_FLAG`
  (`--prefer-drm`, `--drm-device` or `none`) skips the probe, for a build whose
  usage text does not list its option; `none` reproduces what upstream does and
  is only useful to see it fail.
- **The card is per session.** It is the card Hermes created for that client,
  so two sessions are two cards. `HERMES_ISOLATED_GAMESCOPE_DRM_DEVICE` is no
  longer read.
- **Input seat.** Hermes sets `XDG_SEAT=hermes-kms-N`, and the session's
  virtual devices carry that seat. Gamescope also gets
  `WLR_LIBINPUT_NO_DEVICES=1`: the seat is empty until the stream starts and
  creates the session's devices, and wlroots' libinput backend otherwise
  refuses to start on it.
- **Display environment.** Gamescope does not inherit the desktop's
  `WAYLAND_DISPLAY`, `WAYLAND_SOCKET`, `_WAYLAND_DISPLAY`, `DISPLAY` or
  `HERMES_WAYLAND_DISPLAY`, so it cannot nest itself inside the desktop.
- **Steam mode, per application.** The application setting *Gamescope Steam
  mode* (`"gamescope-steam-mode": true` in `apps.json`) adds `-e` and a second
  Xwayland server (`--xwayland-count 2`), as SteamOS runs `steam -gamepadui`.
  `HERMES_ISOLATED_GAMESCOPE_STEAM_MODE=1` in the server's environment still
  forces it on for every application.
- **First frame.** The session has `first-frame-timeout` seconds (per
  application; default 30 for application sessions, 5 for desktop sessions) to
  attach a framebuffer to its card before the launch fails. Steam's gamepad UI
  regularly needs far more than the old 5 seconds.
- **No mouse nudge.** Isolated streams no longer move the mouse +1/-1 when
  they start; in Gamescope that put a cursor in the middle of the gamepad UI.
- **Audio on resume.** A resumed stream captures the session's own sink. It
  used to take the host path and move the host's default sink.

Hermes-KMS needs its own fix for the seat brokers: `hermes-kms-seatd@.service`
must run seatd with `SEATD_VTBOUND=0`. VT-bound - seatd's default - each broker
adopts the VT active on seat0 and switches it between graphics and text mode as
sessions come and go, which is what put tty1 into text mode under a `startx`
desktop in #57.

## What Gamescope needs

Two things Hermes cannot do from outside:

1. **A scanout selector** - #2220 or the #57 patch, above.
2. **Reading input from the session seat.** Gamescope's wlroots asks libseat
   which seat it is on, and seatd only ever answers `seat0`, so libinput is
   pointed at the host's keyboard and mouse - they reach the game as well as
   the desktop. The #57 patch makes Gamescope take its libinput seat from
   `XDG_SEAT`, which Hermes already sets. This is the same limitation the
   Hermes-KMS seat rule documents for wlroots compositors, and the counterpart
   of weston's `--seat`. It has no upstream proposal yet.

Without (1) a session does not start. Without (2) it starts, but host input
leaks into it.

## Settings at a glance

| Setting | Where | Effect |
| --- | --- | --- |
| `gamescope-steam-mode` | app | `-e` and `--xwayland-count 2` |
| `first-frame-timeout` | app | seconds to the first frame; `0` = default |
| `HERMES_ISOLATED_GAMESCOPE_DRM_FLAG` | server env | force `--prefer-drm`, `--drm-device` or `none` |
| `HERMES_ISOLATED_GAMESCOPE_STEAM_MODE=1` | server env | Steam mode for every app |

Server environment variables go in the environment of the Hermes **server
process**; restart Hermes after changing them.

## Validation needed

1. Desktop and session at once: physical keyboard/mouse only on the desktop,
   client input only in the session (needs the Gamescope seat patch).
2. With the patched Gamescope: confirm in its log that it scans out on the
   session card and renders with the real GPU; check capture for CPU readbacks.
3. With unpatched Gamescope: the launch must fail with the selector error and
   leave the host's display alone.
4. Steam mode with `steam -gamepadui`: overlay, no stray cursor, first frame
   within the deadline. Leave it off for non-Steam applications.
5. Disconnect and resume: the resumed stream carries the game's audio and the
   desktop keeps its own.
6. With the Hermes-KMS seatd fix installed: start and stop a session from a
   `startx` desktop on tty1; tty1 must stay in graphics mode.

## Not covered yet

Reported in #57 and waiting on the patches promised there: moving streams that
pick a sink by name (Unity) onto the session's sink, microphone forwarding
(needs Artemis changes), attaching the gamepad when the stream starts, seat
permissions for the session user, and sharing the host's Discord IPC socket
with the session's runtime directory.

The session runs as the host user with the host's `HOME`. Steam allows one
instance per user, so a Steam client already running on the desktop is likely
to take over a `steam -gamepadui` started in the session; this is untested.
