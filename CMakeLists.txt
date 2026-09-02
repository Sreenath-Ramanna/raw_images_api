cmake_minimum_required(VERSION 3.13)
project(raw_images_api VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)   # only RIA_API symbols are exported

option(RIA_BUILD_SHARED   "Build a shared library"            ON)
option(RIA_BUILD_LEGACY_ABI "Export the pre-extraction raw_* ABI" ON)
option(RIA_BUILD_EXAMPLES "Build the command-line example"    ON)
option(RIA_BUILD_TESTS    "Build the test binary"             ON)
option(RIA_USE_OPENMP     "Parallelise pixel loops with OpenMP" ON)
# A parent project that embeds this with add_subdirectory usually wants to
# place the .so itself — into an application bundle, say — and not have
# headers and a .pc file land in its own install prefix.
option(RIA_INSTALL        "Generate install rules"            ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(PkgConfig REQUIRED)
# libraw_r is the thread-safe build of the same library. Prefer it: this
# library's whole threading contract is "one handle per thread is fine".
pkg_check_modules(LIBRAW IMPORTED_TARGET libraw_r)
if(NOT LIBRAW_FOUND)
  pkg_check_modules(LIBRAW REQUIRED IMPORTED_TARGET libraw)
endif()

set(RIA_SOURCES
  src/ria_common.c
  src/ria_image.c
  src/ria_raw.c
  src/ria_focus.c
  src/ria_adjust.c
  src/ria_filter.c
)

if(RIA_BUILD_LEGACY_ABI)
  list(APPEND RIA_SOURCES src/ria_legacy.c)
endif()

if(RIA_BUILD_SHARED)
  add_library(raw_images_api SHARED ${RIA_SOURCES})
else()
  add_library(raw_images_api STATIC ${RIA_SOURCES})
endif()
add_library(raw_images_api::raw_images_api ALIAS raw_images_api)

target_include_directories(raw_images_api
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(raw_images_api PRIVATE PkgConfig::LIBRAW m)

target_compile_options(raw_images_api PRIVATE
  -Wall -Wextra -Wno-unused-parameter
  $<$<NOT:$<CONFIG:Debug>>:-O3>
)

set_target_properties(raw_images_api PROPERTIES
  VERSION   ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  POSITION_INDEPENDENT_CODE ON
  C_VISIBILITY_PRESET hidden
)

# The legacy ABI predates RIA_API, so its symbols carry no visibility
# attribute and would be hidden by the project-wide default.
if(RIA_BUILD_LEGACY_ABI)
  set_source_files_properties(src/ria_legacy.c PROPERTIES
    COMPILE_OPTIONS "-fvisibility=default")
  target_compile_definitions(raw_images_api PRIVATE RIA_BUILD_LEGACY_ABI=1)
endif()

if(RIA_USE_OPENMP)
  find_package(OpenMP COMPONENTS C)
  if(OpenMP_C_FOUND)
    target_link_libraries(raw_images_api PRIVATE OpenMP::OpenMP_C)
    message(STATUS "raw_images_api: OpenMP enabled")
  else()
    message(STATUS "raw_images_api: OpenMP not found, pixel loops are serial")
  endif()
endif()

# ── Examples and tests ──────────────────────────────────────────────────────

if(RIA_BUILD_EXAMPLES)
  add_executable(ria_tool examples/ria_tool.c)
  target_link_libraries(ria_tool PRIVATE raw_images_api m)
  target_compile_options(ria_tool PRIVATE -Wall -Wextra)
endif()

if(RIA_BUILD_TESTS)
  enable_testing()
  add_executable(ria_tests tests/test_ria.c)
  target_link_libraries(ria_tests PRIVATE raw_images_api m)
  target_include_directories(ria_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_compile_options(ria_tests PRIVATE -Wall -Wextra)
  add_test(NAME ria_tests COMMAND ria_tests)
endif()

# ── Install ─────────────────────────────────────────────────────────────────

if(NOT RIA_INSTALL)
  return()
endif()

include(GNUInstallDirs)

install(TARGETS raw_images_api
  EXPORT raw_images_api-targets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES include/raw_images_api.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
if(RIA_BUILD_LEGACY_ABI)
  install(FILES include/raw_images_api_legacy.h
          DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()

install(EXPORT raw_images_api-targets
  FILE raw_images_api-config.cmake
  NAMESPACE raw_images_api::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/raw_images_api
)

configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/raw_images_api.pc.in
  ${CMAKE_CURRENT_BINARY_DIR}/raw_images_api.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/raw_images_api.pc
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
