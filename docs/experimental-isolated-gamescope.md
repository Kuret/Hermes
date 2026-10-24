# Experimental isolated Gamescope session improvements

This branch is deliberately separate from main. The existing single-desktop streaming path is unchanged.

## Changes

- Standalone DRM Gamescope launches now remove inherited `WAYLAND_DISPLAY`, `WAYLAND_SOCKET`, `_WAYLAND_DISPLAY`, `DISPLAY`, and `HERMES_WAYLAND_DISPLAY` environment variables before starting the compositor. This avoids accidentally nesting under the user's X11/Wayland desktop.
- Optional `HERMES_ISOLATED_GAMESCOPE_STEAM_MODE=1` appends `-e` to the isolated Gamescope launch command.
- Optional `HERMES_ISOLATED_GAMESCOPE_DRM_DEVICE=/dev/dri/cardN` appends `--drm-device /dev/dri/cardN` for **patched Gamescope builds that accept this argument**. An invalid non-card path is rejected; this does not itself guarantee GPU rendering or zero-copy.

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
