# Builds an .icns application icon from the SVG source.
#
# The macOS tools are used directly: qlmanage renders the SVG at a given size,
# and iconutil packs the resulting iconset. Doing this at build time keeps one
# source of truth for the artwork -- assets/icon.svg -- rather than a set of
# binary images that drift out of step with it.
#
# Invoked as:
#   cmake -DOQB_SOURCE_ICON=<svg> -DOQB_ICON_OUT=<icns> -P make-icns.cmake

if(NOT OQB_SOURCE_ICON OR NOT OQB_ICON_OUT)
    message(FATAL_ERROR "make-icns.cmake needs OQB_SOURCE_ICON and OQB_ICON_OUT")
endif()

if(NOT EXISTS "${OQB_SOURCE_ICON}")
    message(FATAL_ERROR "Icon source not found: ${OQB_SOURCE_ICON}")
endif()

get_filename_component(work_dir "${OQB_ICON_OUT}" DIRECTORY)
set(iconset "${work_dir}/OpenQBrowser.iconset")
set(render_dir "${work_dir}/icon-renders")

file(REMOVE_RECURSE "${iconset}")
file(REMOVE_RECURSE "${render_dir}")
file(MAKE_DIRECTORY "${iconset}")
file(MAKE_DIRECTORY "${render_dir}")

# An iconset needs each of these sizes, plus a "@2x" render of the smaller ones.
set(sizes 16 32 128 256 512)

# Renders the icon at `size` pixels and moves the result to `destination`.
# Rendering at each target size, rather than downscaling one bitmap, is what
# keeps the small sizes crisp in the Dock and in the Finder.
function(oqb_render_icon size destination)
    file(REMOVE "${render_dir}/icon.svg.png")
    execute_process(
        COMMAND qlmanage -t -s ${size} -o "${render_dir}" "${OQB_SOURCE_ICON}"
        OUTPUT_QUIET
        ERROR_QUIET
    )
    if(NOT EXISTS "${render_dir}/icon.svg.png")
        message(FATAL_ERROR "qlmanage could not render ${OQB_SOURCE_ICON} at ${size}px")
    endif()
    file(RENAME "${render_dir}/icon.svg.png" "${destination}")
endfunction()

foreach(size IN LISTS sizes)
    oqb_render_icon(${size} "${iconset}/icon_${size}x${size}.png")
    math(EXPR double "${size} * 2")
    oqb_render_icon(${double} "${iconset}/icon_${size}x${size}@2x.png")
endforeach()

file(REMOVE_RECURSE "${render_dir}")

execute_process(
    COMMAND iconutil -c icns "${iconset}" -o "${OQB_ICON_OUT}"
    RESULT_VARIABLE icon_result
    ERROR_VARIABLE icon_error
)

if(NOT icon_result EQUAL 0 OR NOT EXISTS "${OQB_ICON_OUT}")
    message(FATAL_ERROR "iconutil failed: ${icon_error}")
endif()

message(STATUS "Generated ${OQB_ICON_OUT} from ${OQB_SOURCE_ICON}")
