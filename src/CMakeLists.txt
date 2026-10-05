# OpenQBrowser source tree.
#
# Layout follows architecture/overview.md; each directory owns one stage of the
# browser pipeline and depends only on the stages before it.
#
#   network/     URLs, HTTP/1.1, HTTPS, redirects and resource loading
#   dom/         Document Object Model
#   html/        HTML tokenizer and tree construction
#   css/         CSS tokenizer, parser, selectors and the cascade
#   renderer/    Box tree, layout, painting and image decoding
#   storage/     History, bookmarks and cookies
#   security/    Origin, permission and TLS policy
#   javascript/  The JavaScript integration seam (see architecture/javascript.md)
#   devtools/    Page inspector
#   browser/     Page and Tab orchestration, built-in pages
#   ui/          Qt Widgets shell: window, tabs, address bar

# Core is deliberately free of any windowing dependency, so a page can be
# fetched, parsed, styled and laid out headlessly. The unit and integration
# tests and the command line tools in main.cpp rely on this.
add_library(oqb_core STATIC
    network/Url.cpp
    network/HttpMessage.cpp
    network/HttpClient.cpp
    network/ResourceLoader.cpp
    dom/Node.cpp
    dom/Document.cpp
    html/Entities.cpp
    html/Tokenizer.cpp
    html/Parser.cpp
    css/Tokenizer.cpp
    css/Colors.cpp
    css/Selector.cpp
    css/Value.cpp
    css/Stylesheet.cpp
    css/Cascade.cpp
    renderer/BoxTree.cpp
    renderer/Layout.cpp
    renderer/Painter.cpp
    storage/History.cpp
    storage/Bookmarks.cpp
    security/SecurityPolicy.cpp
    javascript/ScriptEngine.cpp
    devtools/Inspector.cpp
    browser/BuiltinPages.cpp
    browser/Page.cpp
    browser/Tab.cpp
)

target_link_libraries(oqb_core
    PUBLIC Qt6::Core Qt6::Gui Qt6::Network ZLIB::ZLIB
    PRIVATE oqb_project_options
)

# The windowed shell: Qt Widgets on top of the core. It is a separate library so
# that the headless paths (dumps, screenshots, tests) never depend on a windowing
# toolkit being present.
add_library(oqb_ui STATIC
    ui/PageView.cpp
    ui/MainWindow.cpp
)

target_link_libraries(oqb_ui
    PUBLIC oqb_core Qt6::Widgets
    PRIVATE oqb_project_options
)

qt_add_executable(openqbrowser
    main.cpp
    # The icon is compiled into the executable itself. A Qt resource inside a
    # static library is not initialised, because nothing references it, so
    # putting it here is what makes the window icon actually load.
    ../assets/assets.qrc
)

target_link_libraries(openqbrowser PRIVATE oqb_ui oqb_project_options Qt6::Widgets Qt6::Svg)

if(APPLE)
    # On macOS the browser is built as an application bundle, so it can be
    # launched from Finder or with `open` like any other app. The headless modes
    # still work: the bundle's executable is an ordinary binary, and running it
    # with --dump-dom or --screenshot behaves exactly as the plain build does.
    set_target_properties(openqbrowser PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "OpenQBrowser"
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.openqbrowser.OpenQBrowser"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/../assets/Info.plist.in"
    )

    # The icon is generated from the SVG at configure time, so the repository
    # holds one source of truth rather than a set of binary images.
    set(OQB_ICON "${CMAKE_BINARY_DIR}/generated/OpenQBrowser.icns")
    add_custom_command(
        OUTPUT "${OQB_ICON}"
        COMMAND "${CMAKE_COMMAND}" -DOQB_SOURCE_ICON=${CMAKE_CURRENT_SOURCE_DIR}/../assets/icon.svg
                -DOQB_ICON_OUT=${OQB_ICON}
                -P "${CMAKE_CURRENT_SOURCE_DIR}/../assets/make-icns.cmake"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../assets/icon.svg"
                "${CMAKE_CURRENT_SOURCE_DIR}/../assets/make-icns.cmake"
        COMMENT "Generating the application icon from assets/icon.svg"
        VERBATIM
    )
    add_custom_target(oqb_icon DEPENDS "${OQB_ICON}")

    set_source_files_properties("${OQB_ICON}" PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources")
    add_dependencies(openqbrowser oqb_icon)
    target_sources(openqbrowser PRIVATE "${OQB_ICON}")

    set_target_properties(openqbrowser PROPERTIES
        MACOSX_BUNDLE_ICON_FILE "OpenQBrowser.icns")
else()
    # Elsewhere the browser is a plain executable; the windowed and headless
    # modes behave identically to the bundle's binary.
    set_target_properties(openqbrowser PROPERTIES
        MACOSX_BUNDLE OFF
        WIN32_EXECUTABLE OFF
    )
endif()


