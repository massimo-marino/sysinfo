// main.cpp -- `sysinfo`: print the specifications of the machine you are on.
//
//   sysinfo                      all sections, human readable
//   sysinfo -s cpu,memory        selected sections only
//   sysinfo -f json -o hw.json   machine readable, written to a file
//   sysinfo --help               everything else
//
// The data is collected by the sysinfo library (sysinfo.h); this file only deals
// with the command line, selecting sections and formatting the result.
#include "sysinfo.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <ostream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
  #include <io.h>
  #define SYSINFO_ISATTY(fd) _isatty(fd)
  #define SYSINFO_STDOUT_FD 1
#else
  #include <unistd.h>
  #define SYSINFO_ISATTY(fd) isatty(fd)
  #define SYSINFO_STDOUT_FD STDOUT_FILENO
#endif

namespace {

// Exit codes (documented in the manual).
constexpr int kExitOk = 0;
constexpr int kExitIo = 1;
constexpr int kExitUsage = 2;

enum class Format { Text, Json, Kv };
enum class ColorMode { Auto, Always, Never };

struct Options {
  Format format {Format::Text};
  ColorMode color {ColorMode::Auto};
  std::vector<std::string> only;
  std::vector<std::string> exclude;
  std::string output;  // empty = standard output
  bool verbose {false};
  bool redact {false};
  bool list {false};
  bool help {false};
  bool version {false};
};

struct UsageError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

constexpr std::string_view kUsage =
    R"(Usage: sysinfo [OPTIONS]

Print the hardware and software specifications of this machine.

Selection:
  -s, --section LIST     only these sections (comma separated, repeatable);
                         names: system cpu memory storage gpu network sensors
                         environment build; also "core" and "all"
  -x, --exclude LIST     leave these sections out (same syntax)
  -l, --list             list the available sections and exit
  -v, --verbose          include the long tail: all CPU flags, pseudo
                         filesystems, virtual network interfaces, ...

Output:
  -f, --format FORMAT    text (default), json or kv (key=value lines)
  -j, --json             same as --format json
  -o, --output FILE      write to FILE instead of standard output
  -r, --redact           hide hostname, user name, MAC and IP addresses
      --color[=WHEN]     colour the text format: auto (default), always, never
      --no-color         same as --color=never (NO_COLOR is honoured as well)

Other:
  -h, --help             show this help and exit
  -V, --version          show the version and exit

Exit status: 0 success, 1 output could not be written, 2 usage error.
)";

// ---------------------------------------------------------------------------
// Command line
// ---------------------------------------------------------------------------

std::vector<std::string> splitList(std::string_view s) {
  std::vector<std::string> out;
  std::size_t i = 0;
  while ( i <= s.size() ) {
    const auto comma = s.find(',', i);
    std::string item(s.substr(i, comma == std::string_view::npos ? std::string_view::npos : comma - i));
    std::erase_if(item, [](char c) { return c == ' ' || c == '\t'; });
    for ( char& c : item ) if ( c >= 'A' && c <= 'Z' ) c = static_cast<char>(c - 'A' + 'a');
    if ( !item.empty() ) out.push_back(std::move(item));
    if ( comma == std::string_view::npos ) break;
    i = comma + 1;
  }
  return out;
}

Format parseFormat(std::string_view v) {
  if ( v == "text" ) return Format::Text;
  if ( v == "json" ) return Format::Json;
  if ( v == "kv" ) return Format::Kv;
  throw UsageError("unknown format '" + std::string(v) + "' (expected text, json or kv)");
}

ColorMode parseColor(std::string_view v) {
  if ( v == "auto" ) return ColorMode::Auto;
  if ( v == "always" || v == "yes" ) return ColorMode::Always;
  if ( v == "never" || v == "no" ) return ColorMode::Never;
  throw UsageError("unknown colour mode '" + std::string(v) + "' (expected auto, always or never)");
}

Options parseArgs(int argc, char** argv) {
  Options o;
  auto append = [](std::vector<std::string>& dst, std::string_view list) {
    auto items = splitList(list);
    dst.insert(dst.end(), items.begin(), items.end());
  };

  for ( int i = 1; i < argc; ++i ) {
    const std::string_view arg = argv[i];

    if ( arg.starts_with("--") && arg.size() > 2 ) {
      const auto eq = arg.find('=');
      const std::string_view name = arg.substr(2, eq == std::string_view::npos ? std::string_view::npos : eq - 2);
      const bool hasValue = eq != std::string_view::npos;
      const std::string_view inlineValue = hasValue ? arg.substr(eq + 1) : std::string_view {};
      auto value = [&]() -> std::string_view {
        if ( hasValue ) return inlineValue;
        if ( i + 1 >= argc ) throw UsageError("option '--" + std::string(name) + "' needs a value");
        return argv[++i];
      };
      auto noValue = [&] {
        if ( hasValue ) throw UsageError("option '--" + std::string(name) + "' takes no value");
      };

      if ( name == "help" ) { noValue(); o.help = true; }
      else if ( name == "version" ) { noValue(); o.version = true; }
      else if ( name == "list" ) { noValue(); o.list = true; }
      else if ( name == "verbose" ) { noValue(); o.verbose = true; }
      else if ( name == "redact" ) { noValue(); o.redact = true; }
      else if ( name == "json" ) { noValue(); o.format = Format::Json; }
      else if ( name == "no-color" || name == "no-colour" ) { noValue(); o.color = ColorMode::Never; }
      else if ( name == "color" || name == "colour" ) o.color = hasValue ? parseColor(inlineValue) : ColorMode::Always;
      else if ( name == "format" ) o.format = parseFormat(value());
      else if ( name == "section" || name == "sections" ) append(o.only, value());
      else if ( name == "exclude" ) append(o.exclude, value());
      else if ( name == "output" ) o.output = std::string(value());
      else throw UsageError("unknown option '--" + std::string(name) + "'");

    } else if ( arg.size() > 1 && arg[0] == '-' && arg[1] != '-' ) {
      for ( std::size_t j = 1; j < arg.size(); ++j ) {
        const char c = arg[j];
        // value of an option that takes one: rest of this word, or the next word
        auto value = [&]() -> std::string_view {
          if ( j + 1 < arg.size() ) {
            const auto v = arg.substr(j + 1);
            j = arg.size();
            return v;
          }
          if ( i + 1 >= argc ) throw UsageError(std::string("option '-") + c + "' needs a value");
          return argv[++i];
        };
        switch ( c ) {
          case 'h': o.help = true; break;
          case 'V': o.version = true; break;
          case 'l': o.list = true; break;
          case 'v': o.verbose = true; break;
          case 'r': o.redact = true; break;
          case 'j': o.format = Format::Json; break;
          case 'f': o.format = parseFormat(value()); break;
          case 's': append(o.only, value()); break;
          case 'x': append(o.exclude, value()); break;
          case 'o': o.output = std::string(value()); break;
          default: throw UsageError(std::string("unknown option '-") + c + "'");
        }
      }
    } else {
      throw UsageError("unexpected argument '" + std::string(arg) + "'");
    }
  }
  return o;
}

// Expands "core"/"all", validates the names and returns the ids to collect, in display order.
std::vector<std::string> selectSections(const Options& o) {
  const auto& known = sysinfo::sections();
  auto expand = [&](const std::vector<std::string>& names) {
    std::set<std::string> ids;
    for ( const auto& n : names ) {
      if ( n == "all" ) {
        for ( const auto& s : known ) ids.insert(s.id);
      } else if ( n == "core" ) {
        for ( const auto& s : known ) if ( s.core ) ids.insert(s.id);
      } else if ( std::ranges::any_of(known, [&](const auto& s) { return s.id == n; }) ) {
        ids.insert(n);
      } else {
        std::string valid;
        for ( const auto& s : known ) valid += (valid.empty() ? "" : ", ") + s.id;
        throw UsageError("unknown section '" + n + "' (valid: " + valid + ", core, all)");
      }
    }
    return ids;
  };
  const auto only = expand(o.only);
  const auto excluded = expand(o.exclude);

  std::vector<std::string> out;
  for ( const auto& s : known ) {
    if ( !only.empty() && !only.contains(s.id) ) continue;
    if ( excluded.contains(s.id) ) continue;
    out.push_back(s.id);
  }
  return out;
}

bool useColor(const Options& o) {
  if ( o.format != Format::Text ) return false;
  switch ( o.color ) {
    case ColorMode::Always: return true;
    case ColorMode::Never: return false;
    case ColorMode::Auto: break;
  }
  if ( !o.output.empty() || !SYSINFO_ISATTY(SYSINFO_STDOUT_FD) ) return false;
  if ( const char* nc = std::getenv("NO_COLOR"); nc && *nc ) return false;
  const char* term = std::getenv("TERM");
  return !(term && std::string_view(term) == "dumb");
}

// ---------------------------------------------------------------------------
// Renderers
// ---------------------------------------------------------------------------

void renderText(std::ostream& os, const std::vector<sysinfo::Section>& secs, bool redact, bool color) {
  const char* bold = color ? "\033[1;36m" : "";
  const char* dim = color ? "\033[2m" : "";
  const char* reset = color ? "\033[0m" : "";

  bool first = true;
  for ( const auto& s : secs ) {
    if ( !first ) os << '\n';
    first = false;
    os << bold << s.title << reset << '\n' << bold << std::string(s.title.size(), '-') << reset << '\n';
    std::size_t width = 0;
    for ( const auto& e : s.entries ) width = std::max(width, e.label.size());
    for ( const auto& e : s.entries ) {
      os << "  " << dim << e.label << std::string(width - e.label.size(), ' ') << reset << "  " << e.display(redact) << '\n';
    }
  }
}

std::string jsonEscape(std::string_view s) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out {'"'};
  for ( std::size_t i = 0; i < s.size(); ) {
    const auto c = static_cast<unsigned char>(s[i]);
    if ( c < 0x80 ) {
      switch ( c ) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
          if ( c < 0x20 || c == 0x7f ) { out += "\\u00"; out += kHex[c >> 4]; out += kHex[c & 15]; }
          else out += static_cast<char>(c);
      }
      ++i;
      continue;
    }
    // multi-byte UTF-8: keep if well formed, else substitute U+FFFD (files in /sys may hold garbage)
    const std::size_t len = c >= 0xf0 && c < 0xf8 ? 4 : c >= 0xe0 ? 3 : c >= 0xc2 ? 2 : 0;
    bool ok = len != 0 && c < 0xf8 && i + len <= s.size();
    for ( std::size_t k = 1; ok && k < len; ++k ) ok = (static_cast<unsigned char>(s[i + k]) & 0xc0) == 0x80;
    if ( ok ) { out.append(s.substr(i, len)); i += len; }
    else { out += "\xEF\xBF\xBD"; ++i; }
  }
  out += '"';
  return out;
}

void renderJson(std::ostream& os, const std::vector<sysinfo::Section>& secs, bool redact) {
  os << "{\n  \"sysinfo\": " << jsonEscape(sysinfo::version()) << ",\n  \"redacted\": " << (redact ? "true" : "false")
     << ",\n  \"sections\": {";
  for ( std::size_t i = 0; i < secs.size(); ++i ) {
    const auto& s = secs[i];
    os << (i ? ",\n    " : "\n    ") << jsonEscape(s.id) << ": {";
    for ( std::size_t k = 0; k < s.entries.size(); ++k ) {
      const auto& e = s.entries[k];
      os << (k ? ",\n      " : "\n      ") << jsonEscape(e.key) << ": " << jsonEscape(e.display(redact));
    }
    os << (s.entries.empty() ? "}" : "\n    }");
  }
  os << (secs.empty() ? "}" : "\n  }") << "\n}\n";
}

void renderKv(std::ostream& os, const std::vector<sysinfo::Section>& secs, bool redact) {
  for ( const auto& s : secs )
    for ( const auto& e : s.entries ) os << e.key << '=' << e.display(redact) << '\n';
}

void renderList(std::ostream& os) {
  for ( const auto& s : sysinfo::sections() ) os << s.id << '\t' << s.title << (s.core ? "\t(core)" : "") << '\n';
}

}  // namespace

int main(int argc, char** argv) {
  Options opts;
  std::vector<std::string> ids;
  try {
    opts = parseArgs(argc, argv);
    if ( !opts.help && !opts.version && !opts.list ) ids = selectSections(opts);
  } catch ( const UsageError& e ) {
    std::cerr << "sysinfo: " << e.what() << "\nTry 'sysinfo --help' for more information.\n";
    return kExitUsage;
  }

  if ( opts.help ) { std::cout << kUsage; return kExitOk; }
  if ( opts.version ) { std::cout << "sysinfo " << sysinfo::version() << '\n'; return kExitOk; }
  if ( opts.list ) { renderList(std::cout); return kExitOk; }

  std::vector<sysinfo::Section> secs;
  secs.reserve(ids.size());
  for ( const auto& id : ids ) secs.push_back(sysinfo::collectSection(id, opts.verbose));

  std::ofstream file;
  std::ostream* out = &std::cout;
  if ( !opts.output.empty() ) {
    file.open(opts.output, std::ios::out | std::ios::trunc);
    if ( !file ) {
      std::cerr << "sysinfo: cannot open '" << opts.output << "' for writing\n";
      return kExitIo;
    }
    out = &file;
  }

  switch ( opts.format ) {
    case Format::Text: renderText(*out, secs, opts.redact, useColor(opts)); break;
    case Format::Json: renderJson(*out, secs, opts.redact); break;
    case Format::Kv: renderKv(*out, secs, opts.redact); break;
  }
  out->flush();
  if ( !*out ) {
    std::cerr << "sysinfo: error while writing output\n";
    return kExitIo;
  }
  return kExitOk;
}
