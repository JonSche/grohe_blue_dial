#pragma once

// Read-only build metadata: which firmware is actually running, for boot
// logging and future diagnostics (e.g. M13's "Display firmware
// information" over Home Assistant) -- never used to change behaviour.
//
// Version/build date/build time are thin wrappers around ESP-IDF's own
// esp_app_desc_t (see esp_app_get_description() in esp_app_desc.h) --
// that structure is already the one place ESP-IDF embeds this data into
// the firmware image, already what a future OTA/rollback check would read
// to compare versions between slots, and already populated automatically
// by the build from this repo's version.txt (see that file's own
// comment) -- nothing here duplicates or re-derives it.
//
// Git commit/branch/dirty have no ESP-IDF equivalent, so they're
// generated at build time instead -- see CMakeLists.txt/
// GenerateGitHeader.cmake for exactly how, and firmware_info.cpp for the
// one place both sources (esp_app_desc_t and the generated git header)
// are read. "unknown" (never a hardcoded placeholder like a fixed SHA)
// whenever git metadata wasn't available at build time -- e.g. building
// from a source archive with no `.git`.
namespace firmware_info {

// e.g. "v1.0.0-dev" -- see version.txt at the repository root, the single
// place this string is ever written.
[[nodiscard]] const char* Version();

// Short SHA of the commit this build was made from (git's own default
// abbreviation length, e.g. "ba1155f"), or "unknown".
[[nodiscard]] const char* GitCommit();

// The branch checked out at build time, "HEAD" in a detached-HEAD
// checkout (common for a CI build of a tag), or "unknown".
[[nodiscard]] const char* GitBranch();

// True if the working tree had uncommitted changes at build time. Always
// false for a from-scratch CI checkout; a genuinely useful signal during
// local development, where it's easy to forget an uncommitted edit is
// what's actually running on the dial.
[[nodiscard]] bool GitDirty();

// Compile date/time, exactly as ESP-IDF's own __DATE__/__TIME__-based
// esp_app_desc_t reports them -- the build machine's local wall-clock
// time, with no timezone attached (not necessarily UTC; do not assume
// so when logging or displaying these).
[[nodiscard]] const char* BuildDate();
[[nodiscard]] const char* BuildTime();

}  // namespace firmware_info
