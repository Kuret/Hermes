# Hermes — October 2026 release candidate 1

Coordinated RC for testing before the end-of-October stable release. This is a **prerelease**, not a stable release.

- Source commit: [`964c5fa24b4992f942a59142e82b3adee8214de9`](https://github.com/MrOz59/Hermes/commit/964c5fa24b4992f942a59142e82b3adee8214de9)
- Validated build: [Actions run 40265406947](https://github.com/MrOz59/Hermes/actions/runs/40265406947)
- Embedded build version: `0.6.0+964c5fa`

This is an immutable promotion of the exact successful CI artifacts. Package/APK bytes, embedded build versions, filenames and signatures are preserved; the shared `october-2026-rc1` tag identifies the coordinated candidate. `RC_MANIFEST.json` records each source/build/artifact identity, and `SHA256SUMS` covers all attached payload files.

## Included

- PyroWave encoding and its privately bundled runtime; select PyroWave explicitly with a compatible Hestia client. The candidate path is SDR YUV 4:2:0.
- libvirtualhid input integration and the isolated Gamescope session work from PR #73.
- Arch/CachyOS package, Ubuntu 24.04/26.04 DEBs and Fedora 43/44 RPMs. Use the package matching your distribution.
- Includes the socket lifecycle test fix and the corrected Bash DEB packaging step. The complete build, input, private-audio and network workflows passed.

For isolated sessions, use the [Hermes Gamescope fork](https://github.com/MrOz59/gamescope) and its documented setup. This RC does not establish NVIDIA zero-copy support.

## Matching candidates

- [Hermes RC1](https://github.com/MrOz59/Hermes/releases/tag/october-2026-rc1) — `964c5fa24b4992f942a59142e82b3adee8214de9`
- [Hestia RC1](https://github.com/MrOz59/Hestia/releases/tag/october-2026-rc1) — `94a1bc58b510f7417bd208ed70cd22d2b89ed649`
- [Hestia-Android RC1](https://github.com/MrOz59/Hestia-Android/releases/tag/october-2026-rc1) — `0ef14edfd152b4d220d3f2992c82c101e582ca44`
- [Hermes-KMS RC1](https://github.com/MrOz59/Hermes-KMS/releases/tag/october-2026-rc1) — `8c8aa0812783437c91e6d65ff8073fdbe57068b2`

## Remaining validation

Real-host AMD capture/encode, desktop and Android streaming, SDR/HDR, disconnect/reconnect, concurrent isolated Gamescope sessions, DS4/optional DualSense, private audio/microphone and upgrade/rollback checks remain part of RC testing. Passing CI does not record those hardware tests as completed. PyroWave HDR is outside the supported candidate path; use the existing codecs for HDR.

When reporting a problem, include this RC tag, component build version, GPU/driver/kernel, client device and codec, and logs from both ends.
