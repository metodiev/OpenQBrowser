function(oqb_add_integration_test name)
    qt_add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE oqb_core oqb_project_options Qt6::Test)
    set_target_properties(${name} PROPERTIES MACOSX_BUNDLE OFF WIN32_EXECUTABLE OFF)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    # The network tests need a real loopback connection, which is quick; the
    # generous timeout covers a slow DNS lookup for the HTTPS case.
    set_tests_properties(${name} PROPERTIES TIMEOUT 120)
endfunction()

oqb_add_integration_test(tst_http)
oqb_add_integration_test(tst_pipeline)

oqb_add_integration_test(tst_redirect)

oqb_add_integration_test(tst_perf)
oqb_add_integration_test(tst_scripting)
