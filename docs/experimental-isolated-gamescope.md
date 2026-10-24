# Experimental isolated Gamescope session improvements

This branch is deliberately separate from main. The existing single-desktop streaming path is unchanged.

## Changes

- Standalone DRM Gamescope launches now remove inherited `WAYLAND_DISPLAY`, `WAYLAND_SOCKET`, `_WAYLAND_DISPLAY`, `DISPLAY`, and `HERMES_WAYLAND_DISPLAY` environment variables before starting the compositor. This avoids accidentally nesting under the user's X11/Wayland desktop.
- Optional `HERMES_ISOLATED_GAMESCOPE_STEAM_MODE=1` appends `-e` to the isolated Gamescope launch command.
- Optional `HERMES_ISOLATED_GAMESCOPE_DRM_DEVICE=/dev/dri/cardN` appends `--drm-device /dev/dri/cardN` for **patched Gamescope builds that accept this argument**. Set `HERMES_ISOLATED_GAMESCOPE_DRM_FLAG=--prefer-drm` to use the independently published `relil/gamescope-eGPU` implementation instead of the default `--drm-device`. An invalid non-card path is rejected; this does not itself guarantee GPU rendering or zero-copy.

Set the opt-in variables in the environment of the Hermes **server process** and restart Hermes. They are not per-application settings yet. Leave them unset to preserve the standard Gamescope command.

## Validation needed

1. Confirm isolated sessions launch with both X11 and Wayland host desktops, without inheriting a desktop display socket.
2. With an unpatched upstream Gamescope binary, leave `HERMES_ISOLATED_GAMESCOPE_DRM_DEVICE` unset.
3. With the downstream patched Gamescope, test DRM device selection, Vulkan GPU selection, scanout on Hermes-KMS and DMA-BUF capture separately. Measure any CPU readbacks.
4. Verify `-e` with Steam, and leave it off for non-Steam applications.
5. Validate simultaneous physical-desktop input and remote-session input before changing seatd or libinput defaults.

## Out of scope for this initial commit

Audio-sink routing, microphone forwarding, virtual-input seat assignment, gamepad attachment timing, first-frame deadlines, IPC socket sharing and seatd VT binding require more extensive integration and tests. The source description alone does not establish patches for those behaviors.

Nothing in this branch changes Hermes-KMS or Gamescope itself.

## Public Gamescope patch research

The reported user's exact Gamescope source/patch has **not** been identified. Upstream Gamescope exposes `--prefer-vk-device` and `-O/--prefer-output`, but does not expose `--drm-device` in its documented option table.

A comparable public implementation is [relil/gamescope-eGPU](https://github.com/relil/gamescope-eGPU), which provides the [feature/prefer-drm](https://github.com/relil/gamescope/tree/master) concept through a `--prefer-drm` selector and the `GAMESCOPE_PREFER_DRM` environment variable. Its goal is to separate Vulkan rendering from KMS scanout on a different DRM device.

Example for that patch:

```sh
HERMES_ISOLATED_GAMESCOPE_DRM_FLAG=--prefer-drm
HERMES_ISOLATED_GAMESCOPE_DRM_DEVICE=/dev/dri/card1
```

Ensure the selected device is the Hermes-KMS *primary* node, not the physical GPU render node. Determine the correct card per isolated session; the process-level setting here is only suitable for initial single-session experiments. The patched Gamescope binary must recognize the requested option (`gamescope --help`). Support for `XDG_SEAT` device filtering is separate and **not** included in this patch.

The new option does not itself prove GPU acceleration or a zero-copy capture path: confirm Vulkan device choice, KMS scanout ownership, frame import and synchronization using logs and traces before asserting performance improvements.
