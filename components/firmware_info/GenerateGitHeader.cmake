# Regenerates firmware_info_git.h from the current git state. Invoked as a
# build-time custom target (see CMakeLists.txt), not just at CMake configure
# time, so a new commit is reflected on the next `idf.py build` without a
# full reconfigure.
#
# Expects -DOUTPUT_FILE=... and -DSOURCE_DIR=... (the repository root -- not
# CMAKE_CURRENT_SOURCE_DIR, since this script runs standalone via `cmake -P`
# and has no project context of its own).
#
# Degrades gracefully to "unknown"/clean rather than failing the build: git
# may not be installed, SOURCE_DIR may not be a git repository at all (e.g.
# a source archive with no .git, exactly the "must not only work on one
# developer machine" case this whole milestone is about), or SOURCE_DIR
# could be a shallow CI checkout -- none of those should ever break the
# build, they should just report what they honestly can't determine.

set(commit "unknown")
set(branch "unknown")
set(dirty "0")

find_package(Git QUIET)
if(GIT_EXECUTABLE)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
    WORKING_DIRECTORY "${SOURCE_DIR}"
    OUTPUT_VARIABLE commit_out
    RESULT_VARIABLE commit_rc
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
  if(commit_rc EQUAL 0 AND commit_out)
    set(commit "${commit_out}")
  endif()

  execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-parse --abbrev-ref HEAD
    WORKING_DIRECTORY "${SOURCE_DIR}"
    OUTPUT_VARIABLE branch_out
    RESULT_VARIABLE branch_rc
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
  if(branch_rc EQUAL 0 AND branch_out)
    set(branch "${branch_out}")
  endif()

  # --untracked-files=no: an untracked scratch file sitting in the tree
  # isn't a reason to call a build "dirty" -- only actual modifications to
  # tracked content are.
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no
    WORKING_DIRECTORY "${SOURCE_DIR}"
    OUTPUT_VARIABLE dirty_out
    RESULT_VARIABLE dirty_rc
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
  if(dirty_rc EQUAL 0 AND dirty_out)
    set(dirty "1")
  endif()
endif()

set(new_content
"// Auto-generated at build time by GenerateGitHeader.cmake.
// Do not edit; do not add to source control.
#pragma once
#define FIRMWARE_INFO_GIT_COMMIT \"${commit}\"
#define FIRMWARE_INFO_GIT_BRANCH \"${branch}\"
#define FIRMWARE_INFO_GIT_DIRTY ${dirty}
")

# Only actually rewrite the file if its content changed, so a build with no
# new commits since the last one doesn't force a spurious recompile of
# everything that includes it.
if(EXISTS "${OUTPUT_FILE}")
  file(READ "${OUTPUT_FILE}" existing_content)
else()
  set(existing_content "")
endif()

if(NOT existing_content STREQUAL new_content)
  file(WRITE "${OUTPUT_FILE}" "${new_content}")
endif()
