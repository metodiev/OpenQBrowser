# Build

OpenQBrowser builds with CMake 3.21 or newer, C++20, Qt 6.5 or newer (Core, Gui,
Widgets, Network and Test) and zlib. `CMakePresets.json` defines two configure
presets, `default` (Release, `build/`) and `debug` (`build-debug/`), each with
matching build and test presets.

## Targets

`CMakeLists.txt` (root) sets the standard, turns on `CMAKE_AUTOMOC` and
`CMAKE_AUTORCC`, defaults `CMAKE_BUILD_TYPE` to `Release`, points
`CMAKE_RUNTIME_OUTPUT_DIRECTORY` at `${CMAKE_BINARY_DIR}/bin`, finds Qt and ZLIB,
and defines an interface library used by everything else:

```cmake
add_library(oqb_project_options INTERFACE)
target_include_directories(oqb_project_options INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(oqb_project_options INTERFACE Qt6::Core)
target_compile_definitions(oqb_project_options INTERFACE
    OPENQBROWSER_VERSION="${PROJECT_VERSION}"
    OPENQBROWSER_APP_NAME="${PROJECT_NAME}")
```

`OPENQBROWSER_VERSION` and `OPENQBROWSER_APP_NAME` are the two macros the source
reads (`http::defaultUserAgent()` and `BuiltinPages`); the `src` directory on the
include path is why every include in the tree is written as
`#include "network/Url.h"` rather than with relative paths.

`src/CMakeLists.txt` adds three targets:

| Target | Type | Links | Contents |
| --- | --- | --- | --- |
| `oqb_core` | `STATIC` | `Qt6::Core Qt6::Gui Qt6::Network ZLIB::ZLIB qjs` (PUBLIC), `oqb_project_options` (PRIVATE) | Everything except `main.cpp` and `src/ui/`: network, dom, html, css, renderer, storage, security, javascript, devtools, browser. |
| `oqb_ui` | `STATIC` | `oqb_core`, `Qt6::Widgets` (PUBLIC), `oqb_project_options` (PRIVATE) | `ui/PageView.cpp`, `ui/MainWindow.cpp`, `assets/assets.qrc`. |
| `openqbrowser` | `qt_add_executable` | `oqb_ui`, `oqb_project_options`, `Qt6::Widgets` | `main.cpp`. |

### The two-library split

`oqb_core` has **no windowing dependency**: it links `Core`, `Gui`, `Network` and
zlib, and never `Widgets`. The header comment states the payoffs:

* a page can be fetched, parsed, styled, laid out and painted headlessly, so the
  `--dump-*` and `--screenshot` modes work with no display;
* the unit and integration suites link `oqb_core` only, so they cannot accidentally
  depend on a widget;
* `oqb_ui` is a thin layer on top, and the only thing that needs a platform plugin
  to show anything is the window.

`Qt6::Gui` is in `oqb_core` because layout measuring needs `QFontMetricsF` and
painting needs `QImage`/`QPainter`. That is the reason `src/main.cpp` creates a
`QApplication` rather than a `QCoreApplication`, and the same reason the tests set
`QT_QPA_PLATFORM=offscreen` (`testing.md`).

### Executable properties

```cmake
set_target_properties(openqbrowser PROPERTIES MACOSX_BUNDLE OFF WIN32_EXECUTABLE OFF)
```

The browser is a plain console executable on every platform, so the headless modes
and the test binaries behave identically everywhere and the command line can be
piped. The same properties are set on every test target.

## Options

| Option | Default | Effect |
| --- | --- | --- |
| `OPENQBROWSER_BUILD_TESTS` | `ON` | Adds `tests/` (which requires `Qt6::Test`) and `enable_testing()`. |
| `OPENQBROWSER_WARNINGS_AS_ERRORS` | `OFF` | Adds `-Werror` on top of `-Wall -Wextra -Wpedantic` (or `/W4` under MSVC). |

The configure step prints a summary of version, build type, Qt version, whether
tests are on, and the output directory.

## Tests

`tests/CMakeLists.txt` requires `Qt6::Test` and adds `unit/`, `integration/` and
`browser/`. Each subdirectory defines a helper function that creates the
executable, links it, sets the console-executable properties, registers it with
`add_test()`, and sets `QT_QPA_PLATFORM=offscreen`:

| Function | Links | Notes |
| --- | --- | --- |
| `oqb_add_unit_test` | `oqb_core`, `oqb_project_options`, `Qt6::Test` | Five targets: `tst_url`, `tst_html`, `tst_css`, `tst_layout`, `tst_javascript`. |
| `oqb_add_integration_test` | same | Five targets: `tst_http`, `tst_pipeline`, `tst_redirect`, `tst_perf`, `tst_scripting`; all get `TIMEOUT 120` for the network cases. |
| `oqb_add_browser_test` | `oqb_ui`, `oqb_project_options`, `Qt6::Test`, `Qt6::Widgets` | `tst_browser`; also gets `TIMEOUT 120`. |

The integration suite registers four targets (`tst_http`, `tst_pipeline`,
`tst_redirect`, `tst_perf`) plus `tst_scripting`, so a full `ctest` run reports
eleven tests. `tst_redirect` drives real pages, including `http://github.com` and
a large Wikipedia article, so it is the one suite that reaches the public
network; its 120-second timeout exists for exactly that reason.

All binaries land in `${CMAKE_BINARY_DIR}/bin` (`build/bin/`), which is why the
commands in `testing.md` and the README use that path.

## Building

```bash
# Configure and build (Release, tests on), from the repository root.
cmake --preset default
cmake --build build -j

# Or without presets:
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# A debug build in its own directory:
cmake --preset debug && cmake --build build-debug -j
```

Artifacts: `build/bin/openqbrowser`, plus `build/bin/tst_url`, `tst_html`,
`tst_css`, `tst_layout`, `tst_http`, `tst_pipeline`, `tst_redirect`, `tst_perf` and
`tst_browser`.

Changing a source file is not enough to build it — see the rule in
`contributing.md`: a new `.cpp` must be listed in `src/CMakeLists.txt` (in
`oqb_core` or `oqb_ui`) or CMake will never compile it.

`examples/` needs no build step at all: the pages there are plain HTML, listed in
`examples/README.md`, and are loaded with the built binary, for example
`./build/bin/openqbrowser --screenshot=out.png "file://$PWD/examples/basic-layout.html"`.
`assets/` holds the window icon; `assets/assets.qrc` is compiled into `oqb_ui`, and
`MainWindow` reaches it as `:/assets/icon.svg`.

## macOS specifics that actually matter

Three things bite on macOS, and all three are visible in this environment.

### 1. `DEVELOPER_DIR` and the CommandLineTools SDK

`CMAKE_OSX_SYSROOT` in a working build cache points at
`/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk`. On a machine where the
CommandLineTools SDK is newer than the linker that `/usr/bin/c++` drives, the link
step fails with a wall of errors like

```
ld: multiple errors: tapi error: malformed file
/Library/Developer/CommandLineTools/SDKs/MacOSX27.0.sdk/usr/lib/libSystem.B.tbd:4:20:
  error: unknown architecture
 in '/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/lib/libSystem.tbd'
```

Each `.tbd` names architectures the `ld` in use does not know, so nothing links —
not even CMake's own compiler check, which fails with
`Check for working CXX compiler: /usr/bin/c++ - broken`. The Xcode toolchain is
fine; the SDK is the problem. Point the build at Xcode's own SDK:

```bash
cmake -S . -B build-xcode -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_OSX_SYSROOT="$(xcrun --sdk macosx --show-sdk-path)"
cmake --build build-xcode -j
ctest --test-dir build-xcode --output-on-failure
```

Two notes:

* Use `xcrun --sdk macosx --show-sdk-path`, **not** plain `xcrun --show-sdk-path`:
  without `--sdk macosx` the plain form still reports
  `/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk`, which is the SDK that
  fails. With `--sdk macosx` it reports Xcode's, e.g.
  `/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX26.2.sdk`.
* `DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer` alone does not fix it,
  because CMake still records the CommandLineTools SDK as `CMAKE_OSX_SYSROOT`; the
  explicit `-DCMAKE_OSX_SYSROOT=…` is what changes the compile and link flags.

Once configured, `cmake --build build-xcode -j && ctest --test-dir build-xcode`
runs the suites. `tst_redirect` includes cases that drive a large generated page
(`http://github.com`, a large Wikipedia article), so it is the one suite that
reaches the public network; everything else is hermetic.

### 2. Qt from a package manager

Homebrew Qt (`Qt6_DIR=/opt/homebrew/opt/qt6/lib/cmake/Qt6`) is what this build uses.
CMake prints `Could NOT find WrapVulkanHeaders` during configure; that is harmless,
because Qt's Vulkan integration is optional and nothing in OpenQBrowser uses it.

### 3. The offscreen platform plugin

Qt Widgets and font metrics need a platform plugin even with no display. Tests set
`QT_QPA_PLATFORM=offscreen` through CTest properties; a manual run needs it too:

```bash
QT_QPA_PLATFORM=offscreen ./build/bin/openqbrowser --dump-all about:home
QT_QPA_PLATFORM=offscreen ./build/bin/tst_layout
```

Without it, a headless run fails to create the platform plugin. A related message,
`qt.qpa.fonts: Populating font family aliases took … ms. Replace uses of missing
font family "-apple-system" …`, comes from the generated `about:` pages naming a
CSS family the system does not have; it is a warning, not a failure, and
`LayoutEngine::familiesFor()` falls back to a concrete stack.

## Command line modes

`src/main.cpp` is the entry point. With no reporting option the window opens
(`ui::MainWindow`), which is what running a browser with no arguments should do;
any reporting option keeps it headless. Output destinations are written
`--option=FILE` so Qt's parser cannot mistake a URL for an option value — every URL
contains a colon.

| Option | Effect |
| --- | --- |
| `--dump-dom[=FILE]` | `Inspector::domTree()`; stdout when no file. |
| `--dump-layout[=FILE]` | `Inspector::boxTree()` with geometry. |
| `--dump-boxes[=FILE]` | `Inspector::geometry()`: `x y width height` lines. |
| `--dump-styles[=FILE]` | The computed style of every element. |
| `--dump-all` | `Inspector::fullReport()`: DOM, layout, boxes, styles, scripts, resources. |
| `--screenshot=FILE` | `Page::renderToImage()` written as a PNG. |
| `--width=PX`, `--height=PX` | Viewport size, default 1024×768. |
| `--no-images` | Sets `PageSettings::loadImages = false`. |
| `--window` | Opens the window (implied when no reporting option is given). |

A failed load still renders the error page, so a dump succeeds; the failure is
reported on stderr where a script can see it.

## The macOS application bundle

On macOS the `openqbrowser` target is built as an application bundle rather than a
plain executable, so it launches from Finder and can be made the default browser.
The bundle is `build/bin/openqbrowser.app`, and its executable inside is the same
program the other platforms get:

```bash
open build/bin/openqbrowser.app                              # Finder-style launch
./build/bin/openqbrowser.app/Contents/MacOS/openqbrowser     # the binary itself
```

Both the windowed and headless modes work from the bundle's binary, so
`--dump-dom` and `--screenshot` behave identically whether they are run from the
bundle or from a plain build.

The icon is generated at build time from `assets/icon.svg` by
`assets/make-icns.cmake`, which calls `qlmanage` to render each required size and
`iconutil` to pack the `.iconset` into `OpenQBrowser.icns`. Rendering each size
from the vector rather than downscaling one bitmap is what keeps the small sizes
crisp. The repository therefore stores one source of truth for the artwork.

`assets/Info.plist.in` supplies the bundle's property list; CMake substitutes the
name and version, so they are defined once in the top-level `CMakeLists.txt`.

The window icon is a Qt resource compiled into the executable. It lives in the
executable rather than in `oqb_ui` deliberately: a resource inside a static
library is not initialised unless something references it, and the icon would
silently fail to load. Qt Svg is linked for the same reason, because the icon is
an SVG.
