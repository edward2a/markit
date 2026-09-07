#ifndef MARKIT_CONFIG_HPP
#define MARKIT_CONFIG_HPP

#include <iosfwd>
#include <optional>
#include <string>

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

// Top-level settings container. Additional per-area settings (e.g. a future
// `display:` section) are added beside `theme`; dump/load operate on the whole
// Config, not just the color scheme.
struct Config {
  Theme theme;
  WrapMode horizontal_wrap = WrapMode::Wrap;
  // Navigation bar (right side) default visibility; toggled live with `n`.
  bool nav_visible = true;
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