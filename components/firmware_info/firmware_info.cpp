#include "firmware_info/firmware_info.hpp"

#include "esp_app_desc.h"

// Generated at build time by GenerateGitHeader.cmake into
// CMAKE_CURRENT_BINARY_DIR (see CMakeLists.txt) -- never committed, never
// hand-edited, and not part of this component's public include path (only
// this one translation unit needs it).
#include "firmware_info_git.h"

namespace firmware_info {

const char* Version() { return esp_app_get_description()->version; }

const char* GitCommit() { return FIRMWARE_INFO_GIT_COMMIT; }

const char* GitBranch() { return FIRMWARE_INFO_GIT_BRANCH; }

bool GitDirty() { return FIRMWARE_INFO_GIT_DIRTY != 0; }

const char* BuildDate() { return esp_app_get_description()->date; }

const char* BuildTime() { return esp_app_get_description()->time; }

}  // namespace firmware_info
