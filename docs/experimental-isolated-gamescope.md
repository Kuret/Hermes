# Experimental isolated Gamescope sessions

This branch narrows isolated sessions to one goal: a game runs in its own
Gamescope session, on its own Hermes-KMS card and input seat, as the same Unix
user, while the host's desktop stays in use - and neither gets in the other's
way. Separate desktops and separate files per client are not the aim here.

Most of the session work is d3adb5's, from
[#57](https://github.com/MrOz59/Hermes/issues/57) and
[d3adb5/hermes](https://github.com/d3adb5/hermes), cherry-picked with his
authorship. Input is the MrOz59/libvirtualhid stack from
`experimental/libvirtualhid-input`, merged in.

## Setting it up

Not available on Ubuntu 24.04 for now: the Gamescope these sessions run in
needs newer libwayland, libdrm and libxkbcommon than 24.04 ships, so Hermes'
Gamescope build is packaged for Ubuntu 26.04, Fedora 43/44 and Arch/CachyOS
only.

`scripts/install.sh` does all of this and asks before each part:

1. Hermes-KMS, configured for your user with
   `sudo /usr/lib/hermes-kms/hermes-kms-setup configure --user <uid>`, and its
   private seat brokers (`hermes-kms-seatd@N`) - which must run with
   `SEATD_VTBOUND=0` (Hermes-KMS PR #16), or they put the host's VT into text
   mode when a session ends.
2. Hermes' Gamescope build (below), PipeWire with WirePlumber, and seatd.
3. `cap_sys_admin,cap_mknod,cap_sys_chroot+p` on `/usr/bin/hermes` for a
   private `/dev` (the packages in this branch set them).
4. `hermes_kms_isolated_sessions = enabled` and
   `virtual_display_backend = hermes_kms`.

## Gamescope

Stock Gamescope cannot run these sessions:

- it scans out on the GPU it renders with - the host's display - instead of
  the session's card;
- its wlroots asks libseat for the seat, seatd always answers `seat0`, and
  libinput reads the host's keyboard and mouse.

[MrOz59/gamescope](https://github.com/MrOz59/gamescope), branch `hermes`, is
upstream `master` plus `--prefer-drm`
([ValveSoftware/gamescope#2220](https://github.com/ValveSoftware/gamescope/pull/2220)),
the libinput seat from `XDG_SEAT` and `wl_touch.frame` after passthrough touch
(both from d3adb5). It installs into `/usr/lib/hermes-gamescope`, beside the
distribution's Gamescope; a source build goes to `/opt/hermes-gamescope`.

Hermes looks for Gamescope in those two prefixes first, then on `PATH`
(`HERMES_GAMESCOPE` names a binary outright). It runs `gamescope --help` once
per binary and passes the session's card with `--prefer-drm` or
`--drm-device` (d3adb5's downstream patch), whichever the build lists, and
exports it as `GAMESCOPE_PREFER_DRM`. A build with neither is refused rather
than started against the host's display; `HERMES_ISOLATED_GAMESCOPE_DRM_FLAG`
(`--prefer-drm`, `--drm-device` or `none`) skips the probe. From a prefix, the
session's `PATH` starts with that build's `bin` (Gamescope finds
`gamescopereaper` on `PATH`) and `VK_ADD_IMPLICIT_LAYER_PATH` points at its WSI
layer.

## What a session gets

- **Display.** Its own Hermes-KMS card; Gamescope never inherits the desktop's
  `WAYLAND_DISPLAY` or `DISPLAY`.
- **Input.** `XDG_SEAT=hermes-kms-N`; the client's virtual devices carry that
  seat, and `WLR_LIBINPUT_NO_DEVICES=1` lets Gamescope start before they exist.
  With a fixed gamepad type, the client's pads are plugged in when the stream
  starts. `74-hermes-session-acl.rules` opens the seat's input and hidraw nodes
  to the session user; Steam Input pads made inside a session take its seat.
- **No stray cursor.** No +1/-1 mouse nudge, and the wiggle Moonlight-based
  clients send on connect is dropped (`isolated_ignore_mouse_wiggle`).
- **Audio.** Its own PipeWire and WirePlumber with no hardware: the session's
  sink is its only device, and with `stream_mic` the client's microphone is
  its only source. Without PipeWire it falls back to a sink on the host's
  server. A resumed stream captures the session's sink.
- **Devices.** `isolate-devices` gives it a private `/dev` (its own mount
  namespace) with only its client's devices; `isolate-sound` removes
  `/dev/snd`.
- **Steam.** `stop-desktop-steam` shuts down a Steam running on the desktop
  first, since Steam runs once per user. The host's Discord IPC sockets are
  linked into the session's runtime directory.

## Per-app settings

| Setting | Values | Effect |
| --- | --- | --- |
| `gamescope-steam-mode` | `auto` / `on` / `off` | `-e` and a second Xwayland; `auto` when `cmd` runs `steam` |
| `gamescope-args` | flags | replaces `--expose-wayland --force-windows-fullscreen` |
| `first-frame-timeout` | seconds | time to the first frame; `0` = 30 s (application), 5 s (desktop) |
| `isolate-devices` | `auto` / `on` / `off` | private `/dev`; `auto` when Hermes has the capabilities |
| `isolate-sound` | `auto` / `on` / `off` | no `/dev/snd` |
| `stop-desktop-steam` | `auto` / `on` / `off` | stop the desktop's Steam first; `auto` when `cmd` runs `steam` |

Server environment: `HERMES_GAMESCOPE`, `HERMES_ISOLATED_GAMESCOPE_DRM_FLAG`,
and `HERMES_ISOLATED_GAMESCOPE_STEAM_MODE=1` (Steam mode for every app).

## Validation needed

1. Desktop and session at once: physical input only on the desktop, client
   input only in the session; host controllers absent from the session with
   `isolate-devices`.
2. libvirtualhid controllers inside a private `/dev`: the mirror finds UHID
   devices by `HID_PHYS`; check event, js and hidraw nodes all appear.
3. Gamescope from `/usr/lib/hermes-gamescope`: scanout on the session card,
   rendering on the real GPU, and the WSI layer picked up from the prefix.
4. Steam mode with `steam -gamepadui`: overlay, no stray cursor, first frame in
   time, the desktop's Steam stopped and its settings saved.
5. Audio and microphone: game audio only on the client, desktop audio
   untouched, microphone only in the session; disconnect and resume.
6. With the Hermes-KMS seatd fix: start and stop a session from a `startx`
   desktop on tty1; tty1 stays in graphics mode.
