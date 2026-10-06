#!/usr/bin/env bash
#
# Configure and build OpenQBrowser.
#
#   ./scripts/build.sh              configure (if needed) and build
#   ./scripts/build.sh --clean      remove the build directory first
#   ./scripts/build.sh --debug      use the "debug" preset and build-debug/
#   ./scripts/build.sh --test       run the test suite afterwards
#
# The build is incremental: run it again after editing a source file and only the
# affected targets are rebuilt.

set -euo pipefail

# shellcheck source=scripts/common.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

# Only a build needs a working linker, so this is the one place the toolchain is
# checked. `run.sh` reaches this via `build.sh` when it needs to compile.
oqb_pick_macos_toolchain

preset="default"
do_clean=0
do_test=0
jobs=""

usage() {
    cat <<'EOF'
Usage: scripts/build.sh [options]

Options:
  --clean        Delete the build directory before configuring
  --debug        Build the Debug preset into build-debug/
  --test         Run the test suite when the build finishes
  -j N           Use N parallel compile jobs (default: all cores); -jN works too
  -h, --help     Show this message

Environment:
  OQB_BUILD_DIR  Build directory to use (default: <repo>/build)
  CMAKE_PREFIX_PATH  Qt prefix; on macOS this is detected from Homebrew
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --clean)  do_clean=1; shift ;;
        --debug)  preset="debug"; shift ;;
        --test)   do_test=1; shift ;;
        -j)       if [ $# -lt 2 ]; then
                      oqb_error "-j needs a number, for example -j 8."
                      exit 2
                  fi
                  jobs="$2"; shift 2 ;;
        -j*)      jobs="${1#-j}"; shift ;;
        -h|--help) usage; exit 0 ;;
        *) oqb_error "Unknown option: $1"; usage >&2; exit 2 ;;
    esac
done

if [ -n "${jobs}" ] && ! printf '%s' "${jobs}" | grep -qE '^[1-9][0-9]*$'; then
    oqb_error "-j needs a positive number, got '${jobs}'."
    exit 2
fi

if [ "${preset}" = "debug" ]; then
    # A preset owns its own binaryDir, and CMake refuses `--preset` together with
    # `-B`. So the preset is only used when the caller has not chosen a directory
    # of their own; the debug preset's directory is build-debug/.
    if [ "${OQB_BUILD_DIR_WAS_SET}" = "0" ]; then
        OQB_BUILD_DIR="${OQB_ROOT}/build-debug"
        export OQB_BUILD_DIR
    fi
fi

# ----------------------------------------------------------------- toolchain
if ! command -v cmake >/dev/null 2>&1; then
    oqb_error "cmake was not found on PATH."
    echo "  Install it with:  brew install cmake" >&2
    exit 1
fi

if ! command -v c++ >/dev/null 2>&1 && ! command -v clang++ >/dev/null 2>&1 \
   && ! command -v g++ >/dev/null 2>&1; then
    oqb_error "No C++ compiler was found on PATH."
    echo "  On macOS:  xcode-select --install" >&2
    exit 1
fi

if [ "${do_clean}" = "1" ]; then
    oqb_info "Removing ${OQB_BUILD_DIR}"
    rm -rf "${OQB_BUILD_DIR}"
fi

# ------------------------------------------------------------------- configure
# Qt is only needed on the first configure; CMake caches the result afterwards.
if [ ! -f "${OQB_BUILD_DIR}/CMakeCache.txt" ]; then
    oqb_info "Configuring (${preset}) in ${OQB_BUILD_DIR}"
    configure_args=(--preset "${preset}")
    if [ "${OQB_BUILD_DIR}" != "${OQB_ROOT}/build" ] \
       && [ "${OQB_BUILD_DIR}" != "${OQB_ROOT}/build-debug" ]; then
        # An explicit directory cannot be combined with a preset's binaryDir.
        configure_args=(-S "${OQB_ROOT}" -B "${OQB_BUILD_DIR}"
                        -DCMAKE_BUILD_TYPE="$([ "${preset}" = "debug" ] && echo Debug || echo Release)"
                        -DOPENQBROWSER_BUILD_TESTS=ON)
    fi
    if qt_prefix="$(oqb_qt_prefix)"; then
        configure_args+=("-DCMAKE_PREFIX_PATH=${qt_prefix}")
        oqb_info "Using Qt at ${qt_prefix}"
    else
        oqb_warn "Qt was not found in the usual Homebrew locations."
        oqb_warn "If configure fails, install it:  brew install qt"
    fi
    cmake "${configure_args[@]}"
else
    oqb_info "Reusing configuration in ${OQB_BUILD_DIR}"
fi

# ----------------------------------------------------------------------- build
build_args=(--build "${OQB_BUILD_DIR}")
if [ -n "${jobs}" ]; then
    build_args+=(-j "${jobs}")
else
    build_args+=(-j "$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)")
fi

oqb_info "Building"
cmake "${build_args[@]}"

# ---------------------------------------------------------------------- result
binary="$(oqb_binary || true)"
if [ -z "${binary}" ]; then
    oqb_error "The build finished but no browser binary was produced."
    exit 1
fi
oqb_ok "Built ${binary}"

# ------------------------------------------------------------------------ test
if [ "${do_test}" = "1" ]; then
    oqb_info "Running tests"
    # The suites build a QApplication, so they need a platform plugin. Offscreen
    # keeps them headless and is what CI uses.
    QT_QPA_PLATFORM=offscreen ctest --test-dir "${OQB_BUILD_DIR}" --output-on-failure
fi

cat <<EOF

${OQB_BOLD}Next:${OQB_RESET}  ./scripts/run.sh              open the browser window
       ./scripts/run.sh https://bbc.com   open a page
       ./scripts/run.sh --dump-dom https://example.com   render headlessly
EOF
