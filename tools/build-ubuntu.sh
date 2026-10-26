#!/usr/bin/env bash
# Source stays on the host; build tools and intermediates stay in OrbStack Ubuntu.
set -euo pipefail
PROJECT_ID=leo-radio
KIND=idf
DEFAULT_VARIANT=default
VARIANTS='default'
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
if [[ ${1:-} == --help || ${1:-} == -h ]]; then
    printf 'Usage: tools/build-ubuntu.sh [variant]\nVariants: %s\nDefault: %s\nOrbStack: embedded / johnson (override EMBEDDED_MACHINE / EMBEDDED_USER)\n' "$VARIANTS" "$DEFAULT_VARIANT"
    exit 0
fi
[[ $# -le 1 ]] || { echo 'Expected at most one variant.' >&2; exit 2; }
VARIANT=${1:-$DEFAULT_VARIANT}
case "$VARIANT" in
    default) ;;
    *) printf 'Unknown build variant: %s\nAllowed: %s\n' "$VARIANT" "$VARIANTS" >&2; exit 2 ;;
esac
JOBS=${BUILD_JOBS:-4}
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || { echo 'BUILD_JOBS must be a positive integer.' >&2; exit 2; }
case "$(uname -s)" in
    Darwin)
        command -v orb >/dev/null || { echo 'OrbStack CLI is required.' >&2; exit 1; }
        exec orb -m "${EMBEDDED_MACHINE:-embedded}" -u "${EMBEDDED_USER:-johnson}" \
            env BUILD_JOBS="$JOBS" bash "/mnt/mac$ROOT/tools/build-ubuntu.sh" "$VARIANT"
        ;;
    Linux) ;;
    *) echo 'Use macOS + OrbStack or the configured Ubuntu machine.' >&2; exit 1 ;;
esac

CHECKOUT=$(printf '%s' "$ROOT" | sha256sum | cut -c1-10)
BUILD="$HOME/build/$PROJECT_ID/$CHECKOUT/$VARIANT"
DEST="$ROOT/.ubuntu-build/$VARIANT"
mkdir -p "$BUILD" "$DEST"
# A failed build must not leave a previous success receipt looking current.
rm -f "$DEST/build-info.json"
case "$KIND" in
    arm)
        export PATH="$HOME/toolchains/arm-gnu-toolchain-13.3.rel1-aarch64-arm-none-eabi/bin:$PATH"
        command -v arm-none-eabi-gcc >/dev/null || { echo 'Install ARM GCC 13.3.rel1 in ~/toolchains first.' >&2; exit 1; }
        # The toolchain appends CMake flags, so match the original --fresh workflow.
        cmake --fresh -S "$ROOT" --preset "$VARIANT" -B "$BUILD"
        cmake --build "$BUILD" --parallel "$JOBS"
        ;;
    idf)
        SUBDIR=.
        CONFIG=sdkconfig
        TARGET=
        TARGET=esp32c3
        SOURCE="$ROOT/$SUBDIR"
        export IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf}"
        export IDF_TOOLS_PATH="${IDF_TOOLS_PATH:-$HOME/.espressif}"
        export IDF_PYTHON_ENV_PATH="${IDF_PYTHON_ENV_PATH:-$IDF_TOOLS_PATH/python_env/idf5.5_py3.12_env}"
        [[ -f "$IDF_PATH/export.sh" && -x "$IDF_PYTHON_ENV_PATH/bin/python" ]] || {
            echo 'ESP-IDF v5.5.4 and its Linux Python 3.12 environment must be installed.' >&2; exit 1;
        }
        set +u
        source "$IDF_PATH/export.sh" >/dev/null
        set -u
        # Never regenerate the host's sdkconfig while switching host OS/toolchain.
        INPUT="$SOURCE/$CONFIG"
        if [[ -f "$INPUT" ]]; then
            INPUT_HASH=$(sha256sum "$INPUT" | cut -d' ' -f1)
            if [[ ! -f "$BUILD/sdkconfig.input.sha256" ]] || [[ $(cat "$BUILD/sdkconfig.input.sha256") != "$INPUT_HASH" ]]; then
                cp "$INPUT" "$BUILD/sdkconfig"
                printf '%s\n' "$INPUT_HASH" > "$BUILD/sdkconfig.input.sha256"
            fi
        fi
        LOCK_INPUT="$SOURCE/dependencies.lock"
        if [[ "$PROJECT_ID" == esp32-radio-os && "$TARGET" != esp32s3 ]]; then
            LOCK_INPUT="$SOURCE/dependencies.lock.$TARGET"
        fi
        if [[ -f "$LOCK_INPUT" ]]; then
            LOCK_HASH=$(sha256sum "$LOCK_INPUT" | cut -d' ' -f1)
            if [[ ! -f "$BUILD/dependencies.input.sha256" ]] || [[ $(cat "$BUILD/dependencies.input.sha256") != "$LOCK_HASH" ]]; then
                cp "$LOCK_INPUT" "$BUILD/dependencies.lock"
                printf '%s\n' "$LOCK_HASH" > "$BUILD/dependencies.input.sha256"
            fi
        fi
        export UBUNTU_DEPENDENCIES_LOCK="$BUILD/dependencies.lock"
        cat > "$BUILD/ubuntu-lock.cmake" <<'CMAKE'
if(COMMAND idf_build_set_property)
    set(DEPENDENCIES_LOCK "$ENV{UBUNTU_DEPENDENCIES_LOCK}")
    idf_build_set_property(DEPENDENCIES_LOCK "${DEPENDENCIES_LOCK}")
endif()
CMAKE
        cd "$SOURCE"
        # Configure with IDF, then use CMake to explicitly limit build parallelism.
        python "$IDF_PATH/tools/idf.py" -B "$BUILD" -D "SDKCONFIG=$BUILD/sdkconfig" -D "IDF_TARGET=$TARGET" \
            -D "CMAKE_PROJECT_INCLUDE_BEFORE=$BUILD/ubuntu-lock.cmake" reconfigure
        cmake --build "$BUILD" --parallel "$JOBS"
        ;;
    pio)
        PIO="$HOME/.venvs/platformio/bin/pio"
        [[ -x "$PIO" ]] || { echo 'Install the Linux PlatformIO venv in ~/.venvs/platformio first.' >&2; exit 1; }
        export PLATFORMIO_BUILD_DIR="$BUILD"
        export PLATFORMIO_LIBDEPS_DIR="$HOME/.cache/platformio-libdeps/$PROJECT_ID/$CHECKOUT"
        if [[ ! -d "$PLATFORMIO_LIBDEPS_DIR" && -d "$ROOT/.pio/libdeps" ]]; then
            mkdir -p "$(dirname "$PLATFORMIO_LIBDEPS_DIR")"
            cp -a "$ROOT/.pio/libdeps" "$PLATFORMIO_LIBDEPS_DIR"
        fi
        if [[ "$VARIANT" == test-native ]]; then
            "$PIO" test --project-dir "$ROOT" --environment native
        else
            "$PIO" run --project-dir "$ROOT" --environment "$VARIANT" --jobs "$JOBS"
            BUILD="$BUILD/$VARIANT"
        fi
        ;;
esac

python3 - "$PROJECT_ID" "$VARIANT" "$ROOT" "$BUILD" "$DEST" "$KIND" <<'PY'
from pathlib import Path
import hashlib, json, os, platform, re, shutil, subprocess, sys, tempfile, uuid
project, variant, root, build, dest, kind = sys.argv[1:]
build, dest = Path(build), Path(dest)
if variant == 'test-native':
    names = []
    required = []
elif kind == 'idf':
    description = json.loads((build/'project_description.json').read_text())
    app = description['project_name']
    required = [description['app_bin'], description['app_elf']]
    names = required + [app+'.map', 'bootloader/bootloader.bin', 'partition_table/partition-table.bin', 'ota_data_initial.bin', 'flasher_args.json', 'flash_args']
elif kind == 'arm':
    cache = (build/'CMakeCache.txt').read_text()
    match = re.search(r'^TARGET:[^=]+=(.+)$', cache, re.MULTILINE)
    app = match.group(1).strip() if match else 'firmware'
    required = [app+'.bin', app+'.elf']
    names = required + [app+'.hex', app+'.map']
else:
    required = ['firmware.bin', 'firmware.elf']
    names = required + ['firmware.map', 'bootloader.bin', 'partitions.bin']
for name in names:
    if Path(name).is_absolute() or '..' in Path(name).parts:
        raise SystemExit('Unexpected output path in build metadata.')
for name in required:
    if not (build/name).is_file():
        raise SystemExit(f'Missing expected current output: {name}')
files = [build/name for name in names if (build/name).is_file()]
stage = Path(tempfile.mkdtemp(prefix='.'+dest.name+'-export-', dir=dest.parent))
hashes = {}
for source in files:
    relative = source.relative_to(build)
    target = stage / relative
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, target)
    digest = hashlib.sha256(source.read_bytes()).hexdigest()
    if hashlib.sha256(target.read_bytes()).hexdigest() != digest:
        raise SystemExit(f'Artifact copy verification failed: {relative}')
    hashes[str(relative)] = digest
commit = subprocess.check_output(['git','-C',root,'rev-parse','HEAD'], text=True).strip()
receipt = {'project':project, 'variant':variant, 'kind':kind, 'source':root,
           'build':str(build), 'commit':commit, 'host_arch':platform.machine(),
           'built_at_utc':__import__('datetime').datetime.now(__import__('datetime').timezone.utc).isoformat(),
           'artifacts':hashes, 'device_tested':False,
           'tracked_changes_sha256':hashlib.sha256(subprocess.check_output(['git','-C',root,'diff','HEAD','--binary'])).hexdigest()}
(stage/'SHA256SUMS').write_text(''.join(f'{value}  {name}\n' for name,value in hashes.items()))
(stage/'build-info.json').write_text(json.dumps(receipt,indent=2)+'\n')
# Replace the exported set, so obsolete outputs cannot survive branch switches.
previous = dest.with_name('.'+dest.name+'-previous-'+uuid.uuid4().hex)
dest.rename(previous)
try:
    stage.rename(dest)
except BaseException:
    previous.rename(dest)
    raise
shutil.rmtree(previous)
print(f'Build verified. Host-visible artifacts: {dest}')
PY
