# Linux input backend: MrOz59/libvirtualhid

**Status: integration branch.** `experimental/libvirtualhid-input` uses
libvirtualhid for the complete Linux virtual-input stack. Inputtino is not
compiled or linked.

## Dependency provenance

The dependency is our fork,
[MrOz59/libvirtualhid](https://github.com/MrOz59/libvirtualhid), pinned by CMake
to commit `1831bc0d5b43b23ba694d397c8a9d4900186521c`. Updating the dependency
requires reviewing and intentionally changing that SHA.

The library uses C++23, Linux `uinput`/`uhid`, and libevdev. Hermes needs read
and write access to `/dev/uinput` and `/dev/uhid`; applications consuming
controller output reports also need the generated `hidraw` node. The packaged
`60-hermes.rules` grants those permissions. Never use
`lvh::BackendKind::fake` in production: it accepts events without creating
kernel devices.

## Integrated paths

| Path | libvirtualhid implementation |
| --- | --- |
| Relative and absolute mouse, five buttons, horizontal and vertical high-resolution scroll | `lvh::Mouse` |
| Keyboard and UTF-8 text | `lvh::Keyboard` |
| Generic HID, Xbox 360/One/Series, DualShock 4, DualSense and Switch Pro controllers | `lvh::GamepadStateAdapter` |
| Controller touchpad, motion and battery | cached `lvh::GamepadState` |
| Rumble, trigger rumble, RGB LED and adaptive triggers | `lvh::GamepadOutput` callback |
| Multitouch, hover and cancel-all | `lvh::Touchscreen` |
| Pen, eraser, pressure, distance, tilt and three barrel buttons | `lvh::PenTablet` |

Hermes still owns GameStream packet parsing, client permissions, keyboard
repeat and feedback messages. The adapter translates those contracts into
libvirtualhid state and reports.

## Configuration and client negotiation

The Hermes web UI's Input tab selects the emulated controller. With `gamepad =
auto`, PlayStation, Nintendo and Xbox client types select DualSense, Switch Pro
and Xbox Series respectively. For an unknown client type, `motion_as_ds4` and
`touchpad_as_ds4` can select DualSense; otherwise Xbox Series is used. A manual
profile always overrides this detection. `xone` remains available explicitly.
`linux_gamepad_backend = auto` uses uinput for generic/Xbox controllers,
preserving ordinary rumble without requiring games to open hidraw. PlayStation
and Switch retain native HID. Choose `native` explicitly for Xbox trigger rumble
and battery, with appropriately restricted hidraw access for the consuming game.
The `uinput` policy restricts automatic selection to Xbox Series and rejects
manual PlayStation/Switch profiles; it does not silently change a manual profile.
Back/Select can click the virtual DS4/DS5 touchpad using
`ds4_back_as_touchpad_click`. `virtualhid_randomize_mac` controls both
PlayStation profiles and accepts the two older DS5-specific configuration keys.

Mouse/keyboard/controller switches gate their input paths. Native pen/touch
advertisement also requires `mouse`, `native_pen_touch` and an available backend.
Native packets are rejected when disabled, except cancel-all cleanup. On the
Android client, MultiTouch sends touchscreen input; absolute/trackpad modes
send mouse input. Pen input follows its own tool-aware path. No client mode
turns missing hardware pressure or motion sensors into real measurements.

Stopping a stream releases mapped keys, all five mouse buttons, gamepad
buttons/axes and active touch/pen state only on its owning input context.
Isolated contexts are not retained after their seat/compositor is destroyed.
Sunshine's global retained-gamepad lifecycle must not be copied across private
seats; a new isolated stream gets a new context.

## Isolated sessions

Every isolated stream receives its own `platf::input_t`, libvirtualhid runtime,
mouse, keyboard and controller collection. Pen and touchscreen state remains
per client. All devices carry the matching `hermes-kms-N` physical identity:

- uinput devices expose it through `UI_SET_PHYS`;
- UHID controllers expose it through `HID_PHYS`;
- node discovery requires an exact physical-identity match; and
- an isolated keyboard or mouse never falls back to process-global XTest.

The packaged udev rule maps that identity to the private `ID_SEAT`. UHID
controller drivers create their input children two levels below the HID device
and may clear `input/phys`, so the rule propagates the seat from the HID parent
to every `input`, `event`, `js` and `hidraw` child. Device names are not used as
the isolation boundary. DualSense MACs are randomized by default; deterministic
mode includes the private session identity for both DS4 and DS5 so two seats cannot alias each
other. Switch Pro initialization also advertises a distinct, session-scoped
MAC rather than the formerly fixed protocol MAC.

Session teardown destroys only that session's runtime and kernel handles. The
shared desktop continues to use an input object with an empty session tag.

## Building

```bash
git clone --recursive https://github.com/MrOz59/Hermes.git
cd Hermes
git switch experimental/libvirtualhid-input
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target sunshine -j2
```

The first configure fetches the pinned fork revision. There is no backend
selection option: Linux builds use libvirtualhid exclusively.

## Validation

The branch workflow compiles Hermes and runs the library's identity/discovery,
UHID round-trip and fail-closed isolation tests. On a host with uinput/uhid,
the library consumer suite also verifies evdev, libinput, SDL, touchscreen,
trackpad and tablet behavior.

Pure unit tests run without kernel device prerequisites. A separate disposable
runner job loads uinput/uhid and runs the real-device smoke tests, per-seat
routing tests and an SDL consumer under an unprivileged UID. That consumer
requires an ACL for its own event node, rejects access to the foreign seat,
and must produce ordinary rumble without any hidraw node. The test grants ACLs
only inside the disposable guest; it does not prove broker provisioning of
those ACLs in production.

SDL versions can expose Xbox directional inputs as raw buttons or a hat; the
tests verify canonical button transitions independently of that layout. The
DualSense kernel-child discovery test requires the HID device to be bound to
`hid-playstation`. It explicitly reports a skip when a runner binds another
driver instead; that skip does not validate motion/touchpad child discovery.
The local VM validation below used `hid-playstation` and ran that test.

The repository's `scripts/vm-isolated-input-test.sh` validates the packaged
udev rules inside a disposable VM. Pass `build/tests/test_sunshine` to also
exercise the actual Hermes adapter with two concurrent private seats, including
seat 12 to cover dynamically allocated indices beyond the old eight-seat pool:

```bash
cmake -B build -G Ninja -DBUILD_TESTS=ON
cmake --build build --target sunshine test_sunshine -j2
# Inside a disposable guest, as root with udev running:
scripts/vm-isolated-input-test.sh /absolute/path/to/build/tests/test_sunshine
```

Library baseline validation on 2026-10-24 passed 193 tests (one xpadneo-specific
test skipped). The expanded adapter validation passed 37 local tests covering
input translation, reset, configuration, locales, coordinates and schema, and
all 25 tests in the VM adapter run. The VM run exercised Generic HID,
Xbox 360/One/Series, DualShock 4, DualSense and Switch Pro with
independent keyboard, relative/absolute mouse, touch and pen devices on
`hermes-kms-1` and `hermes-kms-12`. For each controller profile it replayed
1000 batches at a target rate of 1000 Hz; each batch contained keyboard, mouse,
touchscreen, pen and controller input. There were no evdev `SYN_DROPPED`
events, all 1000 keyboard transitions and relative moves arrived, and the
checked input events did not appear on the other session. Teardown removed
only the first session's devices; the surviving session still accepted input.

These tests verify kernel input routing and lifetime, not an entire streamed
desktop or applications running under broker-created Unix accounts. Such
applications still require device access appropriate to their account; seatd
access for a compositor does not itself grant hidraw access to a game. The
container profile deliberately excludes hidraw, so native HID controller
feedback and advanced features need separate container validation before this
branch is promoted to a release.

Physical pen hardware is not required for the synthetic pen/tablet tests. It
is still useful before a release for validating device-specific Android stylus
quirks, desktop applications such as Krita, and end-to-end feel. Likewise,
real concurrent clients remain the final integration test for compositor,
audio and lifecycle behavior outside the input library itself.
