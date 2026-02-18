# Copilot Agent Instructions — 2 Ship 2 Harkinian (Switch)

## ⚠️ CRITICAL: Do NOT Build

**Do not run CMake builds, `cmake --build`, or `scripts/switch-build.sh` during agent sessions.**
A full build takes **30+ minutes** and wastes runtime costs. Building is handled by the CI pipeline
(`.github/workflows/main.yml`). Validate your changes through other means:

- Read the code carefully and reason about correctness.
- Check that your changes match existing patterns and conventions in surrounding code.
- Use `clang-format` and `clang-tidy` for style and lint checks (see below).
- Run targeted Python scripts if modifying asset extraction tooling.
- Review header includes, type usage, and macro definitions to ensure compatibility.

## Project Overview

This is a C/C++ port of *The Legend of Zelda: Majora's Mask* targeting the **Nintendo Switch**
(and also Windows, Linux, macOS). It is built with **CMake 3.26+** using the **C++20** standard.
The Switch build uses the **devkitPro A64 toolchain** with Ninja.

### Key Directories

| Directory | Purpose |
|---|---|
| `mm/src/` | Decompiled game source (C). Mirror of the zeldaret/mm decomp. |
| `mm/2s2h/` | Port-specific C++ code (enhancements, GUI, randomizer, etc.) |
| `mm/include/` | Game header files |
| `mm/assets/` | Asset XML definitions and custom assets |
| `libultraship/` | Cross-platform abstraction library (git submodule) |
| `OTRExporter/` | Asset exporter tooling (Python + C++) |
| `ZAPDTR/` | Zelda Asset Processor for data extraction |
| `CMake/` | CMake modules and helper scripts |
| `scripts/` | Build automation (e.g., `switch-build.sh` for Docker-based Switch builds) |
| `.github/workflows/` | CI pipelines |
| `docs/` | Build instructions (`BUILDING.md`) |

### Submodules

The `libultraship` directory is a **git submodule**. Do not modify files inside it directly;
changes to that library should go through its own repository.

## Languages

- **C** — Decompiled game code in `mm/src/` and `mm/include/`
- **C++ (C++20)** — Port enhancements in `mm/2s2h/`, libultraship integration
- **CMake** — Build system configuration throughout
- **Python** — Asset extraction scripts (`OTRExporter/extract_assets.py`, `mm/format.py`)
- **Bash / PowerShell** — Build and formatting helper scripts
- **YAML** — GitHub Actions workflows

## Code Style

The project uses **clang-format** (version 14) and **clang-tidy** for enforcing style.
Configuration files are at the repo root:

- **`.clang-format`** — 4-space indentation, 120 column limit, Cpp language, attached braces,
  left-aligned pointers, no tab characters.
- **`.clang-tidy`** — Checks for `readability-braces-around-statements` and
  `readability-inconsistent-declaration-parameter-name`.

### Running Format Checks

```bash
# Format all game source files (requires clang-format-14)
./run-clang-format.sh

# Or use the Python formatter from the mm/ directory
cd mm && python3 format.py -j $(nproc)
```

### Style Rules Summary

- **Indentation**: 4 spaces (no tabs).
- **Column limit**: 120 characters.
- **Braces**: Attach style (opening brace on same line).
- **Pointers**: Left-aligned (`int* ptr`, not `int *ptr`).
- **Includes**: Do not sort (`SortIncludes: false`).
- **Short blocks**: Allowed on a single line; short if/loop/case are not.
- **Naming**: Follow existing conventions — `camelCase` for local variables/functions in C++ port code,
  decomp code in `mm/src/` follows the original decomp naming conventions (e.g., `snake_case` with prefixes).

## CI Pipeline

The CI (`.github/workflows/main.yml`) runs on every push and performs:

1. **`generate-2ship-otr`** — Builds asset tools and generates `2ship.o2r` on Ubuntu 22.04
2. **`build-switch`** — Compiles the Switch NRO using devkitPro container with ccache
3. **`combine-artifacts`** — Merges OTR + NRO into a distributable artifact

The `copilot-setup-steps.yml` workflow pre-configures the build environment for Copilot agents
(installs deps, SDL2, tinyxml2, runs cmake configure). This means the build directory and
CMake cache may be available, but **do not use them to compile** — just use them for reference
if needed.

## Validation Without Building

Since builds are prohibited during agent runs, use these approaches:

1. **Static analysis**: Run `clang-tidy` on changed files if available in the environment.
2. **Pattern matching**: Compare your changes against similar existing code in the same directory.
3. **Header verification**: Ensure all `#include` paths exist and types/macros are defined.
4. **Syntax check**: If clang is available, use `clang -fsyntax-only` on individual files
   (this is fast and does not link).
5. **Review CI results**: After pushing, check the GitHub Actions workflow results to verify
   the build succeeds.

## Common Patterns

### Port Enhancements (mm/2s2h/)

Port-specific code uses `extern "C"` blocks to interface with the C game code:

```cpp
extern "C" {
#include "z64.h"
#include "functions.h"
#include "macros.h"
}
```

CVars (console variables) are used for feature toggles:

```cpp
CVarGetInteger("gEnhancements.Graphics.IncreaseActorDrawDistance", 1);
```

### Game Code (mm/src/)

Decompiled code in `mm/src/` follows the decomp project conventions. When modifying these files:

- Preserve the original decomp structure and naming.
- Use `// 2S2H` comments to mark port-specific additions.
- Wrap port-specific code in `#ifdef` guards or `extern` declarations where appropriate.

### Asset References

Game assets are referenced via string paths from the generated `2s2h_assets.h` header
and related asset headers in `mm/assets/`.

## Platform Considerations

Code may contain platform-specific guards:

```cpp
#if defined(__SWITCH__)
    // Nintendo Switch specific code
#elif defined(__WIIU__)
    // Wii U specific code
#else
    // Desktop platforms
#endif
```

Always consider the Switch target when making changes. The Switch uses:
- **aarch64** architecture
- **OpenGL** for rendering (via devkitPro mesa)
- **SDL2** for input/audio
- Limited memory compared to desktop platforms
