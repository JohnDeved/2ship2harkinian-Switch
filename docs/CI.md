# CI/CD Documentation

## GitHub Actions Workflow

The main CI/CD workflow (`.github/workflows/main.yml`) builds 2Ship2Harkinian for Nintendo Switch using a three-job architecture optimized for parallel execution.

### Workflow Architecture

```
┌─────────────────────┐
│ generate-2ship-otr  │ ──┐
│ (Ubuntu 24.04)      │   │
└─────────────────────┘   │
                          ├──> ┌──────────────────┐
┌─────────────────────┐   │    │ combine-artifacts│
│ build-switch        │ ──┘    │ (Ubuntu Latest)  │
│ (DevKit Container)  │        └──────────────────┘
└─────────────────────┘
```

#### Job 1: generate-2ship-otr
- **Platform**: Ubuntu 24.04
- **Purpose**: Extracts game assets and generates the `2ship.o2r` file
- **Runtime**: ~5-10 minutes (with cache)
- **Key optimizations**:
  - ccache for C/C++ compilation caching (500MB limit)
  - Uses prebuilt distro packages for SDL2 and tinyxml2 (no source bootstrap step)
  - Artifact: `2ship.o2r` (retained for 3 days)

#### Job 2: build-switch
- **Platform**: Ubuntu Latest with devkitpro/devkita64:latest container
- **Purpose**: Cross-compiles the Switch NRO executable
- **Runtime**: ~5-10 minutes (with cache)
- **Runs in parallel** with Job 1 (no dependency)
- **Key optimizations**:
  - ccache for C/C++ compilation caching (1GB limit)
  - Docker container is automatically cached by GitHub Actions
  - Artifact: `2ship-switch-nro` containing NRO, readme, and gamecontrollerdb.txt (retained for 3 days)

#### Job 3: combine-artifacts
- **Platform**: Ubuntu Latest
- **Purpose**: Merges the OTR and NRO artifacts into a single distributable
- **Depends on**: Both Job 1 and Job 2
- **Runtime**: <1 minute
- **Output**: Final `2ship-switch` artifact with all files needed for Switch

### Caching Strategy

#### Build Caches
1. **ccache (OTR build)**:
   - Key: `${{ runner.os }}-otr-{timestamp}`
   - Size limit: 500MB
   - Stores compiled object files from ZAPD and OTR generation

2. **ccache (Switch build)**:
   - Key: `${{ runner.os }}-{timestamp}`
   - Size limit: 1GB
   - Stores compiled object files from Switch cross-compilation

#### Docker Image Caching
- The `devkitpro/devkita64:latest` container is automatically cached by GitHub Actions when using the `container:` key
- No manual Docker layer caching is required

### Performance Characteristics

#### Cold Cache (First Build)
- **generate-2ship-otr**: ~10-15 minutes
- **build-switch**: ~10-15 minutes
- **combine-artifacts**: <1 minute
- **Total wall time**: ~10-16 minutes (parallel execution)

#### Warm Cache (Incremental Build)
- **generate-2ship-otr**: ~3-5 minutes
- **build-switch**: ~3-5 minutes
- **combine-artifacts**: <1 minute
- **Total wall time**: ~3-6 minutes (parallel execution)

### Artifacts

The workflow produces three artifact sets:

1. **2ship.o2r** (intermediate)
   - The OTR archive containing extracted game assets
   - retention: 3 days
   - Size: ~300-400MB

2. **2ship-switch-nro** (intermediate)
   - The compiled Switch executable and supporting files
   - retention: 3 days
   - Size: ~20-30MB

3. **2ship-switch** (final)
   - Combined artifact with all files needed for Switch
   - retention: 3 days
   - Size: ~320-430MB
   - Contents: `2ship.nro`, `2ship.o2r`, `readme.txt`, `gamecontrollerdb.txt`

### Optimization Benefits

Compared to the previous sequential workflow:

1. **Parallel Execution**: OTR generation and Switch compilation now run simultaneously, cutting total build time roughly in half
2. **Faster Dependency Setup**: SDL2 and tinyxml2 come from prebuilt Ubuntu packages, removing custom source build/install steps
3. **Better Resource Utilization**: Both jobs can use full CPU resources without waiting
4. **Cleaner Artifact Management**: Intermediate artifacts are separated, making debugging easier

### Maintenance Notes

- Cache keys use timestamps for incremental versioning
- The OTR job now relies on prebuilt Ubuntu packages for SDL2/tinyxml2 instead of a separate dependency source-build cache
- Docker image pulls are handled automatically by GitHub Actions
- All three jobs must complete successfully for the workflow to succeed
