# Build in nanobind's split mode with the same backend module and stable ABI
# floor as pyslang, so both extensions share one nanobind runtime and can pass
# slang objects between each other.
nanobind_add_module(rtl_wiring_inspector BACKEND_MODULE nanobind_backend
                    STABLE_ABI_VERSION 3.11 rtl_wiring_inspector.cpp)

target_link_libraries(rtl_wiring_inspector PUBLIC netlist slang::slang fmt::fmt)

# slang's bindings directory provides PyAnalysisManager.h, the declaration of
# the type bound as pyslang.analysis.AnalysisManager.
target_include_directories(
  rtl_wiring_inspector
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/python ${slang_SOURCE_DIR}/bindings/python
  PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/source>
         $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>)

install(TARGETS rtl_wiring_inspector DESTINATION ${CMAKE_INSTALL_LIBDIR})

install(FILES $<TARGET_FILE:pyslang> DESTINATION ${CMAKE_INSTALL_LIBDIR})
