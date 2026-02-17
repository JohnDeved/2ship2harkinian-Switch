# GitHub Actions CI/CD

## Workflow Overview

The main GitHub Actions workflow (`generate-builds`) has been optimized for faster build times through parallelization and improved caching strategies.

## Architecture

The workflow consists of three jobs that run in the following order:

```
build-otr (Linux)  ──┐
                     ├──> combine-artifacts
build-switch (NX)  ──┘
```

### Job: `build-otr`
- **Purpose**: Builds ZAPD (the asset extraction tool) and generates the `2ship.o2r` file containing custom assets
- **Runner**: Ubuntu Linux (native, not containerized)
- **Parallelization**: Runs in parallel with `build-switch`
- **Outputs**: `2ship.o2r` artifact

**Key optimizations:**
- Uses ccache with content-based cache keys (`hashFiles()` on CMakeLists.txt and asset paths)
- Builds only the `Generate2ShipOtr` target (not the full game)
- Uses all available CPU cores with `$(nproc)`

### Job: `build-switch`
- **Purpose**: Cross-compiles the game for Nintendo Switch
- **Runner**: Ubuntu Linux with `devkitpro/devkita64:latest` container
- **Parallelization**: Runs in parallel with `build-otr`
- **Outputs**: `2ship.nro` (Nintendo Switch executable)

**Key optimizations:**
- Uses ccache with content-based cache keys for better hit rates
- Docker container image is automatically cached by GitHub Actions
- Builds only the `2ship_nro` target (not ZAPD or OTR generation)

### Job: `combine-artifacts`
- **Purpose**: Combines the outputs from both parallel jobs into a single distributable artifact
- **Runner**: Ubuntu Linux (lightweight, no container needed)
- **Dependencies**: Waits for both `build-otr` and `build-switch` to complete
- **Outputs**: Final `2ship-switch` artifact containing:
  - `2ship.nro` (Switch executable)
  - `2ship.o2r` (custom assets)
  - `readme.txt` (documentation)
  - `gamecontrollerdb.txt` (controller mappings)

## Performance Improvements

### 1. Parallelization
Previously, builds ran sequentially. Now:
- OTR generation and Switch compilation run simultaneously
- Total wall-clock time reduced to `max(otr_time, switch_time)` instead of `otr_time + switch_time`

### 2. Improved Cache Keys
Old cache key: `${{ runner.os }}` (too broad, many unnecessary rebuilds)

New cache keys:
- OTR build: `${{ runner.os }}-otr-${{ hashFiles('**/CMakeLists.txt', 'ZAPDTR/**', 'OTRExporter/**') }}`
- Switch build: `${{ runner.os }}-switch-${{ hashFiles('**/CMakeLists.txt', 'libultraship/**', 'mm/**') }}`

Benefits:
- Cache invalidation only when relevant files change
- Separate caches for OTR and Switch builds prevent conflicts
- Better cache hit rates = faster builds

### 3. Docker Container Caching
- The `devkitpro/devkita64:latest` container image is automatically cached by GitHub Actions
- No manual Docker layer management needed when using the `container:` key

## Why This Architecture?

### Separation of OTR and NRO Builds

**ZAPD cannot be cross-compiled for Switch** because it:
- Uses host-only tools (Python, native executables)
- Requires x86_64 Linux environment for asset extraction
- Depends on libraries not available in the Switch SDK

Therefore, we must:
1. Build ZAPD and generate OTR on a native Linux host
2. Build the Switch NRO in a cross-compilation environment
3. Combine the outputs afterward

### Benefits of Parallel Execution

Assuming approximate build times:
- OTR generation: ~5-8 minutes (asset extraction)
- Switch compilation: ~10-15 minutes (full game build)

**Sequential (old)**: 15-23 minutes total
**Parallel (new)**: 10-15 minutes total (40-50% time savings)

## Artifact Management

The workflow uses short-lived intermediate artifacts:
- `2ship-otr` and `2ship-nro` have 1-day retention
- Final `2ship-switch` artifact uses default retention (90 days)

This approach:
- Keeps intermediate artifacts only long enough for the combine step
- Saves GitHub storage space
- Maintains the final release artifact for downloads

## Maintenance Notes

### Updating Cache Keys

When adding new dependencies or changing build configuration:

1. **For OTR builds**: Update the `hashFiles()` pattern in `build-otr` job
   ```yaml
   key: ${{ runner.os }}-otr-${{ hashFiles('**/CMakeLists.txt', 'ZAPDTR/**', 'OTRExporter/**', 'NEW_PATH/**') }}
   ```

2. **For Switch builds**: Update the `hashFiles()` pattern in `build-switch` job
   ```yaml
   key: ${{ runner.os }}-switch-${{ hashFiles('**/CMakeLists.txt', 'libultraship/**', 'mm/**', 'NEW_PATH/**') }}
   ```

### Troubleshooting

**If OTR generation fails:**
- Check that ZAPD builds successfully on Linux
- Verify Python dependencies are installed
- Check asset extraction logs

**If Switch build fails:**
- Verify devkitPro toolchain is up to date
- Check ccache logs (last 50 lines are printed)
- Ensure Switch-specific dependencies are available

**If combine step fails:**
- Verify both upstream jobs completed successfully
- Check artifact names match exactly
- Ensure artifacts were uploaded correctly
