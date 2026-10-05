# End-to-end tests: the browser is driven the way a user drives it, through the
# window and its tabs rather than through the pipeline directly.
function(oqb_add_browser_test name)
    qt_add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE oqb_ui oqb_project_options Qt6::Test Qt6::Widgets)
    set_target_properties(${name} PROPERTIES MACOSX_BUNDLE OFF WIN32_EXECUTABLE OFF)
    add_test(NAME ${name} COMMAND ${name})
    # Widgets need a platform plugin; offscreen keeps the window out of the way.
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    set_tests_properties(${name} PROPERTIES TIMEOUT 120)
endfunction()

oqb_add_browser_test(tst_browser)
