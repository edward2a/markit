#ifndef MARKIT_CONFIG_HPP
#define MARKIT_CONFIG_HPP

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

// Parse a color string: a named FTXUI Palette16 color (case-insensitive) or a
// hex truecolor string "#rrggbb" / "#rgb". Returns nullopt on any failure.
std::optional<ftxui::Color> ParseColor(const std::string& s);

// Resolve the default config path "${HOME}/.config/markit/markit.yml".
// Returns an empty string when $HOME is unset.
std::string DefaultConfigPath();

// Load and validate the config file at `path`.
//  - Missing file (or empty path): returns the default Theme (silently).
//  - Parse error or schema violation: throws std::runtime_error whose message
//    includes the offending key path, value, and a description.
Theme LoadConfig(const std::string& path);

}  // namespace markit

#endif  // MARKIT_CONFIG_HPP