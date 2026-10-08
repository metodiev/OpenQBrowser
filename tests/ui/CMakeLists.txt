# UI tests: the window and its tabs, driven through the widgets.
#
# These link oqb_ui, which pulls in Qt WebEngine (Chromium), so they are slower
# to start than the other suites. They run offscreen, which keeps them headless;
# Chromium renders into its own process either way, so tab bookkeeping is what
# these tests are about rather than pixels.
function(oqb_add_ui_test name)
    qt_add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE oqb_ui oqb_project_options Qt6::Test Qt6::Widgets)
    set_target_properties(${name} PROPERTIES MACOSX_BUNDLE OFF WIN32_EXECUTABLE OFF)
    add_test(NAME ${name} COMMAND ${name})
    # The offscreen platform provides no GPU context, and Chromium's raster path
    # crashes rather than degrading when it cannot make one current:
    #
    #   Unable to initialize SkSurface
    #   Context lost during MakeCurrent
    #   Received signal 11 (SIGSEGV)
    #
    # Disabling the GPU makes Chromium raster on the CPU, which is what an
    # offscreen test wants anyway - no test here asserts on pixels.
    set_tests_properties(${name} PROPERTIES ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu")
    # Chromium's first launch in a fresh profile takes a few seconds.
    set_tests_properties(${name} PROPERTIES TIMEOUT 180)
endfunction()

oqb_add_ui_test(tst_tabs)
oqb_add_ui_test(tst_theme)
