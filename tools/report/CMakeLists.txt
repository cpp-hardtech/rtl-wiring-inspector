add_executable(rtl-report report.cpp)

target_include_directories(
  rtl-report PRIVATE $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)

target_link_libraries(rtl-report PRIVATE slang::slang fmt::fmt)

install(TARGETS rtl-report)
