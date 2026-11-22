add_executable(rtl-wiring-inspector driver.cpp)

target_include_directories(
  rtl-wiring-inspector PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>/include)

target_link_libraries(rtl-wiring-inspector PUBLIC netlist slang::slang fmt::fmt)

install(TARGETS rtl-wiring-inspector)
