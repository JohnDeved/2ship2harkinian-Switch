# Copilot Agent Instructions — 2 Ship 2 Harkinian (Switch)

## ⚠️ CRITICAL: Do NOT Build

**Do not run CMake builds, `cmake --build`, or `scripts/switch-build.sh` during agent sessions.**
A full build takes 30+ minutes. Building is handled by CI (`.github/workflows/main.yml`). Instead:

- Reason about correctness by reading the code and matching existing patterns.
- Use `clang-format` and `clang-tidy` for style/lint checks.
- Use `clang -fsyntax-only` for fast syntax-only checks on individual files.
- After pushing, verify via GitHub Actions workflow results.

## Project Overview

C/C++ port of *The Legend of Zelda: Majora's Mask* targeting **Nintendo Switch** (plus Windows, Linux, macOS).
Built with **CMake 3.26+**, **C++20**, Switch uses the **devkitPro A64 toolchain** with Ninja.

## Code Style

Uses **clang-format** and **clang-tidy** (config at repo root). Run format checks with:

```bash
./run-clang-format.sh
# or
cd mm && python3 format.py -j $(nproc)
```

Key rules: 4-space indent, 120-column limit, attached braces, left-aligned pointers (`int* ptr`), no sorted includes.
`camelCase` in C++ port code; decomp naming conventions in `mm/src/`.

## Common Patterns

### Port Enhancements (`mm/2s2h/`)

Use `extern "C"` to interface with game C code, and CVars for feature toggles:

```cpp
extern "C" {
#include "z64.h"
#include "functions.h"
}
CVarGetInteger("gEnhancements.Graphics.IncreaseActorDrawDistance", 1);
```

### Game Code (`mm/src/`)

- Preserve original decomp structure and naming.
- Mark port-specific additions with `// 2S2H` comments.
- Use `s32` for boolean flags in C code, `bool` in C++ code.

## Platform Considerations

```cpp
#if defined(__SWITCH__)
    // Nintendo Switch (aarch64, OpenGL via SDL2)
#elif defined(__WIIU__)
    // Wii U
#else
    // Desktop platforms
#endif
```

### Switch Hardware Specs

| Component | Specification |
|-----------|--------------|
| **SoC** | NVIDIA Tegra X1 |
| **CPU** | 4× ARM Cortex-A57 @ 1020 MHz (max 1785 MHz with overclock), 32 KB L1D/core, 2 MB shared L2 |
| **GPU** | Maxwell, 256 CUDA cores — Docked: 768 MHz, Handheld: 307–384 MHz |
| **Memory** | 4 GB LPDDR4 @ 1600 MHz (25.6 GB/s, **shared** CPU+GPU) |
| **Graphics API** | OpenGL ES 3.2 via NVN compatibility layer |
| **Build** | AArch64, `-O3 -ffast-math`, devkitPro toolchain |

Performance profiles (`gSwitchPerfMode`): MAXIMUM (1785 MHz) → HIGH → BOOST → STOCK (1020 MHz) → POWERSAVINGM1–M3 (714 MHz).

### Switch Optimization Guidelines

- **Minimize draw calls**: The Maxwell GPU has significant per-draw overhead. Batch geometry to reduce `glDrawArrays` calls.
- **Avoid runtime shader compilation**: Shader compilation on Maxwell is very expensive. Pre-warm all variants at load time.
- **Conserve shared memory bandwidth**: CPU and GPU share the 25.6 GB/s LPDDR4 bus. Minimize large texture uploads during active rendering.
- **Use NEON SIMD**: The A57 has 128-bit NEON (4× f32 or 16× u8 lanes). Key intrinsics: `vmulq_n_f32`/`vmlaq_n_f32` for multiply-accumulate, `vst4_u8` for interleaved stores, `vrev16q_u8` for endian swap.
- **Respect the L1 cache**: 32 KB L1D per core. Keep hot data structures small and access patterns sequential.
- **Use worker cores**: Core 0 is the bottleneck (~100% load). Offload parallel work (collision OC, effects) to cores 1 and 3 via the `TaskWorkerPool`.
- **Beware thermal throttling**: Sustained 100% Core 0 load triggers GPU clock reduction after minutes of gameplay. Spread work across cores.
