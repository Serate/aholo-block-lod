# cmake-js config for find_package
add_library(cmake_js::node INTERFACE IMPORTED)

if(MSVC)
    if(CMAKE_JS_NODELIB_DEF AND CMAKE_JS_NODELIB_TARGET)
        # Generate node.lib
        add_custom_command(OUTPUT ${CMAKE_JS_NODELIB_TARGET}
            COMMAND ${CMAKE_AR} /def:${CMAKE_JS_NODELIB_DEF} /out:${CMAKE_JS_NODELIB_TARGET} ${CMAKE_STATIC_LINKER_FLAGS}
        )
        add_custom_target(node_lib DEPENDS ${CMAKE_JS_NODELIB_TARGET})
        target_link_libraries(cmake_js::node INTERFACE ${CMAKE_JS_NODELIB_TARGET})
        add_dependencies(cmake_js::node node_lib)
    endif()
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_link_libraries(cmake_js::node INTERFACE "delayimp.lib")
endif()

set_target_properties(cmake_js::node PROPERTIES
    INTERFACE_COMPILE_FEATURES cxx_std_14
    INTERFACE_COMPILE_DEFINITIONS "NAPI_VERSION=8"
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_JS_INC}"
    INTERFACE_SOURCES "${CMAKE_JS_SRC}"
)
