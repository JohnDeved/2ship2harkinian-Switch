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

| Directory | Purpose |
|---|---|
| `mm/src/` | Decompiled game source (C) — mirrors zeldaret/mm decomp |
| `mm/2s2h/` | Port-specific C++ code (enhancements, GUI, randomizer, etc.) |
| `mm/include/` | Game header files |
| `mm/assets/` | Asset XML definitions and custom assets |
| `libultraship/` | Cross-platform abstraction library |
| `OTRExporter/` | Asset exporter tooling (Python + C++) — git submodule |
| `ZAPDTR/` | Zelda Asset Processor for data extraction — git submodule |
| `CMake/` | CMake modules and helper scripts |
| `scripts/` | Build automation (`switch-build.sh` for Docker-based Switch builds) |
| `docs/` | Build instructions (`BUILDING.md`) |

## Code Style

Uses **clang-format** and **clang-tidy** (config at repo root). Run format checks with:

```bash
./run-clang-format.sh
# or
cd mm && python3 format.py -j $(nproc)
```

Key rules: 4-space indent, 120-column limit, attached braces, left-aligned pointers (`int* ptr`), no sorted includes.
`camelCase` in C++ port code; decomp naming conventions in `mm/src/`.

## CI Pipeline

`.github/workflows/main.yml` runs on every push:

1. **`generate-2ship-otr`** — Builds asset tools and generates `2ship.o2r` (Ubuntu 24.04)
2. **`build-switch`** — Compiles Switch NRO via devkitPro container with ccache
3. **`combine-artifacts`** — Merges OTR + NRO into a distributable artifact

`copilot-setup-steps.yml` installs apt dependencies and configures ccache for agent environments.

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

### Asset References

Assets are referenced via string paths from `mm/assets/2s2h_assets.h` and headers in `mm/assets/`.

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

The Switch target uses **aarch64**, **OpenGL** via SDL2, and has limited memory compared to desktop.
