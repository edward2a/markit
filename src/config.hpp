#ifndef MARKIT_CONFIG_HPP
#define MARKIT_CONFIG_HPP

#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

#include <ftxui/component/event.hpp>
#include <ftxui/screen/color.hpp>

namespace markit {

// Centralized color scheme. Defaults match the original hardcoded colors so a
// missing config file renders identically to the pre-config behavior.
struct Theme {
  ftxui::Color heading_h1 = ftxui::Color::Red;
  ftxui::Color heading_h2 = ftxui::Color::Yellow;
  ftxui::Color heading_h3 = ftxui::Color::Green;
  ftxui::Color heading_h4 = ftxui::Color::CyanLight;
  ftxui::Color link = ftxui::Color::CyanLight;
  ftxui::Color inline_code_fg = ftxui::Color::Green;
  ftxui::Color inline_code_bg = ftxui::Color::GrayDark;
  ftxui::Color code_block_fg = ftxui::Color::GrayLight;
  ftxui::Color code_block_bg = ftxui::Color::GrayDark;
  ftxui::Color quote_marker = ftxui::Color::GrayDark;
};

// Horizontal overflow handling for block-level content that exceeds the
// viewport width.
enum class WrapMode { Wrap, Scroll };

// Key bindings for every interactive action, one key list per action.
// Defaults preserve the historical hardcoded mappings (see DumpDefaultConfig).
// A binding list may be emptied to unbind the action; overlaps between actions
// that share an evaluation context (e.g. two content actions on the same key)
// are rejected at load, while modal overlaps across contexts (Esc in both
// `quit` and `search_cancel`, Enter in `search_accept`/`nav_activate`, the
// scroll keys shared between content and nav) are allowed by design — the
// event chain resolves them by precedence.
struct KeyBindings {
  // Main content view (handled by the scroller).
  std::vector<ftxui::Event> scroll_up;
  std::vector<ftxui::Event> scroll_down;
  std::vector<ftxui::Event> page_up;
  std::vector<ftxui::Event> page_down;
  std::vector<ftxui::Event> goto_top;
  std::vector<ftxui::Event> goto_bottom;
  std::vector<ftxui::Event> pan_left;   // scroll display mode only.
  std::vector<ftxui::Event> pan_right;  // scroll display mode only.
  // Search and toggles (handled in main.cpp).
  std::vector<ftxui::Event> search_open;
  std::vector<ftxui::Event> search_next;
  std::vector<ftxui::Event> search_prev;
  std::vector<ftxui::Event> search_accept;  // while the prompt is open.
  std::vector<ftxui::Event> search_cancel;  // while the prompt is open.
  std::vector<ftxui::Event> quit;
  std::vector<ftxui::Event> focus_switch;
  std::vector<ftxui::Event> toggle_wrap;
  std::vector<ftxui::Event> toggle_nav;
  // Outline navigation (handled in main.cpp while nav_focused).
  std::vector<ftxui::Event> nav_up;
  std::vector<ftxui::Event> nav_down;
  std::vector<ftxui::Event> nav_page_up;
  std::vector<ftxui::Event> nav_page_down;
  std::vector<ftxui::Event> nav_top;
  std::vector<ftxui::Event> nav_bottom;
  std::vector<ftxui::Event> nav_activate;

  KeyBindings();  // fills every list with its default keys.
};

bool operator==(const KeyBindings& a, const KeyBindings& b);
inline bool operator!=(const KeyBindings& a, const KeyBindings& b) {
  return !(a == b);
}

// True when `event` equals any key in `keys` (FTXUI Event equality compares
// the underlying input, so Character('q') matches the predefined Event::q).
bool MatchesKey(const ftxui::Event& event,
                const std::vector<ftxui::Event>& keys);

// Top-level settings container. Additional per-area settings (e.g. a future
// `display:` section) are added beside `theme`; dump/load operate on the whole
// Config, not just the color scheme.
struct Config {
  Theme theme;
  WrapMode horizontal_wrap = WrapMode::Wrap;
  // Navigation bar (right side) default visibility; toggled live with `n`.
  bool nav_visible = true;
  // In-document search (`/`): case-sensitive matching when true, folding
  // ASCII case otherwise.
  bool search_case_sensitive = false;
  KeyBindings keybindings;
};

// Parse a color string: a named FTXUI Palette16 color (case-insensitive) or a
// hex truecolor string "#rrggbb" / "#rgb". Returns nullopt on any failure.
std::optional<ftxui::Color> ParseColor(const std::string& s);

// Resolve the default config path "${HOME}/.config/markit/markit.yml".
// Returns an empty string when $HOME is unset.
std::string DefaultConfigPath();

// Load and validate the config file at `path`.
//  - Missing file (or empty path): returns the default Config (silently).
//  - Parse error or schema violation: throws std::runtime_error whose message
//    includes the offending key path, value, and a description.
Config LoadConfig(const std::string& path);

// Write the default Config as a human-readable YAML document to `out`,
// including a commented header explaining the supported color values (named
// colors and hex truecolor strings). Useful with `markit --dump-config > file`.
void DumpDefaultConfig(std::ostream& out);

}  // namespace markit

#endif  // MARKIT_CONFIG_HPP