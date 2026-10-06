#!/usr/bin/env bash
#
# Shared setup for the OpenQBrowser helper scripts.
#
# Sourced, never executed: `build.sh` and `run.sh` both need the same answers to
# "where is the source tree", "which build directory" and "where is Qt", and a
# second copy of those answers would drift from the first.

# Resolve the repository root from this file's location, so the scripts work when
# invoked from any directory and through a symlink.
_common_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OQB_ROOT="$(cd "${_common_dir}/.." && pwd)"
export OQB_ROOT

# The build directory. `OQB_BUILD_DIR` overrides it, and a relative value is
# resolved against the repository root rather than the caller's directory.
if [ -n "${OQB_BUILD_DIR:-}" ]; then
    OQB_BUILD_DIR_WAS_SET=1
else
    OQB_BUILD_DIR_WAS_SET=0
fi
OQB_BUILD_DIR="${OQB_BUILD_DIR:-${OQB_ROOT}/build}"
case "${OQB_BUILD_DIR}" in
    /*) ;;
    *) OQB_BUILD_DIR="${OQB_ROOT}/${OQB_BUILD_DIR}" ;;
esac
export OQB_BUILD_DIR OQB_BUILD_DIR_WAS_SET

# ----------------------------------------------------------------------- Qt
#
# Qt is normally found through the CMake package registry, but a Homebrew install
# is the common case on macOS and passing the prefix explicitly makes the first
# configure work without any environment setup.
oqb_qt_prefix() {
    if [ -n "${CMAKE_PREFIX_PATH:-}" ]; then
        printf '%s\n' "${CMAKE_PREFIX_PATH}"
        return 0
    fi
    for prefix in /opt/homebrew/opt/qt6 /opt/homebrew/opt/qt \
                  /usr/local/opt/qt6 /usr/local/opt/qt; do
        if [ -d "${prefix}/lib/cmake/Qt6" ]; then
            printf '%s\n' "${prefix}"
            return 0
        fi
    done
    if command -v brew >/dev/null 2>&1; then
        local prefix
        prefix="$(brew --prefix qt6 2>/dev/null || brew --prefix qt 2>/dev/null)"
        if [ -n "${prefix}" ] && [ -d "${prefix}/lib/cmake/Qt6" ]; then
            printf '%s\n' "${prefix}"
            return 0
        fi
    fi
    return 1
}

# -------------------------------------------------------------------- output
if [ -t 1 ]; then
    OQB_BOLD=$'\033[1m'; OQB_DIM=$'\033[2m'; OQB_RED=$'\033[31m'
    OQB_GREEN=$'\033[32m'; OQB_YELLOW=$'\033[33m'; OQB_RESET=$'\033[0m'
else
    OQB_BOLD=''; OQB_DIM=''; OQB_RED=''; OQB_GREEN=''; OQB_YELLOW=''; OQB_RESET=''
fi
export OQB_BOLD OQB_DIM OQB_RED OQB_GREEN OQB_YELLOW OQB_RESET

oqb_info()  { printf '%s==>%s %s\n' "${OQB_BOLD}" "${OQB_RESET}" "$*"; }
oqb_warn()  { printf '%sWarning:%s %s\n' "${OQB_YELLOW}" "${OQB_RESET}" "$*" >&2; }
oqb_error() { printf '%sError:%s %s\n' "${OQB_RED}" "${OQB_RESET}" "$*" >&2; }
oqb_ok()    { printf '%s%s%s\n' "${OQB_GREEN}" "$*" "${OQB_RESET}"; }

# The absolute path of the browser binary inside the bundle, or the plain
# executable when the bundle does not exist (non-macOS builds). Prints nothing
# when the program has not been built.
oqb_binary() {
    local bundled="${OQB_BUILD_DIR}/bin/openqbrowser.app/Contents/MacOS/openqbrowser"
    if [ -x "${bundled}" ]; then
        printf '%s\n' "${bundled}"
        return 0
    fi
    if [ -x "${OQB_BUILD_DIR}/bin/openqbrowser" ]; then
        printf '%s\n' "${OQB_BUILD_DIR}/bin/openqbrowser"
        return 0
    fi
    return 1
}

# The application bundle, for launching through the Finder/LaunchServices so the
# window gets a proper Dock icon and menu bar. Prints nothing when absent.
oqb_bundle() {
    local bundle="${OQB_BUILD_DIR}/bin/openqbrowser.app"
    [ -d "${bundle}" ] && printf '%s\n' "${bundle}"
}

# ------------------------------------------------------------------ toolchain
#
# macOS only, and only a workaround for a mismatch that has nothing to do with
# this project. `xcode-select` can point at an Xcode whose linker is older than
# the SDK's `.tbd` stubs; the linker then rejects the newer entries:
#
#   ld: tapi error: malformed file
#   .../libSystem.B.tbd:4:20: error: unknown architecture
#
# The symptom is misleading — it looks like a missing or corrupt library — and it
# fails at configure time as well as at link time.
#
# Rather than compare version numbers, ask the toolchain: link a trivial program.
# The environment that is already configured is tried first, so a setup that
# works is never second-guessed — only a demonstrably broken one is replaced.
#
# Nobody calls this on startup: it costs a compile. `build.sh` calls it, and
# `run.sh` does not need to, because the only way `run.sh` compiles is by
# invoking `build.sh`, which probes again for itself.
oqb_pick_macos_toolchain() {
    [ "$(uname -s)" = "Darwin" ] || return 0

    local cxx=/usr/bin/c++
    [ -x "${cxx}" ] || cxx=c++

    local dir; dir="$(mktemp -d "${TMPDIR:-/tmp}/oqb-probe.XXXXXX")" || return 0
    printf 'int main() { return 0; }\n' > "${dir}/probe.cpp"

    if "${cxx}" "${dir}/probe.cpp" -o "${dir}/probe" >/dev/null 2>&1; then
        rm -rf "${dir}"
        return 0
    fi

    # The current selection cannot link. Fall back to the Command Line Tools,
    # which ship a linker new enough for the SDK's .tbd stubs.
    local clt=/Library/Developer/CommandLineTools
    if [ -d "${clt}" ] &&
       env DEVELOPER_DIR="${clt}" "${cxx}" "${dir}/probe.cpp" -o "${dir}/probe" \
           >/dev/null 2>&1; then
        export DEVELOPER_DIR="${clt}"
        oqb_warn "The selected macOS toolchain cannot link; using ${clt}."
        oqb_warn "See architecture/build.md for why."
    else
        oqb_warn "No macOS toolchain here can link; the build will fail."
        oqb_warn "See architecture/build.md."
    fi
    rm -rf "${dir}"
    return 0
}
