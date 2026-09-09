#include "config.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <yaml-cpp/yaml.h>

namespace markit {

namespace {

const std::unordered_map<std::string, ftxui::Color>& NamedColors() {
  static const std::unordered_map<std::string, ftxui::Color> kColors = {
      {"black", ftxui::Color::Black},
      {"red", ftxui::Color::Red},
      {"green", ftxui::Color::Green},
      {"yellow", ftxui::Color::Yellow},
      {"blue", ftxui::Color::Blue},
      {"magenta", ftxui::Color::Magenta},
      {"cyan", ftxui::Color::Cyan},
      {"graylight", ftxui::Color::GrayLight},
      {"graydark", ftxui::Color::GrayDark},
      {"redlight", ftxui::Color::RedLight},
      {"greenlight", ftxui::Color::GreenLight},
      {"yellowlight", ftxui::Color::YellowLight},
      {"bluelight", ftxui::Color::BlueLight},
      {"magentalight", ftxui::Color::MagentaLight},
      {"cyanlight", ftxui::Color::CyanLight},
      {"white", ftxui::Color::White},
  };
  return kColors;
}

int HexDigit(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

// Convert "#888" to "#888888" so both long and short forms parse alike.
std::string ExpandHexShorthand(const std::string& s) {
  std::string out = "#";
  for (size_t i = 1; i < s.size(); ++i) {
    out += s[i];
    out += s[i];
  }
  return out;
}

std::string Lowercase(std::string s) {
  for (auto& c : s) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return s;
}

void LookupColor(const std::string& key, const YAML::Node& value,
                 ftxui::Color& out, const std::string& prefix) {
  const std::string path = prefix + "." + key;
  if (!value.IsScalar()) {
    throw std::runtime_error(
        "config: " + path + ": expected a color string, got a non-scalar");
  }
  auto parsed = ParseColor(value.Scalar());
  if (!parsed.has_value()) {
    throw std::runtime_error("config: " + path + ": invalid color '" +
                             value.Scalar() +
                             "' (expected a named color or #rrggbb/#rgb)");
  }
  out = *parsed;
}

// Validate `theme` mapping against the embedded schema, filling `theme` with
// the colors found (unset fields keep their defaults). Any unknown key, wrong
// type, or unparseable color throws std::runtime_error with a dotted path.
void ValidateTheme(const YAML::Node& theme, Theme& out) {
  static const std::unordered_set<std::string> kKnownTopLevel = {
      "heading", "link", "inline_code", "code_block", "quote_marker",
  };
  static const std::unordered_set<std::string> kKnownHeading = {
      "h1", "h2", "h3", "h4",
  };
  static const std::unordered_set<std::string> kKnownPair = {
      "fg", "bg",
  };

  if (!theme.IsMap()) {
    throw std::runtime_error(
        "config: theme: expected a mapping of color settings");
  }

  for (auto it = theme.begin(); it != theme.end(); ++it) {
    if (!it->first.IsScalar()) {
      throw std::runtime_error("config: theme: expected string keys");
    }
    const std::string key = it->first.Scalar();
    const YAML::Node value = it->second;

    if (key == "heading") {
      if (!value.IsMap()) {
        throw std::runtime_error(
            "config: theme.heading: expected a mapping {h1..h4}");
      }
      for (auto h = value.begin(); h != value.end(); ++h) {
        if (!h->first.IsScalar()) {
          throw std::runtime_error(
              "config: theme.heading: expected string keys");
        }
        const std::string hk = h->first.Scalar();
        if (kKnownHeading.count(hk) == 0) {
          throw std::runtime_error("config: theme.heading." + hk +
                                   ": unknown key");
        }
        if (hk == "h1") {
          LookupColor(hk, h->second, out.heading_h1, "theme.heading");
        } else if (hk == "h2") {
          LookupColor(hk, h->second, out.heading_h2, "theme.heading");
        } else if (hk == "h3") {
          LookupColor(hk, h->second, out.heading_h3, "theme.heading");
        } else {
          LookupColor(hk, h->second, out.heading_h4, "theme.heading");
        }
      }
    } else if (key == "inline_code" || key == "code_block") {
      if (!value.IsMap()) {
        throw std::runtime_error("config: theme." + key +
                                 ": expected a mapping {fg, bg}");
      }
      for (auto p = value.begin(); p != value.end(); ++p) {
        if (!p->first.IsScalar()) {
          throw std::runtime_error("config: theme." + key +
                                   ": expected string keys");
        }
        const std::string pk = p->first.Scalar();
        if (kKnownPair.count(pk) == 0) {
          throw std::runtime_error("config: theme." + key + "." + pk +
                                   ": unknown key");
        }
        if (key == "inline_code") {
          if (pk == "fg") {
            LookupColor(pk, p->second, out.inline_code_fg, "theme.inline_code");
          } else {
            LookupColor(pk, p->second, out.inline_code_bg, "theme.inline_code");
          }
        } else {
          if (pk == "fg") {
            LookupColor(pk, p->second, out.code_block_fg, "theme.code_block");
          } else {
            LookupColor(pk, p->second, out.code_block_bg, "theme.code_block");
          }
        }
      }
    } else if (key == "link") {
      LookupColor(key, value, out.link, "theme");
    } else if (key == "quote_marker") {
      LookupColor(key, value, out.quote_marker, "theme");
    } else if (kKnownTopLevel.count(key) == 0) {
      throw std::runtime_error("config: theme." + key + ": unknown key");
    }
  }
}

// Validate the top-level `display` mapping against the embedded schema,
// filling `cfg` with the values found (unset fields keep their defaults). Any
// unknown key or unparseable value throws std::runtime_error with a dotted
// path, matching the theme validation style.
void ValidateDisplay(const YAML::Node& display, Config& cfg) {
  static const std::unordered_set<std::string> kKnown = {
      "horizontal",
      "navigation",
  };

  if (!display.IsMap()) {
    throw std::runtime_error(
        "config: display: expected a mapping of display settings");
  }

  for (auto it = display.begin(); it != display.end(); ++it) {
    if (!it->first.IsScalar()) {
      throw std::runtime_error("config: display: expected string keys");
    }
    const std::string key = it->first.Scalar();
    const YAML::Node value = it->second;

    if (key == "horizontal") {
      if (!value.IsScalar()) {
        throw std::runtime_error(
            "config: display.horizontal: expected a string (wrap or scroll)");
      }
      const std::string mode = Lowercase(value.Scalar());
      if (mode == "wrap") {
        cfg.horizontal_wrap = WrapMode::Wrap;
      } else if (mode == "scroll") {
        cfg.horizontal_wrap = WrapMode::Scroll;
      } else {
        throw std::runtime_error(
            "config: display.horizontal: invalid value '" + value.Scalar() +
            "' (expected 'wrap' or 'scroll')");
      }
    } else if (key == "navigation") {
      if (!value.IsScalar()) {
        throw std::runtime_error(
            "config: display.navigation: expected a string (visible or "
            "hidden)");
      }
      const std::string visibility = Lowercase(value.Scalar());
      if (visibility == "visible") {
        cfg.nav_visible = true;
      } else if (visibility == "hidden") {
        cfg.nav_visible = false;
      } else {
        throw std::runtime_error(
            "config: display.navigation: invalid value '" + value.Scalar() +
            "' (expected 'visible' or 'hidden')");
      }
    } else if (kKnown.count(key) == 0) {
      throw std::runtime_error("config: display." + key + ": unknown key");
    }
  }
}

// Validate the top-level `search` mapping against the embedded schema,
// filling `cfg` with the values found (unset fields keep their defaults). Any
// unknown key or unparseable value throws std::runtime_error with a dotted
// path, matching the display validation style.
void ValidateSearch(const YAML::Node& search, Config& cfg) {
  static const std::unordered_set<std::string> kKnown = {
      "case_sensitive",
  };

  if (!search.IsMap()) {
    throw std::runtime_error(
        "config: search: expected a mapping of search settings");
  }

  for (auto it = search.begin(); it != search.end(); ++it) {
    if (!it->first.IsScalar()) {
      throw std::runtime_error("config: search: expected string keys");
    }
    const std::string key = it->first.Scalar();
    const YAML::Node value = it->second;

    if (key == "case_sensitive") {
      if (!value.IsScalar()) {
        throw std::runtime_error(
            "config: search.case_sensitive: expected a boolean (true or "
            "false)");
      }
      const std::string flag = Lowercase(value.Scalar());
      if (flag == "true") {
        cfg.search_case_sensitive = true;
      } else if (flag == "false") {
        cfg.search_case_sensitive = false;
      } else {
        throw std::runtime_error(
            "config: search.case_sensitive: invalid value '" + value.Scalar() +
            "' (expected 'true' or 'false')");
      }
    } else if (kKnown.count(key) == 0) {
      throw std::runtime_error("config: search." + key + ": unknown key");
    }
  }
}

}  // namespace

std::optional<ftxui::Color> ParseColor(const std::string& in) {
  const std::string s = Lowercase(in);

  if (!s.empty() && s[0] == '#') {
    std::string hex =
        (s.size() == 4) ? ExpandHexShorthand(s) : s;  // #rgb -> #rrggbb
    if (hex.size() != 7) {
      return std::nullopt;
    }
    int r = HexDigit(hex[1]);
    int g = HexDigit(hex[3]);
    int b = HexDigit(hex[5]);
    if (r < 0 || g < 0 || b < 0 ||
        HexDigit(hex[2]) < 0 || HexDigit(hex[4]) < 0 || HexDigit(hex[6]) < 0) {
      return std::nullopt;
    }
    return ftxui::Color::RGB(static_cast<uint8_t>(r * 16 + HexDigit(hex[2])),
                             static_cast<uint8_t>(g * 16 + HexDigit(hex[4])),
                             static_cast<uint8_t>(b * 16 + HexDigit(hex[6])));
  }

  auto it = NamedColors().find(s);
  if (it == NamedColors().end()) {
    return std::nullopt;
  }
  return it->second;
}

std::string DefaultConfigPath() {
  const char* home = std::getenv("HOME");
  if (home == nullptr) {
    return std::string();
  }
  return std::string(home) + "/.config/markit/markit.yml";
}

Config LoadConfig(const std::string& path) {
  Config cfg;

  if (path.empty()) {
    return cfg;
  }
  std::ifstream exists(path);
  if (!exists.is_open()) {
    return cfg;  // missing config: silently use defaults
  }
  exists.close();

  YAML::Node root = YAML::LoadFile(path);

  if (!root.IsDefined() || root.IsNull()) {
    return cfg;  // empty config file: silently use defaults.
  }
  if (!root.IsMap()) {
    throw std::runtime_error(
        "config: expected a mapping at the top level");
  }

  // Validate top-level keys (strict, matching the theme-style schema).
  for (auto it = root.begin(); it != root.end(); ++it) {
    if (!it->first.IsScalar()) {
      throw std::runtime_error("config: expected string keys at top level");
    }
    const std::string key = it->first.Scalar();
    if (key != "theme" && key != "display" && key != "search") {
      throw std::runtime_error("config: " + key + ": unknown top-level key");
    }
  }

  if (root["theme"].IsDefined()) {
    ValidateTheme(root["theme"], cfg.theme);
  }
  if (root["display"].IsDefined()) {
    ValidateDisplay(root["display"], cfg);
  }
  if (root["search"].IsDefined()) {
    ValidateSearch(root["search"], cfg);
  }
  return cfg;
}

// Map a named Color back to its canonical lowercase name, used when emitting
// the default config. Returns an empty string for non-palette colors (which
// the default theme never contains).
std::string ColorName(ftxui::Color color) {
  for (const auto& [name, c] : NamedColors()) {
    if (c == color) {
      return name;
    }
  }
  return std::string();
}

void DumpDefaultConfig(std::ostream& out) {
  out << "# markit configuration\n"
      << "#\n"
      << "# Every color value may be either:\n"
      << "#   - a named palette color (case-insensitive):\n"
      << "#       black, red, green, yellow, blue, magenta, cyan, white,\n"
      << "#       redlight, greenlight, yellowlight, bluelight, magentalight,\n"
      << "#       cyanlight, graylight, graydark\n"
      << "#   - a hex truecolor string:\n"
      << "#       #rrggbb  (e.g. #ff8000)\n"
      << "#       #rgb     (shorthand, e.g. #f80)\n"
      << "#\n"
      << "# Unset keys fall back to these defaults. To customize, edit a value,\n"
      << "# then run:\n"
      << "#     markit --config <path>\n"
      << "theme:\n"
      << "  heading:        # heading colors per level\n"
      << "    h1: " << ColorName(Theme{}.heading_h1) << "\n"
      << "    h2: " << ColorName(Theme{}.heading_h2) << "\n"
      << "    h3: " << ColorName(Theme{}.heading_h3) << "\n"
      << "    h4: " << ColorName(Theme{}.heading_h4) << "\n"
      << "  link: " << ColorName(Theme{}.link) << "\n"
      << "  inline_code:    # inline `code` text\n"
      << "    fg: " << ColorName(Theme{}.inline_code_fg) << "\n"
      << "    bg: " << ColorName(Theme{}.inline_code_bg) << "\n"
      << "  code_block:     # fenced ``` code blocks\n"
      << "    fg: " << ColorName(Theme{}.code_block_fg) << "\n"
      << "    bg: " << ColorName(Theme{}.code_block_bg) << "\n"
      << "  quote_marker: " << ColorName(Theme{}.quote_marker) << "\n"
      << "display:\n"
      << "  horizontal: " << (Config{}.horizontal_wrap == WrapMode::Scroll
                                  ? "scroll"
                                  : "wrap")
      << "\n"
      << "  navigation: " << (Config{}.nav_visible ? "visible" : "hidden")
      << "\n"
      << "search:\n"
      << "  case_sensitive: "
      << (Config{}.search_case_sensitive ? "true" : "false") << "\n";
}

}  // namespace markit