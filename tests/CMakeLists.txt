# OpenQBrowser test suites.
#
#   unit/         single components in isolation
#   integration/  several components working together, including the network
#   browser/      end-to-end scenarios driven through the browser itself

find_package(Qt6 REQUIRED COMPONENTS Test)

add_subdirectory(unit)
add_subdirectory(integration)
add_subdirectory(ui)
