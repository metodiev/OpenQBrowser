#!/usr/bin/env bash
#
# Make the macOS application bundle self-contained.
#
#   ./scripts/deploy.sh              bundle Qt and its resources into the .app
#   ./scripts/deploy.sh --dmg        also produce a distributable .dmg
#
# The windowed browser renders pages with Qt WebEngine (Chromium), so a
# distributable bundle must contain not only the Qt frameworks but also the
# QtWebEngineProcess helper application and its resources (ICU data, PAK files,
# translations). macdeployqt assembles all of that from the Qt installation the
# project was built against.
#
# The deployed bundle no longer depends on Qt being installed on the target
# machine: it runs from Finder, from `open`, or by double-clicking it.

set -euo pipefail

# shellcheck source=scripts/common.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

make_dmg=0
verbose=1

usage() {
    cat <<'EOF'
Usage: scripts/deploy.sh [options]

Options:
  --dmg         Produce a distributable .dmg next to the .app
  --verbose     Print each step macdeployqt takes (default: warnings only)
  -h, --help    Show this message

The app must already be built (./scripts/build.sh); this script only deploys.
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --dmg)     make_dmg=1; shift ;;
        --verbose) verbose=2; shift ;;
        -h|--help) usage; exit 0 ;;
        *) oqb_error "Unknown option: $1"; usage >&2; exit 2 ;;
    esac
done

# ------------------------------------------------------------ prerequisites
if [ "$(uname -s)" != "Darwin" ]; then
    oqb_error "deploy.sh bundles macOS application bundles; it only runs on macOS."
    exit 1
fi

if ! command -v macdeployqt >/dev/null 2>&1; then
    oqb_error "macdeployqt was not found on PATH."
    echo "  Install it with:  brew install qt" >&2
    exit 1
fi

bundle="$(oqb_bundle || true)"
if [ -z "${bundle}" ]; then
    oqb_error "No application bundle was found in ${OQB_BUILD_DIR}/bin."
    echo "  Build it first with:  ./scripts/build.sh" >&2
    exit 1
fi

# ------------------------------------------------------------------- deploy
oqb_info "Deploying Qt and its resources into ${bundle}"

# QtWebEngine ships its browser process as a separate helper application and a
# set of resources (ICU, PAK, translations). These live inside the QtWebEngineCore
# framework, and macdeployqt copies them as part of that framework. This is a
# pure Widgets application, so no -qmldir scan is wanted: pointing macdeployqt
# at Qt's own QML directory would copy the entire QML module set (QtPdf, the
# virtual keyboard, QtDBus and friends), most of it with dependencies macdeployqt
# cannot fully resolve.
macdeployqt "${bundle}" "-verbose=${verbose}" "-no-strip"

# --------------------------------------------------------------------- fixup
# macdeployqt copies every platform and image plugin it knows, including ones
# this browser never uses. Two of them drag in frameworks that are either not
# installed (the virtual keyboard) or pure bloat (QtPdf, for reading PDFs as
# images). Removing them keeps the bundle small and avoids macdeployqt's
# "Cannot resolve rpath" warnings for components nothing loads.
oqb_info "Trimming unused plugins and frameworks"

pdf_plugin="$(find "${bundle}" -name "libqpdf.dylib" -path "*/imageformats/*" -print -quit)"
[ -n "${pdf_plugin}" ] && rm -f "${pdf_plugin}"
rm -rf "${bundle}/Contents/Frameworks/QtPdf.framework"
rm -f "${bundle}/Contents/PlugIns/platforminputcontexts/libqtvirtualkeyboardplugin.dylib"

# macdeployqt does not rewrite the QtWebEngineProcess helper's own load
# commands: after it runs, the helper still points at the absolute Homebrew
# framework paths. The helper lives at
#
#   Contents/Frameworks/QtWebEngineCore.framework/Versions/A/Helpers/QtWebEngineProcess.app
#
# and is launched by QtWebEngineCore at runtime, so on a machine without Qt
# installed it would fail to start. Each Qt framework the helper links is
# rewritten to the @rpath form, which the helper already has an rpath for
# (pointing at the app's Frameworks directory).
oqb_info "Rewiring QtWebEngineProcess to the bundled frameworks"

helper_binary="$(find "${bundle}" -path "*/QtWebEngineProcess.app/Contents/MacOS/QtWebEngineProcess" -type f -print -quit)"
if [ -z "${helper_binary}" ]; then
    oqb_error "QtWebEngineProcess was not deployed; the app will not be self-contained."
    exit 1
fi

while read -r framework_path; do
    # /opt/homebrew/opt/<pkg>/lib/<Name>.framework/Versions/A/<Name>
    framework_name="$(basename "${framework_path%.framework/Versions/A/*}")"
    if [ -z "${framework_name}" ]; then
        oqb_warn "Could not parse framework path: ${framework_path}"
        continue
    fi
    install_name_tool -change "${framework_path}" \
        "@rpath/${framework_name}.framework/Versions/A/${framework_name}" \
        "${helper_binary}"
done < <(otool -L "${helper_binary}" | awk '/^[[:space:]]*\/opt\/homebrew\/.*\.framework\/Versions\/A\// { print $1 }')

# Any plain dylib the helper references that macdeployqt copied into Frameworks
# gets the same treatment, so the helper resolves it from the bundle too.
while read -r dylib_path; do
    dylib_name="$(basename "${dylib_path}")"
    install_name_tool -change "${dylib_path}" "@rpath/${dylib_name}" "${helper_binary}"
done < <(otool -L "${helper_binary}" | awk '/^[[:space:]]*\/opt\/homebrew\/.*\.dylib/ { print $1 }')

# A few copied libraries keep their absolute Homebrew path as their *install
# name* (what otool -D reports). Nothing references them by that path any more,
# but rewriting the install name to the @rpath form makes the bundle honest and
# future-proof if something later links against them.
while IFS= read -r -d '' file; do
    file "${file}" 2>/dev/null | grep -q "Mach-O" || continue
    absolute="$(otool -D "${file}" 2>/dev/null | sed -n '2p' | awk '/^\/opt\/homebrew\// { print $1 }')"
    [ -z "${absolute}" ] && continue

    case "${absolute}" in
        *.framework/Versions/A/*)
            # /opt/homebrew/opt/<pkg>/lib/<Name>.framework/Versions/A/<Name>
            name="$(basename "${absolute%.framework/Versions/A/*}")"
            new="@rpath/${name}.framework/Versions/A/${name}"
            ;;
        *)
            # /opt/homebrew/opt/<pkg>/lib/<lib>.<ver>.dylib
            name="$(basename "${absolute}")"
            new="@rpath/${name}"
            ;;
    esac

    install_name_tool -id "${new}" "${file}"
done < <(find "${bundle}" -type f -print0 2>/dev/null)

# macdeployqt rewrites every framework and dylib reference to the form
# "@executable_path/../Frameworks/<name>". That is only correct for the main
# executable: QtWebEngineProcess is its own executable in a nested bundle, so
# its "@executable_path" is the helper's directory and the lookup goes to the
# helper's (nonexistent) Frameworks directory. Rewriting those references to
# "@rpath/<name>" lets every image resolve through its own rpath: the main app
# points at its Frameworks directory, and the helper at the app's Frameworks
# directory.
oqb_info "Rewriting framework references to @rpath"

while IFS= read -r -d '' file; do
    file "${file}" 2>/dev/null | grep -q "Mach-O" || continue
    install_id="$(otool -D "${file}" 2>/dev/null | sed -n '2p')"
    while read -r dep; do
        # The install name is reported first by otool -L but is not a dependency;
        # changing it here would fail, so it is skipped.
        [ -n "${install_id}" ] && [ "${dep}" = "${install_id}" ] && continue
        install_name_tool -change "${dep}" "@rpath/${dep#@executable_path/../Frameworks/}" \
            "${file}" 2>/dev/null || true
    done < <(otool -L "${file}" 2>/dev/null \
             | awk '/^[[:space:]]*@executable_path\/\.\.\/Frameworks\// { print $1 }')
done < <(find "${bundle}" -type f -print0 2>/dev/null)

# The main executable still carries the build machine's Qt rpath. Its own
# references are all @rpath now, so it only needs the standard bundle rpath.
main_binary="${bundle}/Contents/MacOS/openqbrowser"
while read -r rpath; do
    [ "${rpath}" = "@executable_path/../Frameworks" ] && continue
    install_name_tool -delete_rpath "${rpath}" "${main_binary}" 2>/dev/null || true
done < <(otool -l "${main_binary}" 2>/dev/null \
         | awk '/LC_RPATH/{f=1; next} f && /path /{print $2; f=0}')
install_name_tool -add_rpath "@executable_path/../Frameworks" "${main_binary}" 2>/dev/null || true

# Re-sign everything. install_name_tool invalidates the embedded signature of
# every file it touches, and on Apple Silicon an invalid signature is a load
# failure, not a warning. The ad-hoc identity (-) matches the rest of the
# bundle; signing deepest-first is what makes `codesign --verify --deep` pass:
# the helper app is nested inside QtWebEngineCore.framework, so it must be
# signed before the framework that seals it.
oqb_info "Re-signing the application bundle"
find "${bundle}/Contents/Frameworks" -name "*.dylib" -print0 \
    | xargs -0 -n1 codesign --force --sign - 2>/dev/null || true
find "${bundle}/Contents/PlugIns" -name "*.dylib" -print0 \
    | xargs -0 -n1 codesign --force --sign - 2>/dev/null || true
codesign --force --sign - "${helper_binary%/Contents/MacOS/QtWebEngineProcess}"
find "${bundle}/Contents/Frameworks" -type d -name "*.framework" \
    | while IFS= read -r framework; do
          codesign --force --sign - "${framework}" 2>/dev/null || true
      done
codesign --force --sign - "${bundle}"

# -------------------------------------------------------------------- result
oqb_ok "Deployed ${bundle}"

if [ "${make_dmg}" = "1" ]; then
    oqb_info "Creating a distributable disk image"
    macdeployqt "${bundle}" -dmg -verbose="${verbose}" -no-strip
    oqb_ok "Created ${OQB_BUILD_DIR}/bin/openqbrowser.dmg"
fi
