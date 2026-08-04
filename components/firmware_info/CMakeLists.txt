idf_component_register(
    SRCS "firmware_info.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_app_format
)

set(firmware_info_git_header "${CMAKE_CURRENT_BINARY_DIR}/firmware_info_git.h")

# Regenerated on every build (two fast `git` subprocess calls), not just on
# a CMake reconfigure -- a new commit with no CMakeLists.txt changes would
# otherwise go unnoticed until the next full reconfigure. The script itself
# only actually rewrites the header when its content changed, so a build
# with no new commits since the last one doesn't force a spurious recompile
# of firmware_info.cpp.
add_custom_target(firmware_info_git_header ALL
    COMMAND "${CMAKE_COMMAND}"
            "-DOUTPUT_FILE=${firmware_info_git_header}"
            "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/GenerateGitHeader.cmake"
    BYPRODUCTS "${firmware_info_git_header}"
    COMMENT "Checking firmware git metadata"
    VERBATIM
)
add_dependencies(${COMPONENT_LIB} firmware_info_git_header)

# Private -- firmware_info.cpp is the one place FIRMWARE_INFO_GIT_* is ever
# read (see firmware_info.hpp's own comment); nothing outside this
# component should include the generated header directly.
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
