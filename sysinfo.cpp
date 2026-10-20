// sysinfo.cpp -- see sysinfo.h.
//
// Linux is the primary target: everything is read from /proc, /sys and a few
// POSIX calls, no external programs are run and no root rights are needed.
// macOS (sysctl) and Windows (Win32) provide a reduced set of sections on a
// best-effort basis.
#include "sysinfo.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <version>

#if __has_include("build_info.h")
  #include "build_info.h"
#endif

#ifndef SYSINFO_BUILD_TYPE
  #define SYSINFO_BUILD_TYPE "unknown"
#endif
#ifndef SYSINFO_CXX_FLAGS
  #define SYSINFO_CXX_FLAGS "unknown"
#endif
#ifndef SYSINFO_VERSION
  #define SYSINFO_VERSION "1.0.0"
#endif

#if defined(__linux__) || defined(__APPLE__)
  #include <sys/utsname.h>
  #include <unistd.h>
  #include <pwd.h>
#endif
#if defined(__linux__)
  #include <arpa/inet.h>
  #include <ifaddrs.h>
  #include <net/if.h>
  #include <sched.h>
  #include <sys/statvfs.h>
  #if __has_include(<gnu/libc-version.h>)
    #include <gnu/libc-version.h>
    #define SYSINFO_HAVE_GNU_LIBC 1
  #endif
#endif
#if defined(__APPLE__)
  #include <sys/sysctl.h>
  #include <sys/types.h>
#endif
#if defined(_WIN32)
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#endif

namespace sysinfo {

std::string_view version() noexcept { return SYSINFO_VERSION; }

namespace {

namespace fs = std::filesystem;

// ===========================================================================
// Small helpers
// ===========================================================================

std::string trim(std::string_view s) {
  constexpr std::string_view ws(" \t\r\n\0", 5);
  const auto b = s.find_first_not_of(ws);
  if ( b == std::string_view::npos ) return {};
  const auto e = s.find_last_not_of(ws);
  return std::string(s.substr(b, e - b + 1));
}

std::string unquote(std::string s) {
  if ( s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front() )
    return s.substr(1, s.size() - 2);
  return s;
}

bool isDigit(char c) noexcept { return c >= '0' && c <= '9'; }

bool allDigits(std::string_view s) noexcept {
  return !s.empty() && std::ranges::all_of(s, isDigit);
}

// "cpu12" matches prefix "cpu"; "cpufreq" does not.
bool isIndexed(std::string_view name, std::string_view prefix) noexcept {
  return name.starts_with(prefix) && allDigits(name.substr(prefix.size()));
}

std::string readFirstLine(const fs::path& p) {
  std::ifstream f(p);
  std::string line;
  if ( f && std::getline(f, line) ) return trim(line);
  return {};
}

std::vector<std::string> readLines(const fs::path& p, std::size_t maxLines = 200000) {
  std::vector<std::string> out;
  std::ifstream f(p);
  std::string line;
  while ( out.size() < maxLines && std::getline(f, line) ) out.push_back(line);
  return out;
}

// Leading unsigned decimal number; trailing text ("kB", ...) is ignored.
std::optional<std::uint64_t> parseU64(std::string_view s, int base = 10) {
  const std::string t = trim(s);
  std::string_view v = t;
  if ( base == 16 && v.starts_with("0x") ) v.remove_prefix(2);
  std::uint64_t out {};
  const auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), out, base);
  if ( ec != std::errc {} || p == v.data() ) return std::nullopt;
  return out;
}

std::optional<double> parseDouble(std::string_view s) {
  const std::string t = trim(s);
  if ( t.empty() ) return std::nullopt;
  char* end {nullptr};
  const double d = std::strtod(t.c_str(), &end);
  if ( end == t.c_str() ) return std::nullopt;
  return d;
}

std::string fixed(double v, int precision) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.*f", precision, v);
  return buf;
}

std::string formatBytes(std::uint64_t bytes) {
  constexpr double kib = 1024.0;
  const double b = static_cast<double>(bytes);
  if ( b >= kib * kib * kib * kib ) return fixed(b / (kib * kib * kib * kib), 2) + " TiB";
  if ( b >= kib * kib * kib ) return fixed(b / (kib * kib * kib), 1) + " GiB";
  if ( b >= kib * kib ) return fixed(b / (kib * kib), 0) + " MiB";
  if ( b >= kib ) return fixed(b / kib, 0) + " KiB";
  return std::to_string(bytes) + " B";
}

std::string formatUptime(std::uint64_t s) {
  const auto d = s / 86400, h = (s % 86400) / 3600, m = (s % 3600) / 60;
  std::string out;
  if ( d ) out += std::to_string(d) + (d == 1 ? " day, " : " days, ");
  out += std::to_string(h) + " h, " + std::to_string(m) + " min";
  return out;
}

std::string join(const std::vector<std::string>& parts, std::string_view sep) {
  std::string out;
  for ( const auto& p : parts ) {
    if ( p.empty() ) continue;
    if ( !out.empty() ) out += sep;
    out += p;
  }
  return out;
}

std::string toLower(std::string s) {
  for ( char& c : s ) if ( c >= 'A' && c <= 'Z' ) c = static_cast<char>(c - 'A' + 'a');
  return s;
}

bool containsNoCase(std::string_view hay, std::string_view needle) {
  return toLower(std::string(hay)).find(toLower(std::string(needle))) != std::string::npos;
}

// "cpu2" < "cpu10"
bool naturalLess(const std::string& a, const std::string& b) {
  std::size_t i = 0, j = 0;
  while ( i < a.size() && j < b.size() ) {
    if ( isDigit(a[i]) && isDigit(b[j]) ) {
      std::size_t i2 = i, j2 = j;
      while ( i2 < a.size() && isDigit(a[i2]) ) ++i2;
      while ( j2 < b.size() && isDigit(b[j2]) ) ++j2;
      std::string_view na(a.data() + i, i2 - i), nb(b.data() + j, j2 - j);
      while ( na.size() > 1 && na.front() == '0' ) na.remove_prefix(1);
      while ( nb.size() > 1 && nb.front() == '0' ) nb.remove_prefix(1);
      if ( na.size() != nb.size() ) return na.size() < nb.size();
      if ( na != nb ) return na < nb;
      i = i2;
      j = j2;
    } else {
      if ( a[i] != b[j] ) return a[i] < b[j];
      ++i;
      ++j;
    }
  }
  return (a.size() - i) < (b.size() - j);
}

std::vector<std::string> listDir(const fs::path& dir) {
  std::vector<std::string> out;
  std::error_code ec;
  for ( fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec) )
    out.push_back(it->path().filename().string());
  std::ranges::sort(out, naturalLess);
  return out;
}

bool pathExists(const fs::path& p) {
  std::error_code ec;
  return fs::exists(p, ec);
}

std::string linkTargetName(const fs::path& p) {
  std::error_code ec;
  const auto t = fs::read_symlink(p, ec);
  return ec ? std::string {} : t.filename().string();
}

std::set<std::string_view> splitWords(const std::string& s) {
  std::set<std::string_view> out;
  std::size_t i = 0;
  while ( i < s.size() ) {
    while ( i < s.size() && s[i] == ' ' ) ++i;
    const std::size_t b = i;
    while ( i < s.size() && s[i] != ' ' ) ++i;
    if ( i > b ) out.emplace(s.data() + b, i - b);
  }
  return out;
}

std::string envOr(const char* name) {
  const char* v = std::getenv(name);
  return v ? std::string(v) : std::string {};
}

// Collects entries for one section.  Keys get the "sys_" prefix.
class Builder {
 public:
  Builder(std::string id, std::string title) {
    sec_.id = std::move(id);
    sec_.title = std::move(title);
  }
  // Empty values are reported as "unknown".
  void add(const std::string& key, std::string label, std::string value, bool sensitive = false,
           std::string redacted = {}) {
    if ( value.empty() ) value = "unknown";
    sec_.entries.push_back(Entry {"sys_" + key, std::move(label), std::move(value), std::move(redacted), sensitive});
  }
  // Empty values are left out.
  void addIf(const std::string& key, std::string label, std::string value, bool sensitive = false,
             std::string redacted = {}) {
    if ( !value.empty() ) add(key, std::move(label), std::move(value), sensitive, std::move(redacted));
  }
  [[nodiscard]] bool empty() const noexcept { return sec_.entries.empty(); }
  Section take() { return std::move(sec_); }

 private:
  Section sec_;
};

[[maybe_unused]] Section unsupported(std::string id, std::string title) {
  const std::string key = id + "_status";
  Builder b(std::move(id), std::move(title));
  b.add(key, "Status", "not available on this platform");
  return b.take();
}

// ===========================================================================
// Build / toolchain (all platforms)
// ===========================================================================

std::string compilerString() {
#if defined(__clang__)
  return std::string("clang ") + __clang_version__;
#elif defined(__GNUC__)
  return std::string("GCC ") + __VERSION__;
#elif defined(_MSC_VER)
  return "MSVC " + std::to_string(_MSC_VER);
#else
  return "unknown";
#endif
}

std::string stdlibString() {
#if defined(_LIBCPP_VERSION)
  return "libc++ " + std::to_string(_LIBCPP_VERSION);
#elif defined(__GLIBCXX__)
  return "libstdc++ (GCC " + std::to_string(_GLIBCXX_RELEASE) + ", " + std::to_string(__GLIBCXX__) + ")";
#elif defined(_MSVC_STL_VERSION)
  return "MSVC STL " + std::to_string(_MSVC_STL_VERSION);
#else
  return "unknown";
#endif
}

std::string cxxStandardString() {
#if defined(_MSVC_LANG)
  const long v = static_cast<long>(_MSVC_LANG);
#else
  const long v = static_cast<long>(__cplusplus);
#endif
  const char* name = v > 202302L ? "C++26" : v >= 202302L ? "C++23" : v >= 202002L ? "C++20"
                   : v >= 201703L ? "C++17" : v >= 201402L ? "C++14" : v >= 201103L ? "C++11" : "pre-C++11";
  return std::string(name) + " (" + std::to_string(v) + "L)";
}

Section sectionBuild(bool) {
  Builder b("build", "Build");
  b.add("compiler", "Compiler", compilerString());
  b.add("stdlib", "Standard library", stdlibString());
  b.add("cxx_std", "C++ standard", cxxStandardString());
  b.add("build_type", "Build type", SYSINFO_BUILD_TYPE);
  b.add("cxx_flags", "Compiler flags", SYSINFO_CXX_FLAGS);
  b.add("pointer_bits", "Pointer size", std::to_string(sizeof(void*) * 8) + " bit");
  b.add("endianness", "Byte order", std::endian::native == std::endian::little ? "little endian" : "big endian");
#ifdef NDEBUG
  b.add("assertions", "Assertions", "disabled (NDEBUG)");
#else
  b.add("assertions", "Assertions", "enabled");
#endif
#if defined(SYSINFO_HAVE_GNU_LIBC)
  b.add("libc", "C library", std::string("glibc ") + gnu_get_libc_version());
#endif
  b.add("sysinfo_version", "sysinfo version", std::string(SYSINFO_VERSION));
  return b.take();
}

// ===========================================================================
// Environment (user session)
// ===========================================================================

Section sectionEnvironment(bool) {
  Builder b("environment", "Environment");
#if defined(__linux__) || defined(__APPLE__)
  std::string user;
  if ( const passwd* pw = getpwuid(geteuid()) ) user = pw->pw_name;
  if ( user.empty() ) user = envOr("USER");
  b.addIf("user", "User", user, true);
  b.addIf("is_root", "Superuser", geteuid() == 0 ? "yes" : "no");
#else
  b.addIf("user", "User", envOr("USERNAME"), true);
#endif
  b.addIf("shell", "Shell", envOr("SHELL"));
  b.addIf("lang", "Locale", envOr("LANG"));
  b.addIf("term", "Terminal", envOr("TERM"));
  b.addIf("session_type", "Session type", envOr("XDG_SESSION_TYPE"));
  b.addIf("desktop", "Desktop", envOr("XDG_CURRENT_DESKTOP"));
#if defined(__linux__)
  std::string tz = readFirstLine("/etc/timezone");
  if ( tz.empty() ) {
    std::error_code ec;
    const auto target = fs::read_symlink("/etc/localtime", ec).string();
    if ( const auto pos = target.find("zoneinfo/"); pos != std::string::npos ) tz = target.substr(pos + 9);
  }
  b.addIf("timezone", "Time zone", tz);
#endif
  return b.take();
}

// ===========================================================================
// Linux
// ===========================================================================
#if defined(__linux__)

struct CpuInfo {
  std::string model, vendor, family, modelNo, stepping, microcode, mhz, flags;
  std::string armImplementer, armPart, hardware;
  unsigned logical {0};
  bool hypervisor {false};
  std::set<std::string> physIds;
  std::set<std::pair<std::string, std::string>> cores;
};

const CpuInfo& cpuInfo() {
  static const CpuInfo info = [] {
    CpuInfo ci;
    std::string physId, coreId;
    auto flush = [&] {
      if ( !physId.empty() && !coreId.empty() ) ci.cores.emplace(physId, coreId);
      physId.clear();
      coreId.clear();
    };
    for ( const auto& line : readLines("/proc/cpuinfo") ) {
      if ( line.empty() ) { flush(); continue; }
      const auto colon = line.find(':');
      if ( colon == std::string::npos ) continue;
      const std::string key = trim(std::string_view(line).substr(0, colon));
      const std::string val = trim(std::string_view(line).substr(colon + 1));
      auto setOnce = [&](std::string& dst) { if ( dst.empty() ) dst = val; };
      if ( key == "processor" ) ++ci.logical;
      else if ( key == "physical id" ) { physId = val; ci.physIds.insert(val); }
      else if ( key == "core id" ) coreId = val;
      else if ( key == "model name" || key == "Model Name" || key == "cpu model" || key == "cpu" ) setOnce(ci.model);
      else if ( key == "Hardware" ) setOnce(ci.hardware);
      else if ( key == "vendor_id" ) setOnce(ci.vendor);
      else if ( key == "cpu family" ) setOnce(ci.family);
      else if ( key == "model" ) setOnce(ci.modelNo);
      else if ( key == "stepping" ) setOnce(ci.stepping);
      else if ( key == "microcode" ) setOnce(ci.microcode);
      else if ( key == "cpu MHz" ) setOnce(ci.mhz);
      else if ( key == "CPU implementer" ) setOnce(ci.armImplementer);
      else if ( key == "CPU part" ) setOnce(ci.armPart);
      else if ( key == "flags" || key == "Features" ) setOnce(ci.flags);
    }
    flush();
    if ( ci.model.empty() ) ci.model = ci.hardware;
    if ( ci.model.empty() && !ci.armImplementer.empty() ) {
      const auto impl = parseU64(ci.armImplementer, 16).value_or(0);
      const char* name = impl == 0x41 ? "ARM" : impl == 0x61 ? "Apple" : impl == 0x51 ? "Qualcomm"
                       : impl == 0x4e ? "NVIDIA" : impl == 0x43 ? "Cavium" : impl == 0x46 ? "Fujitsu"
                       : impl == 0xc0 ? "Ampere" : "unknown vendor";
      ci.model = std::string(name) + " CPU (implementer " + ci.armImplementer + ", part " + ci.armPart + ")";
    }
    if ( ci.logical == 0 ) ci.logical = std::thread::hardware_concurrency();
    ci.hypervisor = splitWords(ci.flags).contains("hypervisor");
    return ci;
  }();
  return info;
}

struct Topology {
  std::set<std::string> packages;
  std::set<std::string> coreGroups;
  unsigned online {0};
};

Topology readTopology() {
  Topology t;
  const fs::path root {"/sys/devices/system/cpu"};
  for ( const auto& name : listDir(root) ) {
    if ( !isIndexed(name, "cpu") ) continue;
    const fs::path dir = root / name;
    if ( readFirstLine(dir / "online") == "0" ) continue;  // cpu0 has no such file
    const std::string pkg = readFirstLine(dir / "topology/physical_package_id");
    const std::string sib = readFirstLine(dir / "topology/thread_siblings_list");
    ++t.online;
    if ( !pkg.empty() ) t.packages.insert(pkg);
    if ( !sib.empty() ) t.coreGroups.insert(pkg + "/" + sib);
  }
  return t;
}

std::optional<std::string> cgroupSelfPath() {
  for ( const auto& line : readLines("/proc/self/cgroup", 64) )
    if ( line.starts_with("0::") ) return line.substr(3);
  return std::nullopt;
}

std::optional<std::uint64_t> cgroupMemoryLimit() {
  std::vector<fs::path> candidates {"/sys/fs/cgroup/memory.max", "/sys/fs/cgroup/memory/memory.limit_in_bytes"};
  if ( const auto self = cgroupSelfPath(); self && *self != "/" ) candidates.push_back(fs::path("/sys/fs/cgroup" + *self) / "memory.max");
  std::optional<std::uint64_t> best;
  for ( const auto& c : candidates ) {
    const std::string v = readFirstLine(c);
    if ( !allDigits(v) ) continue;  // "max" or missing
    if ( const auto n = parseU64(v); n && (!best || *n < *best) ) best = n;
  }
  return best;
}

std::optional<double> cgroupCpuLimit() {  // in CPUs
  std::vector<fs::path> candidates {"/sys/fs/cgroup/cpu.max"};
  if ( const auto self = cgroupSelfPath(); self && *self != "/" ) candidates.push_back(fs::path("/sys/fs/cgroup" + *self) / "cpu.max");
  std::optional<double> best;
  for ( const auto& c : candidates ) {
    const std::string v = readFirstLine(c);
    const auto sp = v.find(' ');
    if ( sp == std::string::npos ) continue;
    const auto quota = parseU64(v.substr(0, sp));
    const auto period = parseU64(v.substr(sp + 1));
    if ( !quota || !period || *period == 0 ) continue;  // "max ..."
    const double cpus = static_cast<double>(*quota) / static_cast<double>(*period);
    if ( !best || cpus < *best ) best = cpus;
  }
  return best;
}

std::string detectContainer() {
  const std::string env = envOr("container");
  if ( !env.empty() ) return env;
  if ( pathExists("/.dockerenv") ) return "Docker";
  if ( pathExists("/run/.containerenv") ) return "Podman";
  for ( const auto& line : readLines("/proc/1/cgroup", 64) ) {
    if ( line.find("kubepods") != std::string::npos ) return "Kubernetes";
    if ( line.find("docker") != std::string::npos ) return "Docker";
    if ( line.find("lxc") != std::string::npos ) return "LXC";
  }
  return {};
}

std::string dmi(const char* name) {
  std::string v = readFirstLine(std::string("/sys/devices/virtual/dmi/id/") + name);
  if ( v.empty() ) v = readFirstLine(std::string("/sys/class/dmi/id/") + name);
  // placeholders written by some firmware
  if ( containsNoCase(v, "to be filled") || containsNoCase(v, "default string") || v == "None" ) return {};
  return v;
}

Section sectionSystem(bool verbose) {
  Builder b("system", "System");

  char host[256] {};
  if ( gethostname(host, sizeof host - 1) == 0 ) b.add("hostname", "Hostname", host, true);

  std::string osName;
  for ( const auto& line : readLines("/etc/os-release", 256) )
    if ( line.starts_with("PRETTY_NAME=") ) osName = unquote(trim(line.substr(12)));
  utsname u {};
  const bool haveUname = (uname(&u) == 0);
  if ( osName.empty() && haveUname ) osName = u.sysname;
  b.add("os", "Operating system", osName);
  if ( haveUname ) {
    b.add("kernel", "Kernel", std::string(u.sysname) + " " + u.release);
    b.add("arch", "Architecture", u.machine);
  }

  std::string machine = trim(dmi("sys_vendor") + " " + dmi("product_name"));
  if ( machine.empty() ) machine = trim(readFirstLine("/proc/device-tree/model"));  // ARM boards
  b.addIf("machine", "Machine", machine);
  b.addIf("board", "Mainboard", trim(dmi("board_vendor") + " " + dmi("board_name")));
  {
    std::string fw = dmi("bios_vendor");
    if ( const auto v = dmi("bios_version"); !v.empty() ) fw += (fw.empty() ? "" : " ") + v;
    if ( const auto d = dmi("bios_date"); !d.empty() ) fw += " (" + d + ")";
    b.addIf("firmware", "Firmware", fw);
  }
  (void)verbose;

  // virtualization
  const auto& ci = cpuInfo();
  const std::string vendor = dmi("sys_vendor"), product = dmi("product_name");
  const std::string vmHint = vendor + " " + product;
  const bool vmByDmi = containsNoCase(vmHint, "qemu") || containsNoCase(vmHint, "kvm") || containsNoCase(vmHint, "vmware") ||
                       containsNoCase(vmHint, "virtualbox") || containsNoCase(vmHint, "xen") || containsNoCase(vmHint, "virtual machine") ||
                       containsNoCase(vmHint, "amazon ec2") || containsNoCase(vmHint, "google compute") || containsNoCase(vmHint, "innotek");
  if ( ci.hypervisor ) b.add("virtualized", "Virtualization", "yes (hypervisor flag set)");
  else if ( vmByDmi ) b.add("virtualized", "Virtualization", "yes (DMI: " + trim(vmHint) + ")");
  else b.add("virtualized", "Virtualization", "no hypervisor flag");
  b.addIf("container", "Container", detectContainer());
  {
    const std::string rel = toLower(haveUname ? std::string(u.release) : std::string {});
    if ( rel.find("microsoft") != std::string::npos || rel.find("wsl") != std::string::npos ) b.add("wsl", "WSL", "yes");
  }

  if ( const auto up = parseDouble(readFirstLine("/proc/uptime")) ) b.add("uptime", "Uptime", formatUptime(static_cast<std::uint64_t>(*up)));
  {
    const std::string la = readFirstLine("/proc/loadavg");
    std::vector<std::string> parts;
    std::size_t i = 0;
    for ( int n = 0; n < 3 && i < la.size(); ++n ) {
      const auto sp = la.find(' ', i);
      parts.push_back(la.substr(i, sp == std::string::npos ? std::string::npos : sp - i));
      if ( sp == std::string::npos ) break;
      i = sp + 1;
    }
    if ( parts.size() == 3 ) b.add("loadavg", "Load average (1/5/15 min)", join(parts, "  "));
  }
  return b.take();
}

#if defined(__x86_64__) || defined(__i386__)
constexpr std::array<std::pair<std::string_view, std::string_view>, 16> kSimdFlags {{
    {"sse2", "SSE2"}, {"sse4_1", "SSE4.1"}, {"sse4_2", "SSE4.2"}, {"avx", "AVX"}, {"avx2", "AVX2"}, {"fma", "FMA"},
    {"avx512f", "AVX-512F"}, {"avx512bw", "AVX-512BW"}, {"avx512vl", "AVX-512VL"}, {"avx512_vnni", "AVX-512 VNNI"},
    {"amx_tile", "AMX"}, {"aes", "AES-NI"}, {"sha_ni", "SHA"}, {"bmi2", "BMI2"}, {"popcnt", "POPCNT"}, {"vaes", "VAES"}}};
#else
constexpr std::array<std::pair<std::string_view, std::string_view>, 9> kSimdFlags {{
    {"asimd", "NEON (ASIMD)"}, {"neon", "NEON"}, {"sve", "SVE"}, {"sve2", "SVE2"}, {"aes", "AES"},
    {"sha2", "SHA2"}, {"crc32", "CRC32"}, {"atomics", "LSE atomics"}, {"fphp", "FP16"}}};
#endif

Section sectionCpu(bool verbose) {
  Builder b("cpu", "Processor");
  const auto& ci = cpuInfo();
  const Topology topo = readTopology();

  b.add("cpu_model", "Model", ci.model);
  b.addIf("cpu_vendor", "Vendor", ci.vendor);
  if ( !ci.family.empty() )
    b.add("cpu_signature", "Family / model / stepping", ci.family + " / " + ci.modelNo + " / " + ci.stepping);
  b.addIf("cpu_microcode", "Microcode", ci.microcode);

  // sockets and cores: sysfs topology first (works on ARM and in VMs), cpuinfo as fallback
  const std::size_t sockets = !topo.packages.empty() ? topo.packages.size() : ci.physIds.size();
  const std::size_t cores = !topo.coreGroups.empty() ? topo.coreGroups.size() : ci.cores.size();
  b.add("cpu_sockets", "Sockets", sockets ? std::to_string(sockets) : std::string {});
  b.add("cpu_physical_cores", "Physical cores", cores ? std::to_string(cores) : std::string {});
  const unsigned logical = ci.logical ? ci.logical : topo.online;
  b.add("cpu_logical_cores", "Logical cores (threads)", std::to_string(logical));

  const std::string smt = readFirstLine("/sys/devices/system/cpu/smt/control");
  if ( smt == "on" || smt == "off" || smt == "forceoff" || smt == "notsupported" || smt == "notimplemented" )
    b.add("cpu_smt", "SMT / Hyper-Threading", smt == "on" ? "on" : smt == "off" || smt == "forceoff" ? "off" : "not supported");

  {
    unsigned nodes = 0;
    for ( const auto& n : listDir("/sys/devices/system/node") ) if ( isIndexed(n, "node") ) ++nodes;
    if ( nodes ) b.add("cpu_numa_nodes", "NUMA nodes", std::to_string(nodes));
  }
  {
    cpu_set_t set;
    CPU_ZERO(&set);
    if ( sched_getaffinity(0, sizeof set, &set) == 0 ) {
      const int usable = CPU_COUNT(&set);
      if ( usable > 0 && static_cast<unsigned>(usable) != logical )
        b.add("cpu_usable_cores", "Usable by this process", std::to_string(usable));
    }
  }
  if ( const auto lim = cgroupCpuLimit() ) b.add("cpu_cgroup_limit", "CPU limit (cgroup)", fixed(*lim, 2) + " CPUs");

  // frequency scaling
  const std::string base {"/sys/devices/system/cpu/cpu0/cpufreq/"};
  if ( const auto v = parseU64(readFirstLine(base + "cpuinfo_max_freq")) ) b.add("cpu_max_mhz", "Max frequency (MHz)", std::to_string(*v / 1000));
  if ( const auto v = parseU64(readFirstLine(base + "cpuinfo_min_freq")) ) b.add("cpu_min_mhz", "Min frequency (MHz)", std::to_string(*v / 1000));
  if ( const auto v = parseU64(readFirstLine(base + "scaling_cur_freq")) ) b.add("cpu_cur_mhz", "Current frequency, cpu0 (MHz)", std::to_string(*v / 1000));
  else if ( const auto d = parseDouble(ci.mhz) ) b.add("cpu_cur_mhz", "Current frequency, cpu0 (MHz)", std::to_string(static_cast<long long>(*d + 0.5)));
  {
    // governor of every cpu (they should all agree for reproducible benchmarks)
    std::set<std::string> govs;
    const fs::path root {"/sys/devices/system/cpu"};
    for ( const auto& name : listDir(root) )
      if ( isIndexed(name, "cpu") )
        if ( auto g = readFirstLine(root / name / "cpufreq/scaling_governor"); !g.empty() ) govs.insert(std::move(g));
    if ( !govs.empty() ) {
      std::vector<std::string> v(govs.begin(), govs.end());
      b.add("cpu_governor", "Frequency governor", join(v, ", ") + (v.size() > 1 ? " (mixed)" : ""));
    }
  }
  b.addIf("cpu_freq_driver", "Frequency driver", readFirstLine(base + "scaling_driver"));
  {
    const std::string noTurbo = readFirstLine("/sys/devices/system/cpu/intel_pstate/no_turbo");
    const std::string boost = readFirstLine("/sys/devices/system/cpu/cpufreq/boost");
    if ( noTurbo == "1" ) b.add("cpu_turbo", "Turbo boost", "disabled");
    else if ( noTurbo == "0" ) b.add("cpu_turbo", "Turbo boost", "enabled");
    else if ( boost == "1" ) b.add("cpu_turbo", "Turbo boost", "enabled (boost)");
    else if ( boost == "0" ) b.add("cpu_turbo", "Turbo boost", "disabled (boost)");
  }

  // instruction set extensions
  {
    const auto words = splitWords(ci.flags);
    std::vector<std::string> found;
    for ( const auto& [flag, name] : kSimdFlags ) if ( words.contains(flag) ) found.emplace_back(name);
    b.addIf("cpu_simd", "Instruction extensions", join(found, ", "));
    if ( verbose ) b.addIf("cpu_flags", "All CPU flags", ci.flags);
  }

  // caches of cpu0
  {
    std::vector<std::string> caches;
    std::string line;
    const std::string root {"/sys/devices/system/cpu/cpu0/cache/"};
    for ( const auto& idx : listDir(root) ) {
      if ( !isIndexed(idx, "index") ) continue;
      const fs::path dir = fs::path(root) / idx;
      const std::string level = readFirstLine(dir / "level");
      if ( level.empty() ) continue;
      const std::string type = readFirstLine(dir / "type");
      std::string tag = "L" + level;
      if ( type == "Data" ) tag += "d";
      else if ( type == "Instruction" ) tag += "i";
      caches.push_back(tag + " " + readFirstLine(dir / "size"));
      if ( line.empty() && level == "1" && type == "Data" ) line = readFirstLine(dir / "coherency_line_size");
    }
    b.addIf("cpu_caches", "Caches (cpu0)", join(caches, ", "));
    b.addIf("cpu_cache_line", "Cache line size", line.empty() ? std::string {} : line + " B");
  }
  return b.take();
}

Section sectionMemory(bool verbose) {
  Builder b("memory", "Memory");
  std::map<std::string, std::uint64_t> m;  // kB
  for ( const auto& line : readLines("/proc/meminfo", 256) ) {
    const auto colon = line.find(':');
    if ( colon == std::string::npos ) continue;
    if ( const auto v = parseU64(line.substr(colon + 1)) ) m[line.substr(0, colon)] = *v;
  }
  const std::uint64_t totalKb = m.contains("MemTotal") ? m["MemTotal"] : 0;
  b.add("ram_total", "Total RAM", totalKb ? formatBytes(totalKb * 1024ULL) : std::string {});
  if ( m.contains("MemAvailable") ) b.add("ram_available", "Available RAM", formatBytes(m["MemAvailable"] * 1024ULL));
  if ( const auto lim = cgroupMemoryLimit(); lim && (totalKb == 0 || *lim < totalKb * 1024ULL) )
    b.add("ram_cgroup_limit", "RAM limit (cgroup)", formatBytes(*lim));  // container limited below physical RAM
  if ( m.contains("SwapTotal") ) {
    const auto total = m["SwapTotal"], free = m.contains("SwapFree") ? m["SwapFree"] : 0;
    b.add("swap", "Swap", total ? formatBytes(total * 1024ULL) + " total, " + formatBytes((total - std::min(total, free)) * 1024ULL) + " used" : "none");
  }
  b.addIf("thp", "Transparent huge pages", readFirstLine("/sys/kernel/mm/transparent_hugepage/enabled"));
  if ( m.contains("Hugepagesize") ) {
    std::string v = formatBytes(m["Hugepagesize"] * 1024ULL);
    if ( m.contains("HugePages_Total") && m["HugePages_Total"] ) v += ", " + std::to_string(m["HugePages_Total"]) + " reserved";
    b.add("hugepages", "Huge page size", v);
  }
#if defined(_SC_PAGESIZE)
  if ( const long ps = sysconf(_SC_PAGESIZE); ps > 0 ) b.add("page_size", "Page size", formatBytes(static_cast<std::uint64_t>(ps)));
#endif
  if ( verbose ) {
    b.addIf("overcommit", "Overcommit policy", readFirstLine("/proc/sys/vm/overcommit_memory"));
    b.addIf("swappiness", "Swappiness", readFirstLine("/proc/sys/vm/swappiness"));
  }
  return b.take();
}

std::string decodeMountField(std::string_view s) {  // "\040" -> ' '
  std::string out;
  for ( std::size_t i = 0; i < s.size(); ++i ) {
    if ( s[i] == '\\' && i + 3 < s.size() + 0 && isDigit(s[i + 1]) && isDigit(s[i + 2]) && isDigit(s[i + 3]) ) {
      out += static_cast<char>((s[i + 1] - '0') * 64 + (s[i + 2] - '0') * 8 + (s[i + 3] - '0'));
      i += 3;
    } else out += s[i];
  }
  return out;
}

std::string sanitizeKey(const std::string& s) {
  std::string out;
  for ( char c : s ) out += (isDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) ? c : '_';
  while ( !out.empty() && out.front() == '_' ) out.erase(out.begin());
  while ( !out.empty() && out.back() == '_' ) out.pop_back();
  return out;
}

Section sectionStorage(bool verbose) {
  Builder b("storage", "Storage");

  // physical block devices
  for ( const auto& name : listDir("/sys/block") ) {
    if ( !verbose && (name.starts_with("loop") || name.starts_with("ram") || name.starts_with("zram") ||
                      name.starts_with("fd") || name.starts_with("sr") || name.starts_with("dm-")) ) continue;
    const fs::path dir = fs::path("/sys/block") / name;
    const auto sectors = parseU64(readFirstLine(dir / "size"));
    if ( !sectors || *sectors == 0 ) continue;
    const std::string rot = readFirstLine(dir / "queue/rotational");
    std::string type;
    if ( name.starts_with("nvme") ) type = "NVMe SSD";
    else if ( name.starts_with("vd") || name.starts_with("xvd") ) type = "virtual disk";
    else if ( name.starts_with("mmcblk") ) type = "MMC/SD";
    else if ( rot == "1" ) type = "HDD";
    else if ( rot == "0" ) type = "SSD";
    std::string vendorId = readFirstLine(dir / "device/vendor");
    if ( vendorId.starts_with("0x") ) vendorId.clear();  // virtio/PCI id, not a name
    const std::string model = trim(vendorId + " " + readFirstLine(dir / "device/model"));
    b.add("disk_" + sanitizeKey(name), "Disk " + name, join({formatBytes(*sectors * 512ULL), type, model}, ", "));
  }

  // mounted filesystems
  static const std::set<std::string, std::less<>> real {
      "ext2", "ext3", "ext4", "xfs", "btrfs", "zfs", "f2fs", "jfs", "reiserfs", "vfat", "exfat", "ntfs", "ntfs3",
      "fuseblk", "nfs", "nfs4", "cifs", "smb3", "9p", "virtiofs", "overlay"};
  static const std::set<std::string, std::less<>> pseudo {
      "proc", "sysfs", "devtmpfs", "devpts", "cgroup", "cgroup2", "securityfs", "debugfs", "tracefs", "configfs",
      "pstore", "bpf", "autofs", "mqueue", "hugetlbfs", "fusectl", "binfmt_misc", "rpc_pipefs", "efivarfs",
      "squashfs", "nsfs", "ramfs", "selinuxfs", "fuse.gvfsd-fuse", "fuse.portal", "fuse.snapfuse", "devfs", "binder"};
  std::set<std::string> seenSources;
  std::set<std::string> seenKeys;
  for ( const auto& line : readLines("/proc/self/mounts", 4096) ) {
    std::vector<std::string> f;
    std::size_t i = 0;
    while ( i < line.size() && f.size() < 4 ) {
      const auto sp = line.find(' ', i);
      f.push_back(decodeMountField(std::string_view(line).substr(i, sp == std::string::npos ? std::string::npos : sp - i)));
      if ( sp == std::string::npos ) break;
      i = sp + 1;
    }
    if ( f.size() < 3 ) continue;
    const std::string &src = f[0], &mp = f[1], &type = f[2];
    if ( verbose ? pseudo.contains(type) : !real.contains(type) ) continue;
    if ( !verbose ) {
      if ( type == "overlay" && mp != "/" ) continue;
      if ( mp.starts_with("/var/lib/docker") || mp.starts_with("/var/lib/containers") || mp.starts_with("/snap/") ) continue;
      if ( type != "overlay" && !seenSources.insert(src).second ) continue;  // bind mounts, btrfs subvolumes
    }
    struct statvfs sv {};
    if ( statvfs(mp.c_str(), &sv) != 0 || sv.f_blocks == 0 ) continue;
    const double frs = static_cast<double>(sv.f_frsize);
    const std::uint64_t total = static_cast<std::uint64_t>(static_cast<double>(sv.f_blocks) * frs);
    const std::uint64_t used = static_cast<std::uint64_t>(static_cast<double>(sv.f_blocks - sv.f_bfree) * frs);
    const std::uint64_t avail = static_cast<std::uint64_t>(static_cast<double>(sv.f_bavail) * frs);
    const double pct = (used + avail) ? 100.0 * static_cast<double>(used) / static_cast<double>(used + avail) : 0.0;
    std::string key = mp == "/" ? "root" : sanitizeKey(mp);
    if ( key.empty() ) key = "mount";
    for ( int n = 2; !seenKeys.insert(key).second; ++n ) key = sanitizeKey(mp) + "_" + std::to_string(n);
    b.add("fs_" + key, "Mount " + mp,
          type + " (" + src + "), " + formatBytes(total) + ", " + fixed(pct, 0) + "% used");
  }
  return b.take();
}

struct PciNames {
  std::string vendor, device;
};

std::string hex4(unsigned v) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "%04x", v & 0xffffu);
  return buf;
}

PciNames lookupPci(unsigned vid, unsigned did) {
  PciNames out;
  static const char* kPaths[] = {"/usr/share/hwdata/pci.ids", "/usr/share/misc/pci.ids", "/usr/share/pci.ids", "/var/lib/pciutils/pci.ids"};
  for ( const char* path : kPaths ) {
    std::ifstream f(path);
    if ( !f ) continue;
    const std::string v = hex4(vid), d = hex4(did);
    std::string line;
    bool inVendor = false;
    while ( std::getline(f, line) ) {
      if ( line.empty() || line[0] == '#' ) continue;
      if ( line[0] != '\t' ) {
        if ( inVendor ) break;  // left the vendor's block
        if ( line.compare(0, 4, v) == 0 && line.size() > 6 ) { out.vendor = trim(line.substr(4)); inVendor = true; }
      } else if ( inVendor && line.size() > 5 && line[1] != '\t' && line.compare(1, 4, d) == 0 ) {
        out.device = trim(line.substr(5));
        break;
      }
    }
    if ( !out.vendor.empty() ) return out;
  }
  switch ( vid ) {
    case 0x10de: out.vendor = "NVIDIA"; break;
    case 0x1002: out.vendor = "AMD/ATI"; break;
    case 0x8086: out.vendor = "Intel"; break;
    case 0x1af4: out.vendor = "Red Hat (virtio)"; break;
    case 0x1b36: out.vendor = "Red Hat (QEMU)"; break;
    case 0x15ad: out.vendor = "VMware"; break;
    case 0x80ee: out.vendor = "VirtualBox"; break;
    case 0x1414: out.vendor = "Microsoft"; break;
    case 0x1234: out.vendor = "QEMU/Bochs"; break;
    default: break;
  }
  return out;
}

Section sectionGpu(bool) {
  Builder b("gpu", "Graphics");
  unsigned n = 0;
  const fs::path pci {"/sys/bus/pci/devices"};
  for ( const auto& name : listDir(pci) ) {
    const fs::path dir = pci / name;
    if ( !readFirstLine(dir / "class").starts_with("0x03") ) continue;  // display controllers
    const unsigned vid = static_cast<unsigned>(parseU64(readFirstLine(dir / "vendor"), 16).value_or(0));
    const unsigned did = static_cast<unsigned>(parseU64(readFirstLine(dir / "device"), 16).value_or(0));
    std::string title;
    for ( const auto& l : readLines("/proc/driver/nvidia/gpus/" + name + "/information", 32) )
      if ( l.starts_with("Model:") ) title = trim(l.substr(6));
    if ( title.empty() ) {
      const auto names = lookupPci(vid, did);
      title = trim(names.vendor + " " + names.device);
      if ( title.empty() ) title = "PCI device " + hex4(vid) + ":" + hex4(did);
    }
    std::vector<std::string> parts {title};
    if ( const auto drv = linkTargetName(dir / "driver"); !drv.empty() ) parts.push_back("driver " + drv);
    if ( const auto vram = parseU64(readFirstLine(dir / "mem_info_vram_total")); vram && *vram ) parts.push_back("VRAM " + formatBytes(*vram));
    b.add("gpu_" + std::to_string(n), "GPU " + std::to_string(n), join(parts, ", "));
    ++n;
  }
  if ( n == 0 ) {  // SoC graphics without a PCI device
    for ( const auto& name : listDir("/sys/class/drm") ) {
      if ( !isIndexed(name, "card") ) continue;
      const auto drv = linkTargetName(fs::path("/sys/class/drm") / name / "device/driver");
      if ( drv.empty() ) continue;
      b.add("gpu_" + std::to_string(n), "GPU " + std::to_string(n), "display device " + name + ", driver " + drv);
      ++n;
    }
  }
  if ( n == 0 ) b.add("gpu_0", "GPU", "none detected");
  if ( const auto v = readFirstLine("/proc/driver/nvidia/version"); v.starts_with("NVRM version:") )
    b.add("gpu_nvidia_driver", "NVIDIA driver", trim(v.substr(13)));
  return b.take();
}

Section sectionNetwork(bool verbose) {
  Builder b("network", "Network");

  std::map<std::string, std::vector<std::string>> addrs;
  {
    ifaddrs* raw {nullptr};
    if ( getifaddrs(&raw) == 0 ) {
      std::unique_ptr<ifaddrs, decltype(&freeifaddrs)> guard(raw, &freeifaddrs);
      for ( const ifaddrs* p = raw; p; p = p->ifa_next ) {
        if ( !p->ifa_addr || !p->ifa_name ) continue;
        char buf[INET6_ADDRSTRLEN] {};
        if ( p->ifa_addr->sa_family == AF_INET ) {
          const auto* a = reinterpret_cast<const sockaddr_in*>(p->ifa_addr);
          if ( !inet_ntop(AF_INET, &a->sin_addr, buf, sizeof buf) ) continue;
          int prefix = 0;
          if ( p->ifa_netmask ) prefix = std::popcount(reinterpret_cast<const sockaddr_in*>(p->ifa_netmask)->sin_addr.s_addr);
          addrs[p->ifa_name].push_back(std::string(buf) + "/" + std::to_string(prefix));
        } else if ( p->ifa_addr->sa_family == AF_INET6 ) {
          const auto* a = reinterpret_cast<const sockaddr_in6*>(p->ifa_addr);
          if ( !verbose && IN6_IS_ADDR_LINKLOCAL(&a->sin6_addr) ) continue;
          if ( !inet_ntop(AF_INET6, &a->sin6_addr, buf, sizeof buf) ) continue;
          int prefix = 0;
          if ( p->ifa_netmask )
            for ( unsigned char byte : reinterpret_cast<const sockaddr_in6*>(p->ifa_netmask)->sin6_addr.s6_addr ) prefix += std::popcount(byte);
          addrs[p->ifa_name].push_back(std::string(buf) + "/" + std::to_string(prefix));
        }
      }
    }
  }

  const fs::path root {"/sys/class/net"};
  for ( const auto& name : listDir(root) ) {
    if ( !verbose ) {
      static constexpr std::array<std::string_view, 9> noisy {"lo", "veth", "docker", "br-", "virbr", "vnet", "cni", "flannel", "cali"};
      if ( std::ranges::any_of(noisy, [&](std::string_view p) { return name == p || (p != "lo" && name.starts_with(p)); }) ) continue;
    }
    const fs::path dir = root / name;
    std::vector<std::string> common;
    const std::string state = readFirstLine(dir / "operstate");
    common.push_back(state.empty() ? "state unknown" : state);
    if ( pathExists(dir / "wireless") ) common.push_back("wireless");
    if ( const auto speed = parseU64(readFirstLine(dir / "speed")); speed && readFirstLine(dir / "speed")[0] != '-' && *speed > 0 )
      common.push_back(std::to_string(*speed) + " Mb/s");
    if ( const auto mtu = readFirstLine(dir / "mtu"); !mtu.empty() ) common.push_back("MTU " + mtu);
    if ( const auto drv = linkTargetName(dir / "device/driver"); !drv.empty() ) common.push_back("driver " + drv);

    std::vector<std::string> priv;
    const std::string mac = readFirstLine(dir / "address");
    if ( !mac.empty() && mac != "00:00:00:00:00:00" ) priv.push_back("MAC " + mac);
    if ( const auto it = addrs.find(name); it != addrs.end() ) priv.insert(priv.end(), it->second.begin(), it->second.end());

    std::vector<std::string> full = common;
    full.insert(full.end(), priv.begin(), priv.end());
    b.add("net_" + sanitizeKey(name), "Interface " + name, join(full, ", "), !priv.empty(), join(common, ", "));
  }
  if ( b.empty() ) b.add("net", "Interfaces", "none detected");
  return b.take();
}

Section sectionSensors(bool) {
  Builder b("sensors", "Power and temperature");

  // batteries and mains adapters
  const fs::path ps {"/sys/class/power_supply"};
  for ( const auto& name : listDir(ps) ) {
    const fs::path dir = ps / name;
    const std::string type = readFirstLine(dir / "type");
    if ( type == "Battery" ) {
      std::vector<std::string> parts;
      if ( auto c = readFirstLine(dir / "capacity"); !c.empty() ) parts.push_back(c + "%");
      if ( auto s = readFirstLine(dir / "status"); !s.empty() ) parts.push_back(s);
      if ( auto t = readFirstLine(dir / "technology"); !t.empty() ) parts.push_back(t);
      auto full = parseU64(readFirstLine(dir / "energy_full")), design = parseU64(readFirstLine(dir / "energy_full_design"));
      if ( !full ) full = parseU64(readFirstLine(dir / "charge_full"));
      if ( !design ) design = parseU64(readFirstLine(dir / "charge_full_design"));
      if ( full && design && *design ) parts.push_back("health " + fixed(100.0 * static_cast<double>(*full) / static_cast<double>(*design), 0) + "%");
      if ( auto cyc = readFirstLine(dir / "cycle_count"); !cyc.empty() && cyc != "0" ) parts.push_back(cyc + " cycles");
      if ( auto m = readFirstLine(dir / "model_name"); !m.empty() ) parts.push_back(m);
      b.add("battery_" + sanitizeKey(name), "Battery " + name, join(parts, ", "));
    } else if ( type == "Mains" ) {
      b.add("ac_" + sanitizeKey(name), "AC adapter " + name, readFirstLine(dir / "online") == "1" ? "connected" : "not connected");
    }
  }

  // temperatures: hwmon chips, else thermal zones
  auto celsius = [](const std::string& milli) -> std::string {
    const auto d = parseDouble(milli);
    return d ? fixed(*d / 1000.0, 1) + " \xC2\xB0" "C" : std::string {};
  };
  std::set<std::string> usedKeys;
  const fs::path hw {"/sys/class/hwmon"};
  for ( const auto& name : listDir(hw) ) {
    const fs::path dir = hw / name;
    const std::string chip = readFirstLine(dir / "name");
    const std::string t = celsius(readFirstLine(dir / "temp1_input"));
    if ( chip.empty() || t.empty() ) continue;
    const std::string label = readFirstLine(dir / "temp1_label");
    std::string key = "temp_" + sanitizeKey(chip);
    for ( int n = 2; !usedKeys.insert(key).second; ++n ) key = "temp_" + sanitizeKey(chip) + "_" + std::to_string(n);
    b.add(key, "Temperature " + chip, t + (label.empty() ? "" : " (" + label + ")"));
  }
  if ( usedKeys.empty() ) {
    const fs::path tz {"/sys/class/thermal"};
    for ( const auto& name : listDir(tz) ) {
      if ( !isIndexed(name, "thermal_zone") ) continue;
      const std::string t = celsius(readFirstLine(tz / name / "temp"));
      if ( t.empty() ) continue;
      b.add("temp_" + sanitizeKey(name), "Temperature " + readFirstLine(tz / name / "type"), t);
    }
  }
  if ( b.empty() ) b.add("sensors", "Sensors", "none exposed (virtual machine or container?)");
  return b.take();
}

#elif defined(__APPLE__)
// ===========================================================================
// macOS (best effort, reduced)
// ===========================================================================

std::string sysctlString(const char* name) {
  std::size_t len {0};
  if ( sysctlbyname(name, nullptr, &len, nullptr, 0) != 0 || len == 0 ) return {};
  std::string s(len, '\0');
  if ( sysctlbyname(name, s.data(), &len, nullptr, 0) != 0 ) return {};
  s.resize(std::char_traits<char>::length(s.c_str()));
  return s;
}
std::uint64_t sysctlU64(const char* name) {
  std::uint64_t v {0};
  std::size_t len {sizeof v};
  if ( sysctlbyname(name, &v, &len, nullptr, 0) != 0 ) return 0;
  if ( len == 4 ) return static_cast<std::uint32_t>(v);  // 32-bit values
  return v;
}

Section sectionSystem(bool) {
  Builder b("system", "System");
  char host[256] {};
  if ( gethostname(host, sizeof host - 1) == 0 ) b.add("hostname", "Hostname", host, true);
  b.add("os", "Operating system", "macOS " + sysctlString("kern.osproductversion"));
  utsname u {};
  if ( uname(&u) == 0 ) {
    b.add("kernel", "Kernel", std::string(u.sysname) + " " + u.release);
    b.add("arch", "Architecture", u.machine);
  }
  b.addIf("machine", "Machine", sysctlString("hw.model"));
  return b.take();
}

Section sectionCpu(bool) {
  Builder b("cpu", "Processor");
  b.add("cpu_model", "Model", sysctlString("machdep.cpu.brand_string"));
  b.add("cpu_sockets", "Sockets", "1");
  b.add("cpu_physical_cores", "Physical cores", std::to_string(sysctlU64("hw.physicalcpu")));
  b.add("cpu_logical_cores", "Logical cores (threads)", std::to_string(sysctlU64("hw.logicalcpu")));
  if ( const auto hz = sysctlU64("hw.cpufrequency_max") ) b.add("cpu_max_mhz", "Max frequency (MHz)", std::to_string(hz / 1000000));
  std::vector<std::string> caches;
  auto cache = [&](const char* tag, const char* key) {
    if ( const auto bytes = sysctlU64(key) ) caches.push_back(std::string(tag) + " " + std::to_string(bytes / 1024) + "K");
  };
  cache("L1d", "hw.l1dcachesize");
  cache("L1i", "hw.l1icachesize");
  cache("L2", "hw.l2cachesize");
  cache("L3", "hw.l3cachesize");
  b.addIf("cpu_caches", "Caches", join(caches, ", "));
  return b.take();
}

Section sectionMemory(bool) {
  Builder b("memory", "Memory");
  const auto mem = sysctlU64("hw.memsize");
  b.add("ram_total", "Total RAM", mem ? formatBytes(mem) : std::string {});
  return b.take();
}

Section sectionStorage(bool) { return unsupported("storage", "Storage"); }
Section sectionGpu(bool) { return unsupported("gpu", "Graphics"); }
Section sectionNetwork(bool) { return unsupported("network", "Network"); }
Section sectionSensors(bool) { return unsupported("sensors", "Power and temperature"); }

#elif defined(_WIN32)
// ===========================================================================
// Windows (best effort, reduced)
// ===========================================================================

Section sectionSystem(bool) {
  Builder b("system", "System");
  using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
  std::string os {"Windows"};
  if ( HMODULE ntdll = GetModuleHandleA("ntdll.dll") ) {
    if ( auto fn = reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion"))) ) {
      OSVERSIONINFOW vi {};
      vi.dwOSVersionInfoSize = sizeof vi;
      if ( fn(&vi) == 0 )
        os += " " + std::to_string(vi.dwMajorVersion) + "." + std::to_string(vi.dwMinorVersion) + " (build " + std::to_string(vi.dwBuildNumber) + ")";
    }
  }
  b.add("os", "Operating system", os);
  SYSTEM_INFO si {};
  GetNativeSystemInfo(&si);
  const char* arch = si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ? "x86_64"
                   : si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64 ? "arm64" : "other";
  b.add("arch", "Architecture", arch);
  return b.take();
}

Section sectionCpu(bool) {
  Builder b("cpu", "Processor");
  char name[256] {};
  DWORD size {sizeof name};
  if ( RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString",
                    RRF_RT_REG_SZ, nullptr, name, &size) == ERROR_SUCCESS )
    b.add("cpu_model", "Model", trim(name));
  else
    b.add("cpu_model", "Model", {});
  SYSTEM_INFO si {};
  GetNativeSystemInfo(&si);
  b.add("cpu_logical_cores", "Logical cores (threads)", std::to_string(si.dwNumberOfProcessors));
  return b.take();
}

Section sectionMemory(bool) {
  Builder b("memory", "Memory");
  MEMORYSTATUSEX ms {};
  ms.dwLength = sizeof ms;
  b.add("ram_total", "Total RAM", GlobalMemoryStatusEx(&ms) ? formatBytes(ms.ullTotalPhys) : std::string {});
  return b.take();
}

Section sectionStorage(bool) { return unsupported("storage", "Storage"); }
Section sectionGpu(bool) { return unsupported("gpu", "Graphics"); }
Section sectionNetwork(bool) { return unsupported("network", "Network"); }
Section sectionSensors(bool) { return unsupported("sensors", "Power and temperature"); }

#else
// ===========================================================================
// Unknown platform
// ===========================================================================

Section sectionSystem(bool) {
  Builder b("system", "System");
  b.add("os", "Operating system", {});
  return b.take();
}
Section sectionCpu(bool) {
  Builder b("cpu", "Processor");
  b.add("cpu_logical_cores", "Logical cores (threads)", std::to_string(std::thread::hardware_concurrency()));
  return b.take();
}
Section sectionMemory(bool) { return unsupported("memory", "Memory"); }
Section sectionStorage(bool) { return unsupported("storage", "Storage"); }
Section sectionGpu(bool) { return unsupported("gpu", "Graphics"); }
Section sectionNetwork(bool) { return unsupported("network", "Network"); }
Section sectionSensors(bool) { return unsupported("sensors", "Power and temperature"); }
#endif

}  // namespace

// ===========================================================================
// Public API
// ===========================================================================

const std::vector<SectionInfo>& sections() {
  static const std::vector<SectionInfo> list {
      {"system", "System", true},
      {"cpu", "Processor", true},
      {"memory", "Memory", true},
      {"storage", "Storage", false},
      {"gpu", "Graphics", false},
      {"network", "Network", false},
      {"sensors", "Power and temperature", false},
      {"environment", "Environment", false},
      {"build", "Build", true},
  };
  return list;
}

Section collectSection(std::string_view id, bool verbose) {
  const auto& list = sections();
  const auto it = std::ranges::find(list, id, &SectionInfo::id);
  if ( it == list.end() ) return {};
  try {
    if ( id == "system" ) return sectionSystem(verbose);
    if ( id == "cpu" ) return sectionCpu(verbose);
    if ( id == "memory" ) return sectionMemory(verbose);
    if ( id == "storage" ) return sectionStorage(verbose);
    if ( id == "gpu" ) return sectionGpu(verbose);
    if ( id == "network" ) return sectionNetwork(verbose);
    if ( id == "sensors" ) return sectionSensors(verbose);
    if ( id == "environment" ) return sectionEnvironment(verbose);
    if ( id == "build" ) return sectionBuild(verbose);
  } catch ( ... ) {
    // information is best effort; a parse problem must not stop the caller
  }
  Section failed {it->id, it->title, {}};
  failed.entries.push_back(Entry {"sys_" + it->id + "_error", "Status", "could not be collected", {}, false});
  return failed;
}

std::vector<Section> collectAll(bool verbose) {
  std::vector<Section> out;
  for ( const auto& s : sections() ) out.push_back(collectSection(s.id, verbose));
  return out;
}

KeyValues collect() {
  KeyValues kv;
  for ( const auto& info : sections() ) {
    if ( !info.core ) continue;
    for ( auto& e : collectSection(info.id, false).entries ) kv.emplace_back(std::move(e.key), std::move(e.value));
  }
  return kv;
}

}  // namespace sysinfo
