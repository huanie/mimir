add_executable(mim-lsp main.cpp)
target_compile_definitions(mim-lsp PRIVATE MIM_LSP_VERSION="0.1.0")
target_link_libraries(mim-lsp PRIVATE libmim lsp)
if(MIM_STATIC_PLUGINS)
    target_link_libraries(mim-lsp PRIVATE mim_static_plugins)
endif()

file(RELATIVE_PATH MIM_LSP_REL_LIBDIR "${CMAKE_INSTALL_FULL_BINDIR}" "${CMAKE_INSTALL_FULL_LIBDIR}")
if(APPLE)
    set(MIM_LSP_INSTALL_RPATH "@loader_path/${MIM_LSP_REL_LIBDIR}")
else()
    set(MIM_LSP_INSTALL_RPATH "$ORIGIN/${MIM_LSP_REL_LIBDIR}")
endif()
set_target_properties(mim-lsp PROPERTIES INSTALL_RPATH "${MIM_LSP_INSTALL_RPATH}")
install(TARGETS mim-lsp RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

get_target_property(MIM_LSP_LIBRARY_TYPE lsp TYPE)
if(MIM_LSP_LIBRARY_TYPE STREQUAL "SHARED_LIBRARY")
    install(TARGETS lsp
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()

add_dependencies(mim_all mim-lsp)

if(BUILD_TESTING)
    find_package(Python REQUIRED COMPONENTS Interpreter)
    add_test(NAME mim-lsp
        COMMAND ${Python_EXECUTABLE} "${PROJECT_SOURCE_DIR}/test/lsp.py" $<TARGET_FILE:mim-lsp>
    )
    set_tests_properties(mim-lsp PROPERTIES TIMEOUT 60)
    add_dependencies(test-all mim-lsp)
endif()
