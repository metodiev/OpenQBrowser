#!/usr/bin/env bash
#
# Start OpenQBrowser.
#
#   ./scripts/run.sh                        open the browser window
#   ./scripts/run.sh https://bbc.com        open a page straight away
#   ./scripts/run.sh --dump-dom https://example.com    render headlessly
#   ./scripts/run.sh --screenshot=page.png https://bbc.com
#
# Builds first if the binary is missing or older than the sources.

set -euo pipefail

# shellcheck source=scripts/common.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

build_if_needed=1

usage() {
    cat <<'EOF'
Usage: scripts/run.sh [options] [url]

Starts OpenQBrowser. Any option this script does not recognise is passed
straight through to the browser, so all of its own flags work:

  --dump-dom[=FILE]     Write the document instead of opening a window
  --screenshot=FILE     Render the page to a PNG
  --width N --height N  Viewport size for the headless modes
  --no-images           Skip image loading
  --window              Force the window open even with a report option

Script options:
  --no-build            Do not build first, even if the binary is missing
  --rebuild             Always build before starting
  -h, --help            Show this message
  -v, --version         Print the browser version

Environment:
  OQB_BUILD_DIR  Build directory to use (default: <repo>/build)
  OQB_GUI=1      On macOS, launch through the app bundle for a Dock icon
EOF
}

browser_args=()
for arg in "$@"; do
    case "${arg}" in
        --no-build) build_if_needed=0 ;;
        --rebuild)  build_if_needed=2 ;;
        -h|--help)  usage; exit 0 ;;
        *)          browser_args+=("${arg}") ;;
    esac
done

# ---------------------------------------------------------------- build first
binary="$(oqb_binary || true)"

needs_build=0
if [ -z "${binary}" ]; then
    needs_build=1
elif [ "${build_if_needed}" = "2" ]; then
    needs_build=1
elif [ "${build_if_needed}" = "1" ]; then
    # Rebuild when any source file is newer than the binary, so `run.sh` after an
    # edit does the obvious thing instead of launching a stale program.
    newer="$(find "${OQB_ROOT}/src" "${OQB_ROOT}/CMakeLists.txt" \
                  -newer "${binary}" -print -quit 2>/dev/null || true)"
    [ -n "${newer}" ] && needs_build=1
fi

if [ "${needs_build}" = "1" ]; then
    if [ "${build_if_needed}" = "0" ]; then
        oqb_error "The browser is not built and --no-build was given."
        echo "  Build it with:  ./scripts/build.sh" >&2
        exit 1
    fi
    oqb_info "The browser needs building first"
    "${OQB_ROOT}/scripts/build.sh" >&2
    binary="$(oqb_binary || true)"
    if [ -z "${binary}" ]; then
        oqb_error "Still no browser binary after building."
        exit 1
    fi
fi

# ------------------------------------------------------------------- headless
# A dump needs no window and no display: the offscreen platform keeps it working
# over SSH. A screenshot, by contrast, renders through the compositor and needs
# a real platform, so only the dump mode is forced offscreen here.
for arg in "${browser_args[@]}"; do
    case "${arg}" in
        --dump-dom*)
            exec env QT_QPA_PLATFORM=offscreen "${binary}" "${browser_args[@]}" ;;
    esac
done

# -------------------------------------------------------------------- windowed
# Qt warns loudly if these are set, and the window would never appear.
unset QT_QPA_PLATFORM QT_QPA_PLUGIN_PATH 2>/dev/null || true

# Launching the bundle through LaunchServices gives a real Dock icon and menu bar.
# `open` cannot forward output, so it is only used when nothing was redirected.
if [ "$(uname -s)" = "Darwin" ] && [ -n "${OQB_GUI:-}" ]; then
    bundle="$(oqb_bundle || true)"
    if [ -n "${bundle}" ]; then
        exec open "${bundle}" --args "${browser_args[@]}"
    fi
fi

exec "${binary}" "${browser_args[@]}"
