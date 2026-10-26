# Hermes — October 2026 release candidate 2

This candidate fixes native Linux packaging from RC1. The RC1 downloads remain available with their original contents.

## Fixed

- Fedora 43/44: the bundled PyroWave runtime installs under `/usr/lib64/hermes` on x86_64. Hermes uses that absolute install RUNPATH so glibc can resolve the codec while file capabilities enable secure execution. This fixes the pre-startup `libpyrowave-shared.so.0: cannot open shared object file` failure and exit status 127 reported on Fedora 44.
- Ubuntu 24.04/26.04 amd64: the same runtime uses `/usr/lib/x86_64-linux-gnu/hermes` and an absolute RUNPATH, preventing the corresponding secure-loader failure.
- DEBs explicitly require `libcap2-bin`, which supplies the installation's `setcap` command. Capture and isolated-session capabilities are preserved.

## Package validation

Publication requires the host test suite, all native package builds, and a four-distribution secure-loader matrix. Each RPM and DEB is installed, its codec/RUNPATH/capabilities are checked, a codec-linked probe proves `AT_SECURE=1` as a non-root user, and the installed Hermes executable runs `--help` in a clean environment. A negative control reproduces rejection of the old relative ORIGIN path. Portable-bundle relocation is checked separately.

`RC_MANIFEST.json` records the exact source commit, embedded build version, successful workflow and package hashes. `SHA256SUMS` covers the payloads and manifest.

## Install or update

Download the package matching your distribution and release.

Fedora, from the download directory:

```bash
sudo dnf install ./hermes-*.fc44.x86_64.rpm
# Fedora 43: select the fc43 package instead.
systemctl --user daemon-reload
systemctl --user reset-failed hermes.service
systemctl --user restart hermes.service
```

Ubuntu, from the download directory:

```bash
sudo apt install ./hermes_*_ubuntu24.04_amd64.deb
# Ubuntu 26.04: select the ubuntu26.04 package instead.
systemctl --user daemon-reload
systemctl --user reset-failed hermes.service
systemctl --user restart hermes.service
```

RC2 packages use `0.6.0+rc2.<commit>` to sort after RC1's commit-only version. Do not remove capabilities or set LD_LIBRARY_PATH as a workaround.

## Compatible components and remaining testing

Use the existing [Hestia RC1](https://github.com/MrOz59/Hestia/releases/tag/october-2026-rc1), [Hestia-Android RC1](https://github.com/MrOz59/Hestia-Android/releases/tag/october-2026-rc1) and [Hermes-KMS RC1](https://github.com/MrOz59/Hermes-KMS/releases/tag/october-2026-rc1). This host packaging update does not require rebuilding those components.

Real-host capture/encode, desktop and Android streaming, HDR with the existing codecs, reconnects, DS4/DualSense, isolated Gamescope sessions and private audio/microphone remain RC hardware tests. PyroWave remains an explicit SDR YUV 4:2:0 LAN option. Report the RC tag, embedded build version, distribution, GPU/driver/kernel and logs from both ends.
