# Experimental NVIDIA capture

Branch: `experimental/nvidia-zero-copy`. This is development work, without a
claim of validated end-to-end zero-copy on NVIDIA hardware.

## First implementation

Host frames now upload with `cudaMemcpy2DToArrayAsync` on the conversion's
CUDA stream. The encoder keeps a shared reference until an upload-completion
event has signalled, preventing the capture pool from recycling the source
while DMA reads it. Stack-backed dummy frames still upload synchronously.
Teardown drains the stream before releasing the image and CUDA array.

The existing pinned allocations and mapping/copy caches are preserved. This
change removes the synchronous upload API from pooled frames; it does not
remove the CPU copy or promise overlap on pageable-memory fallbacks.

For an A/B comparison, launch Hermes with `HERMES_CUDA_ASYNC_UPLOAD=0` to use
the previous synchronous upload. Otherwise this branch uses asynchronous
uploads. Compare capture-to-bitstream latency and frame age, not just FPS.

## Opt-in DMA-BUF experiment

`HERMES_KMS_EXPERIMENTAL_NVENC_DMABUF=1` selects the existing EGL/GL/CUDA
consumer for Hermes-KMS instead of forcing CPU-copy capture. It is explicitly
opt-in because system-memory imports have produced corruption beyond about
2 MiB and driver teardown crashes on a tested NVIDIA stack.

Use only on a test session after running the full-frame checker from the
matching experimental Hermes-KMS branch. The current CUDA backend selects
device 0; start with one NVIDIA GPU. `HERMES_KMS_FORCE_CPU_COPY=1` takes
precedence. A failed DMA-BUF initialization does not silently switch to a
physical monitor. Unset the experimental variable to restore the default.

This route still converts into GL Y/UV targets and copies them into the CUDA
encoder frame. It can avoid host access to pixels only when the compositor
also supplies GPU-resident buffers. KWin may still perform a CPU readback
before Hermes sees the scanout. Modifiers alone do not prove otherwise.

## Validation and next steps

The standalone ownership regression runs without a GPU:

```sh
c++ -std=c++17 -Wall -Wextra -Werror -pedantic -I. \
  tests/standalone/cuda-upload-lifetime.cc -o /tmp/cuda-upload-lifetime
/tmp/cuda-upload-lifetime
```

Branch CI also compiles the CUDA-enabled host. Neither a headless build nor
the ownership regression establishes hardware correctness or performance.
Real-host checks still need SDR/HDR, padded pitches, resize, repeated
reconnection, multi-client teardown, and a game near GPU saturation.

## GPU-origin producer now available

The matching Hermes-KMS experimental branch includes
`tools/hermes-gpu-scanout-test/hermes-gpu-scanout-test`. It allocates a modified
GBM BO on an explicit GPU, renders a page-varying pattern without CPU pixel
upload, imports it into an unused Hermes-KMS card, and requires capture to
return the original DMA-BUF identity and layout. It checks every source and
returned pixel, including the suffix beyond 2 MiB, and performs real GL/EGL
teardown. It does not change the compositor or Hermes capture defaults.

Follow the [producer's build/run instructions](https://github.com/MrOz59/Hermes-KMS/blob/experimental/nvidia-zero-copy/tools/hermes-gpu-scanout-test/README.md).
Its `--hold-seconds` and private `--session-file` let the CUDA-enabled
`hermes-egl-import-check --no-cpu-ref --full-teardown` inspect the same static
GPU-origin source. Use the same explicit NVIDIA render node for both. The
producer owns the output; do not launch another compositor that tries to own
that card while this diagnostic is running.

Passing the producer establishes storage passthrough and known-pattern pixel
correctness. Passing the CUDA checker adds GL/CUDA image and lifecycle checks.
Neither proves physical VRAM residency, NVENC encode/decode or KWin's choice
of scanout allocation. Hardware results are still pending. The next runtime
steps are compositor passthrough, selected-device propagation, import caching
and direct YUV output after the GPU-origin experiment passes on the real host.
