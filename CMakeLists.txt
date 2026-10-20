cmake_minimum_required(VERSION 3.20)

project(sysinfo
  VERSION 1.0.0
  DESCRIPTION "Command line tool that prints the specifications of the machine it runs on"
  LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(SYSINFO_BUILD_CLI          "Build the sysinfo command line tool"    ON)
option(SYSINFO_BUILD_TESTS        "Build the tests (run them with ctest)"  ON)
option(SYSINFO_WARNINGS_AS_ERRORS "Treat compiler warnings as errors"      OFF)

if(PROJECT_IS_TOP_LEVEL)
  # Release by default: the build type is reported by sysinfo itself.
  if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
  endif()
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

# ---------------------------------------------------------------------------
# build_info.h: build type and flags, reported by `sysinfo --section build`
# ---------------------------------------------------------------------------
if(CMAKE_BUILD_TYPE)
  set(_type "${CMAKE_BUILD_TYPE}")
  string(TOUPPER "${CMAKE_BUILD_TYPE}" _type_upper)
  set(_flags "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${_type_upper}}")
else()
  set(_type "multi-config")
  set(_flags "${CMAKE_CXX_FLAGS}")
endif()
string(STRIP "${_flags}" _flags)
string(REPLACE "\\" "\\\\" _flags "${_flags}")
string(REPLACE "\"" "\\\"" _flags "${_flags}")
set(SYSINFO_BUILD_TYPE_STR "${_type}")
set(SYSINFO_CXX_FLAGS_STR  "${_flags}")
configure_file(build_info.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/build_info.h" @ONLY)

# ---------------------------------------------------------------------------
# Library: usable from other projects (add_subdirectory + sysinfo::sysinfo)
# ---------------------------------------------------------------------------
add_library(sysinfo_lib STATIC sysinfo.cpp sysinfo.h)
add_library(sysinfo::sysinfo ALIAS sysinfo_lib)
set_target_properties(sysinfo_lib PROPERTIES OUTPUT_NAME sysinfo)
target_include_directories(sysinfo_lib
  PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_features(sysinfo_lib PUBLIC cxx_std_20)
set_target_properties(sysinfo_lib PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
if(WIN32)
  target_link_libraries(sysinfo_lib PRIVATE advapi32)
endif()

function(sysinfo_set_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
    if(SYSINFO_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
    if(SYSINFO_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()
sysinfo_set_warnings(sysinfo_lib)

# ---------------------------------------------------------------------------
# Command line tool
# ---------------------------------------------------------------------------
if(SYSINFO_BUILD_CLI)
  add_executable(sysinfo main.cpp)
  target_link_libraries(sysinfo PRIVATE sysinfo_lib)
  set_target_properties(sysinfo PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  sysinfo_set_warnings(sysinfo)

  include(GNUInstallDirs)
  install(TARGETS sysinfo RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(FILES README.md DESTINATION ${CMAKE_INSTALL_DOCDIR} OPTIONAL)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(SYSINFO_BUILD_CLI AND SYSINFO_BUILD_TESTS AND PROJECT_IS_TOP_LEVEL)
  include(CTest)
  add_test(NAME help            COMMAND sysinfo --help)
  add_test(NAME version         COMMAND sysinfo --version)
  add_test(NAME list            COMMAND sysinfo --list)
  add_test(NAME text            COMMAND sysinfo)
  add_test(NAME verbose         COMMAND sysinfo --verbose)
  add_test(NAME json            COMMAND sysinfo --format json)
  add_test(NAME kv              COMMAND sysinfo -f kv -s core)
  add_test(NAME redacted        COMMAND sysinfo -r -x build)
  add_test(NAME bad_option      COMMAND sysinfo --no-such-option)
  add_test(NAME bad_section     COMMAND sysinfo -s nonsense)
  add_test(NAME bad_format      COMMAND sysinfo -f xml)
  add_test(NAME unwritable_file COMMAND sysinfo -o /nonexistent-dir/out.txt)
  set_tests_properties(bad_option bad_section bad_format PROPERTIES WILL_FAIL TRUE)
  set_tests_properties(unwritable_file PROPERTIES WILL_FAIL TRUE)

  set_tests_properties(version PROPERTIES PASS_REGULAR_EXPRESSION "^sysinfo ${PROJECT_VERSION}")
  set_tests_properties(text    PROPERTIES PASS_REGULAR_EXPRESSION "System.*Processor.*Memory")
  set_tests_properties(json    PROPERTIES PASS_REGULAR_EXPRESSION "\"sys_os\": ")
  set_tests_properties(kv      PROPERTIES PASS_REGULAR_EXPRESSION "sys_cpu_logical_cores=[0-9]+")
endif()

# ---------------------------------------------------------------------------
# Manual (optional): cmake --build <dir> --target manual
# ---------------------------------------------------------------------------
find_program(SYSINFO_LATEXMK latexmk)
if(SYSINFO_LATEXMK AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/doc/sysinfo-manual.tex")
  add_custom_target(manual
    COMMAND "${SYSINFO_LATEXMK}" -pdf -interaction=nonstopmode -halt-on-error
            -outdir=${CMAKE_CURRENT_BINARY_DIR}/doc sysinfo-manual.tex
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/doc"
    COMMENT "Building doc/sysinfo-manual.pdf"
    VERBATIM)
endif()
