# Experimental Linux input backend: MrOz59/libvirtualhid

**Status: experimental branch only.** This document describes
`experimental/libvirtualhid-input`. There is no PR to `main`, release date,
or promise of production compatibility.

## Dependency provenance

The dependency is **our fork**, [MrOz59/libvirtualhid](https://github.com/MrOz59/libvirtualhid),
NOT the upstream LizardByte repository. CMake fetches exactly commit
`439579c38180f9dad7c5324da195374cdbd325b4` from that fork. Updating
the dependency requires reviewing and intentionally changing this commit SHA.

The library uses C++23, standard Linux `uinput`/`uhid` interfaces, and
`libevdev`. Existing Hermes needs for libevdev and GPU/capture dependencies
remain unchanged. The `/dev/uinput` device must be accessible to Hermes.
Do not use `lvh::BackendKind::fake` in production: it accepts events but
creates no Linux virtual devices.

## First integration milestone

| Path | Active implementation with `HERMES_LVH_PEN_TOUCH=ON` | Status |
| --- | --- | --- |
| S Pen / digitizer | `lvh::PenTablet` | Integrated, no physical validation |
| Multitouch / finger hover | `lvh::Touchscreen` | Integrated, no physical validation |
| Mouse, absolute mouse, scroll | `inputtino` | Not migrated |
| Keyboard / text / shortcuts | `inputtino` | Not migrated |
| Gamepads (Xbox One, Switch, DualSense) | `inputtino` | Not migrated |
| Gamepad motion / touchpad / battery | `inputtino` | Not migrated |
| Rumble / LEDs / adaptive triggers | `inputtino` | Not migrated |

Hermes' existing input packets and client-permission checks remain
unchanged. Pen and touch are initialized per client, and the previous
`0xBEEF:0xDEAD` virtual-input vendor/product identifiers and device
names are retained for desktop display binding. Touch contact IDs,
`CANCEL_ALL`, stylus pressure, hover distance, tilt, eraser, and stylus
side buttons are translated into the new API. Button-only pen events
do not interrupt contact.

## Building

```bash
git clone --recursive https://github.com/MrOz59/Hermes.git
cd Hermes
git switch experimental/libvirtualhid-input
git submodule update --init --recursive
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DHERMES_LVH_PEN_TOUCH=ON
cmake --build build --target sunshine -j2
```

The first configure fetches the pinned `MrOz59/libvirtualhid` revision.
A fresh checkout and network access are needed for FetchContent.
The `experimental/libvirtualhid-input` branch has its own Actions
workflow which compiles **both** configurations:

```bash
# A/B baseline: pen and touch continue using inputtino
cmake -B build-inputtino -G Ninja -DHERMES_LVH_PEN_TOUCH=OFF
cmake --build build-inputtino --target sunshine -j2
```

**Do not launch both builds simultaneously on one seat**: two processes can
compete for virtual-display devices and input. A build can compile without
`/dev/uinput` access but cannot create the real input devices without it.

## Known gaps before retiring inputtino

1. Migrate mouse and keyboard while preserving relative/absolute pointer
   placement, high-resolution scroll, Linux layout/scancode semantics,
   Unicode text, and X11 fallback.
2. Reproduce gamepad selection and capability signaling, per-client
   indices, stable controller identity, rumble, LED, adaptive triggers,
   motion sensors, battery and controller touchpad.
3. Replace temporary dual-library build wiring and inputtino-specific
   settings with an adapter owning all `lvh::Runtime` devices.
4. Test Wayland and X11 pen/touch with physical hardware (especially S Pen,
   eraser and barrel button), multi-monitor offsets, GNOME/KDE display
   binding and per-client isolation. Verify no duplicate or stuck devices
   after disconnect.
5. Add reproducible fake-runtime behavioral tests and a long-running
   input replay test for contact/hover/pressure ordering.
6. Check sandbox/container `uinput` permissions and image-based Linux
   distro packaging (including Bazzite) before experimental binaries
   are shared more broadly.

Until these are complete, removing `inputtino` entirely would regress
keyboard, mouse or gamepad functionality.

## Validation

The GitHub workflow compiles Hermes with the new path ON and the legacy
path OFF. This catches compile/link failures, not real digitizer
semantics. Physical S Pen, Krita and Linux desktop behavior remain to
be tested after the code builds.
