// sysinfo.h -- collects a description of the machine the program runs on.
//
// Two ways to use it:
//   * collectSection()/collectAll() return structured, titled sections (used by
//     the `sysinfo` command line tool);
//   * collect() returns the flat "sys_*" key/value list of the *core* sections
//     (system, cpu, memory, build).  This is the original API: the pairs can be
//     attached to benchmark output with benchmark::AddCustomContext().
//
// Nothing in this header throws.  A value that cannot be determined is reported
// as "unknown" (or left out when it is optional).
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sysinfo {

// ---------------------------------------------------------------------------
// Data model
// ---------------------------------------------------------------------------

using KeyValues = std::vector<std::pair<std::string, std::string>>;

inline constexpr std::string_view kRedacted = "<redacted>";

struct Entry {
  std::string key;       // stable machine name, always "sys_..."
  std::string label;     // human readable name
  std::string value;     // never empty
  std::string redacted;  // value shown instead of `value` when redacting; empty = kRedacted
  bool sensitive{false}; // identifies the machine/person (hostname, MAC, IP, user, ...)

  // The text to print; honours the redaction mode.
  [[nodiscard]] std::string display(bool redact) const {
    if ( !redact || !sensitive ) return value;
    return redacted.empty() ? std::string(kRedacted) : redacted;
  }
};

struct Section {
  std::string id;     // short lower-case name used on the command line
  std::string title;  // heading
  std::vector<Entry> entries;
};

struct SectionInfo {
  std::string id;
  std::string title;
  bool core;  // included in the flat collect() list
};

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

// Library version, e.g. "1.0.0".
[[nodiscard]] std::string_view version() noexcept;

// All known sections in display order.
[[nodiscard]] const std::vector<SectionInfo>& sections();

// Collects one section; `id` must be one of sections().  An unknown id yields an
// empty section.  `verbose` adds the long tail (all CPU flags, pseudo
// filesystems, virtual network devices, ...).
[[nodiscard]] Section collectSection(std::string_view id, bool verbose = false);

// Collects every section.
[[nodiscard]] std::vector<Section> collectAll(bool verbose = false);

// Flat key/value list of the core sections (system, cpu, memory, build).
[[nodiscard]] KeyValues collect();

}  // namespace sysinfo
