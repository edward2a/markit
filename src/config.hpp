#ifndef MARKIT_CONFIG_HPP
#define MARKIT_CONFIG_HPP

#include <iosfwd>
#include <optional>
#include <string>

#include "display.hpp"
#include "keybindings.hpp"
#include "theme.hpp"

namespace markit {

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

// Parse a color string: a named FTXUI Palette16 color (case-insensitive), a
// hex truecolor string "#rrggbb" / "#rgb", or "none" (case-insensitive) for
// the default terminal background (ftxui::Color::Default). Returns nullopt on
// any failure.
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
