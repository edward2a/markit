#include "config.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

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

// Key-name language for the `keybindings:` section. Canonicalize a raw key
// name to its canonical form, or nullopt when unknown:
//  - special keys (case-insensitive): up, down, left, right, pageup,
//    pagedown, home, end, tab, enter (alias return), esc (alias escape),
//    space, slash (alias /).
//  - ctrl chords: ctrl+<letter> (case-insensitive, canonicalized lowercase).
//    Only letters are accepted; whether the terminal/FTXUI can actually
//    deliver a given chord is a runtime matter, not a config error.
//  - single printable ASCII characters, case-sensitive (n vs N). A lone
//    space or slash folds to the "space"/"slash" canonical forms.
std::optional<std::string> CanonicalKeyName(const std::string& raw) {
  const std::string low = Lowercase(raw);
  if (low == "up" || low == "down" || low == "left" || low == "right" ||
      low == "pageup" || low == "pagedown" || low == "home" || low == "end" ||
      low == "tab") {
    return low;
  }
  if (low == "enter" || low == "return") {
    return "enter";
  }
  if (low == "esc" || low == "escape") {
    return "esc";
  }
  if (low == "space") {
    return "space";
  }
  if (low == "slash" || low == "/") {
    return "slash";
  }
  if (low.size() == 6 && low.compare(0, 5, "ctrl+") == 0 && low[5] >= 'a' &&
      low[5] <= 'z') {
    return low;
  }
  if (raw.size() == 1) {
    const char c = raw[0];
    if (c == ' ') {
      return "space";
    }
    if (c == '/') {
      return "slash";
    }
    if (c >= 0x21 && c <= 0x7e) {
      return std::string(1, c);
    }
  }
  return std::nullopt;
}

// Build the FTXUI event for a canonical key name (see CanonicalKeyName).
// Ctrl+letter is the raw control byte 0x01-0x1a, matching FTXUI's predefined
// Event::CtrlA..CtrlZ (Event::Special("\x01")..("\x1a")).
ftxui::Event KeyEventForCanonical(const std::string& canonical) {
  using ftxui::Event;
  if (canonical == "up") {
    return Event::ArrowUp;
  }
  if (canonical == "down") {
    return Event::ArrowDown;
  }
  if (canonical == "left") {
    return Event::ArrowLeft;
  }
  if (canonical == "right") {
    return Event::ArrowRight;
  }
  if (canonical == "pageup") {
    return Event::PageUp;
  }
  if (canonical == "pagedown") {
    return Event::PageDown;
  }
  if (canonical == "home") {
    return Event::Home;
  }
  if (canonical == "end") {
    return Event::End;
  }
  if (canonical == "tab") {
    return Event::Tab;
  }
  if (canonical == "enter") {
    return Event::Return;
  }
  if (canonical == "esc") {
    return Event::Escape;
  }
  if (canonical == "space") {
    return Event::Character(' ');
  }
  if (canonical == "slash") {
    return Event::Character('/');
  }
  if (canonical.size() == 6 && canonical.compare(0, 5, "ctrl+") == 0) {
    return Event::Special(
        std::string(1, static_cast<char>(1 + (canonical[5] - 'a'))));
  }
  // Single printable character (case preserved by CanonicalKeyName).
  return Event::Character(canonical[0]);
}

// Action table for the `keybindings:` section: name -> (member pointer,
// context group, default keys). Groups mirror the event chain in main.cpp:
// 0 = content/global (scroller + n/N, /, w, Ctrl+N, quit, Tab), 1 =
// nav-focus, 2 = search-prompt. The same table seeds KeyBindings::KeyBindings
// and ValidateKeybindings, so defaults cannot drift between construction,
// validation, and --dump-config.
using KeyBindingMember = std::vector<ftxui::Event> KeyBindings::*;
struct KeyBindingActionDef {
  const char* action;
  KeyBindingMember member;
  int group;
  std::initializer_list<const char*> defaults;
};
const KeyBindingActionDef kKeyBindingActions[] = {
    {"scroll_up", &KeyBindings::scroll_up, 0, {"up", "k"}},
    {"scroll_down", &KeyBindings::scroll_down, 0, {"down", "j"}},
    {"page_up", &KeyBindings::page_up, 0, {"pageup"}},
    {"page_down", &KeyBindings::page_down, 0, {"pagedown", "space"}},
    {"goto_top", &KeyBindings::goto_top, 0, {"home"}},
    {"goto_bottom", &KeyBindings::goto_bottom, 0, {"end"}},
    {"pan_left", &KeyBindings::pan_left, 0, {"left", "h"}},
    {"pan_right", &KeyBindings::pan_right, 0, {"right", "l"}},
    {"search_open", &KeyBindings::search_open, 0, {"slash"}},
    {"search_next", &KeyBindings::search_next, 0, {"n"}},
    {"search_prev", &KeyBindings::search_prev, 0, {"N"}},
    {"search_accept", &KeyBindings::search_accept, 2, {"enter"}},
    {"search_cancel", &KeyBindings::search_cancel, 2, {"esc"}},
    {"quit", &KeyBindings::quit, 0, {"q", "esc", "ctrl+c"}},
    {"focus_switch", &KeyBindings::focus_switch, 0, {"tab"}},
    {"toggle_wrap", &KeyBindings::toggle_wrap, 0, {"w"}},
    {"toggle_nav", &KeyBindings::toggle_nav, 0, {"ctrl+n"}},
    {"nav_up", &KeyBindings::nav_up, 1, {"up", "k"}},
    {"nav_down", &KeyBindings::nav_down, 1, {"down", "j"}},
    {"nav_page_up", &KeyBindings::nav_page_up, 1, {"pageup"}},
    {"nav_page_down", &KeyBindings::nav_page_down, 1, {"pagedown", "space"}},
    {"nav_top", &KeyBindings::nav_top, 1, {"home"}},
    {"nav_bottom", &KeyBindings::nav_bottom, 1, {"end"}},
    {"nav_activate", &KeyBindings::nav_activate, 1, {"enter"}},
};

// Validate the top-level `keybindings:` mapping against the embedded schema,
// filling `out` (pre-seeded with defaults) with the actions found.
// Specifying an action replaces its whole default list; an empty list unbinds
// it. Unknown actions, non-list values, non-scalar items, and unknown key
// names throw std::runtime_error with a dotted path. So does a key claimed by
// two actions in the same evaluation context (content, nav, or prompt):
// modal overlaps across contexts (Esc cancel/quit, Enter accept/activate,
// the scroll keys shared with nav) are allowed by design and resolved by
// event-chain precedence.
void ValidateKeybindings(const YAML::Node& node, KeyBindings& out) {

  if (!node.IsMap()) {
    throw std::runtime_error(
        "config: keybindings: expected a mapping of action names to key "
        "lists");
  }

  std::unordered_map<std::string, const KeyBindingActionDef*> by_name;
  for (const auto& def : kKeyBindingActions) {
    by_name[def.action] = &def;
  }

  // Seed the ownership map with the defaults so a remap colliding with
  // another action's (default or configured) keys fails fast. Keyed by
  // context group + canonical name; modal cross-context sharing is allowed.
  std::unordered_map<std::string, std::string> owner;  // group+key -> action
  for (const auto& def : kKeyBindingActions) {
    for (const char* key : def.defaults) {
      owner[std::to_string(def.group) + '\0' + key] = def.action;
    }
  }

  for (auto it = node.begin(); it != node.end(); ++it) {
    if (!it->first.IsScalar()) {
      throw std::runtime_error("config: keybindings: expected string keys");
    }
    const std::string action = it->first.Scalar();
    const auto found = by_name.find(action);
    if (found == by_name.end()) {
      std::string known;
      for (const auto& def : kKeyBindingActions) {
        known += (known.empty() ? "" : ", ") + std::string(def.action);
      }
      throw std::runtime_error("config: keybindings." + action +
                               ": unknown action (expected one of: " + known +
                               ")");
    }
    const KeyBindingActionDef& def = *found->second;
    const YAML::Node value = it->second;
    if (!value.IsSequence()) {
      throw std::runtime_error("config: keybindings." + action +
                               ": expected a list of key names");
    }
    // Release this action's previous claims (defaults or an earlier entry)
    // before claiming the replacement list.
    for (auto o = owner.begin(); o != owner.end();) {
      if (o->second == action) {
        o = owner.erase(o);
      } else {
        ++o;
      }
    }
    std::vector<ftxui::Event> keys;
    int index = 0;
    for (auto k = value.begin(); k != value.end(); ++k, ++index) {
      if (!k->IsScalar()) {
        throw std::runtime_error("config: keybindings." + action + "[" +
                                 std::to_string(index) +
                                 "]: expected a key name string");
      }
      const std::string raw = k->Scalar();
      const auto canonical = CanonicalKeyName(raw);
      if (!canonical.has_value()) {
        throw std::runtime_error(
            "config: keybindings." + action + "[" + std::to_string(index) +
            "]: unknown key '" + raw +
            "' (expected a special key name, ctrl+<letter>, or a single "
            "printable character)");
      }
      const std::string claim =
          std::to_string(def.group) + '\0' + *canonical;
      const auto taken = owner.find(claim);
      if (taken != owner.end()) {
        if (taken->second == action) {
          continue;  // repeated within one list: harmless, dedupe.
        }
        throw std::runtime_error(
            "config: keybindings." + action + "[" + std::to_string(index) +
            "]: key '" + raw + "' is already bound to '" + taken->second + "'");
      }
      owner[claim] = action;
      keys.push_back(KeyEventForCanonical(*canonical));
    }
    out.*(def.member) = std::move(keys);
  }
}

}  // namespace

KeyBindings::KeyBindings() {
  for (const auto& def : kKeyBindingActions) {
    std::vector<ftxui::Event> keys;
    for (const char* key : def.defaults) {
      // Table entries are canonical by construction (see CanonicalKeyName).
      keys.push_back(KeyEventForCanonical(key));
    }
    this->*(def.member) = std::move(keys);
  }
}

bool operator==(const KeyBindings& a, const KeyBindings& b) {
  return a.scroll_up == b.scroll_up && a.scroll_down == b.scroll_down &&
         a.page_up == b.page_up && a.page_down == b.page_down &&
         a.goto_top == b.goto_top && a.goto_bottom == b.goto_bottom &&
         a.pan_left == b.pan_left && a.pan_right == b.pan_right &&
         a.search_open == b.search_open && a.search_next == b.search_next &&
         a.search_prev == b.search_prev &&
         a.search_accept == b.search_accept &&
         a.search_cancel == b.search_cancel && a.quit == b.quit &&
         a.focus_switch == b.focus_switch && a.toggle_wrap == b.toggle_wrap &&
         a.toggle_nav == b.toggle_nav && a.nav_up == b.nav_up &&
         a.nav_down == b.nav_down && a.nav_page_up == b.nav_page_up &&
         a.nav_page_down == b.nav_page_down && a.nav_top == b.nav_top &&
         a.nav_bottom == b.nav_bottom && a.nav_activate == b.nav_activate;
}

bool MatchesKey(const ftxui::Event& event,
                const std::vector<ftxui::Event>& keys) {
  for (const auto& key : keys) {
    if (event == key) {
      return true;
    }
  }
  return false;
}

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
    if (key != "theme" && key != "display" && key != "search" &&
        key != "keybindings") {
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
  if (root["keybindings"].IsDefined()) {
    ValidateKeybindings(root["keybindings"], cfg.keybindings);
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
       << (Config{}.search_case_sensitive ? "true" : "false") << "\n"
       << "#\n"
       << "# Key bindings: every action lists the keys that trigger it.\n"
       << "# Special key names (case-insensitive): up, down, left, right,\n"
       << "# pageup, pagedown, home, end, tab, enter (alias return), esc\n"
       << "# (alias escape), space, slash (alias /).\n"
       << "# Single printable characters are case-sensitive (n vs N).\n"
       << "# Ctrl chords take the form ctrl+<letter> (e.g. ctrl+c).\n"
       << "# Specifying an action replaces its whole list; an empty list\n"
       << "# unbinds it. Unknown actions/keys and a key shared by two actions\n"
       << "# in the same context (content, nav-focus, or search-prompt) are\n"
       << "# errors; modal overlaps across contexts (Esc cancel/quit, Enter\n"
       << "# accept/activate, scroll keys shared with nav) are allowed.\n"
       << "keybindings:\n"
       << "  # main content view\n"
       << "  scroll_up: [up, k]\n"
       << "  scroll_down: [down, j]\n"
       << "  page_up: [pageup]\n"
       << "  page_down: [pagedown, space]\n"
       << "  goto_top: [home]\n"
       << "  goto_bottom: [end]\n"
       << "  pan_left: [left, h]       # scroll display mode only\n"
       << "  pan_right: [right, l]     # scroll display mode only\n"
       << "  # search and toggles\n"
       << "  search_open: [slash]\n"
       << "  search_next: [n]\n"
       << "  search_prev: [N]\n"
       << "  search_accept: [enter]    # while the prompt is open\n"
       << "  search_cancel: [esc]      # while the prompt is open\n"
       << "  quit: [q, esc, ctrl+c]\n"
       << "  focus_switch: [tab]\n"
       << "  toggle_wrap: [w]\n"
       << "  toggle_nav: [ctrl+n]\n"
       << "  # outline navigation (while nav_focused)\n"
       << "  nav_up: [up, k]\n"
       << "  nav_down: [down, j]\n"
       << "  nav_page_up: [pageup]\n"
       << "  nav_page_down: [pagedown, space]\n"
       << "  nav_top: [home]\n"
       << "  nav_bottom: [end]\n"
       << "  nav_activate: [enter]\n";
}

}  // namespace markit