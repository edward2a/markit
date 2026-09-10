// Tests for config file handling and color parsing.
#include <gtest/gtest.h>

#include <cstdio>        // for remove
#include <fstream>       // for ofstream
#include <sstream>       // for ostringstream
#include <string>
#include <unistd.h>      // for getpid

#include <yaml-cpp/yaml.h>  // for YAML::Load

#include "config.hpp"

namespace {

std::string TempYaml(const std::string& body) {
  // __FILE__ is the absolute path to this file in the source tree; derive the
  // repo root (parent of src/) and use its ./tmp scratch directory (ignored by
  // git, with tmp/.gitkeep tracked so the dir exists). Tests run with build/ as
  // cwd, so a bare relative "tmp/" would land in the wrong place.
  std::string src_dir = __FILE__;
  const size_t slash = src_dir.rfind('/');
  src_dir.erase(slash == std::string::npos ? 0 : slash);   // /home/.../markit/src
  const size_t root_slash = src_dir.rfind('/');
  std::string repo_root = src_dir;
  repo_root.erase(root_slash == std::string::npos ? 0 : root_slash);  // /home/.../markit
  const std::string dir = repo_root + "/tmp";
  const std::string path =
      dir + "/markit-config-test-" + std::to_string(getpid()) + ".yml";
  std::ofstream out(path);
  out << body;
  out.close();
  return path;
}

TEST(Config, ParseColor_NamedCaseInsensitive) {
  const auto red = markit::ParseColor("red");
  ASSERT_TRUE(red.has_value());
  EXPECT_EQ(*red, ftxui::Color::Red);

  const auto upper = markit::ParseColor("CYANLIGHT");
  ASSERT_TRUE(upper.has_value());
  EXPECT_EQ(*upper, ftxui::Color::CyanLight);
}

TEST(Config, ParseColor_HexLong) {
  const auto c = markit::ParseColor("#ff8000");
  ASSERT_TRUE(c.has_value());
  EXPECT_EQ(*c, ftxui::Color::RGB(0xff, 0x80, 0x00));
}

TEST(Config, ParseColor_HexShorthand) {
  const auto c = markit::ParseColor("#f80");
  ASSERT_TRUE(c.has_value());
  EXPECT_EQ(*c, ftxui::Color::RGB(0xff, 0x88, 0x00));
}

TEST(Config, ParseColor_Invalid) {
  EXPECT_FALSE(markit::ParseColor("notacolor").has_value());
  EXPECT_FALSE(markit::ParseColor("#gggggg").has_value());
  EXPECT_FALSE(markit::ParseColor("#1234567").has_value());
  EXPECT_FALSE(markit::ParseColor("#12").has_value());
  EXPECT_FALSE(markit::ParseColor("").has_value());
}

TEST(Config, LoadConfig_MissingFileReturnsDefaults) {
  const markit::Config cfg =
      markit::LoadConfig("/nonexistent/markit-missing.yml");
  const markit::Theme& t = cfg.theme;
  EXPECT_EQ(t.heading_h1, ftxui::Color::Red);
  EXPECT_EQ(t.link, ftxui::Color::CyanLight);
  EXPECT_EQ(t.inline_code_fg, ftxui::Color::Green);
  EXPECT_EQ(t.inline_code_bg, ftxui::Color::GrayDark);
  EXPECT_EQ(t.code_block_fg, ftxui::Color::GrayLight);
  EXPECT_EQ(t.code_block_bg, ftxui::Color::GrayDark);
  EXPECT_EQ(t.quote_marker, ftxui::Color::GrayDark);
}

TEST(Config, LoadConfig_EmptyPathReturnsDefaults) {
  const markit::Config cfg = markit::LoadConfig("");
  EXPECT_EQ(cfg.theme.heading_h1, ftxui::Color::Red);
}

// A default-constructed Config mirrors the built-in theme: load/dump operate
// on the whole config container, not just the color scheme.
TEST(Config, Config_WrapsTheme) {
  const markit::Config defaults;
  EXPECT_EQ(defaults.theme.heading_h1, ftxui::Color::Red);
  EXPECT_EQ(defaults.theme.inline_code_bg, ftxui::Color::GrayDark);
  EXPECT_EQ(defaults.theme.quote_marker, ftxui::Color::GrayDark);
}

// The default display mode is Wrap (user decision Q1) and survives load.
TEST(Config, LoadConfig_DefaultWrapMode) {
  EXPECT_EQ(markit::LoadConfig("").horizontal_wrap, markit::WrapMode::Wrap);
  const markit::Config cfg = markit::LoadConfig("/nonexistent/markit-missing.yml");
  EXPECT_EQ(cfg.horizontal_wrap, markit::WrapMode::Wrap);
}

TEST(Config, LoadConfig_DisplayScrollExplicit) {
  const std::string path = TempYaml("display:\n  horizontal: scroll\n");
  const markit::Config cfg = markit::LoadConfig(path);
  EXPECT_EQ(cfg.horizontal_wrap, markit::WrapMode::Scroll);
  EXPECT_EQ(cfg.theme.heading_h1, ftxui::Color::Red);  // theme untouched
  std::remove(path.c_str());
}

TEST(Config, LoadConfig_DisplayWrapExplicit) {
  const std::string path = TempYaml("display:\n  horizontal: wrap\n");
  const markit::Config cfg = markit::LoadConfig(path);
  EXPECT_EQ(cfg.horizontal_wrap, markit::WrapMode::Wrap);
  std::remove(path.c_str());
}

TEST(Config, LoadConfig_DisplayInvalidValueThrows) {
  const std::string path = TempYaml("display:\n  horizontal: sideways\n");
  EXPECT_THROW(markit::LoadConfig(path), std::runtime_error);
  std::remove(path.c_str());
}

TEST(Config, LoadConfig_DisplayUnknownKeyThrows) {
  const std::string path = TempYaml("display:\n  vertical: wrap\n");
  EXPECT_THROW(markit::LoadConfig(path), std::runtime_error);
  std::remove(path.c_str());
}

// The nav bar is visible by default and survives load.
TEST(Config, LoadConfig_DefaultNavVisible) {
  EXPECT_TRUE(markit::LoadConfig("").nav_visible);
  const markit::Config cfg = markit::LoadConfig("/nonexistent/markit-missing.yml");
  EXPECT_TRUE(cfg.nav_visible);
}

TEST(Config, LoadConfig_NavigationHiddenExplicit) {
  const std::string path = TempYaml("display:\n  navigation: hidden\n");
  const markit::Config cfg = markit::LoadConfig(path);
  EXPECT_FALSE(cfg.nav_visible);
  EXPECT_EQ(cfg.horizontal_wrap, markit::WrapMode::Wrap);  // mode untouched
  std::remove(path.c_str());
}

TEST(Config, LoadConfig_NavigationVisibleExplicit) {
  const std::string path = TempYaml("display:\n  navigation: visible\n");
  EXPECT_TRUE(markit::LoadConfig(path).nav_visible);
  std::remove(path.c_str());
}

TEST(Config, LoadConfig_NavigationInvalidValueThrows) {
  const std::string path = TempYaml("display:\n  navigation: sideways\n");
  EXPECT_THROW(markit::LoadConfig(path), std::runtime_error);
  std::remove(path.c_str());
}

// Search is case-insensitive by default and survives load.
TEST(Config, LoadConfig_DefaultSearchCaseInsensitive) {
  EXPECT_FALSE(markit::LoadConfig("").search_case_sensitive);
  const markit::Config cfg = markit::LoadConfig("/nonexistent/markit-missing.yml");
  EXPECT_FALSE(cfg.search_case_sensitive);
}

TEST(Config, LoadConfig_SearchCaseSensitiveExplicit) {
  const std::string path = TempYaml("search:\n  case_sensitive: true\n");
  const markit::Config cfg = markit::LoadConfig(path);
  EXPECT_TRUE(cfg.search_case_sensitive);
  EXPECT_EQ(cfg.horizontal_wrap, markit::WrapMode::Wrap);  // display untouched
  std::remove(path.c_str());
}

TEST(Config, LoadConfig_SearchCaseInsensitiveExplicit) {
  const std::string path = TempYaml("search:\n  case_sensitive: false\n");
  EXPECT_FALSE(markit::LoadConfig(path).search_case_sensitive);
  std::remove(path.c_str());
}

TEST(Config, LoadConfig_SearchInvalidValueThrows) {
  const std::string path = TempYaml("search:\n  case_sensitive: maybe\n");
  EXPECT_THROW(markit::LoadConfig(path), std::runtime_error);
  std::remove(path.c_str());
}

TEST(Config, LoadConfig_SearchUnknownKeyThrows) {
  const std::string path = TempYaml("search:\n  smart_case: true\n");
  EXPECT_THROW(markit::LoadConfig(path), std::runtime_error);
  std::remove(path.c_str());
}

TEST(Config, LoadConfig_UnknownTopLevelSectionThrows) {
  const std::string path = TempYaml("sidebar:\n  width: 30\n");
  EXPECT_THROW(markit::LoadConfig(path), std::runtime_error);
  std::remove(path.c_str());
}

TEST(Config, LoadConfig_PartialOverride) {
  const std::string path = TempYaml(
      "theme:\n"
      "  link: yellow\n"
      "  heading:\n"
      "    h1: '#ff0000'\n");
  const markit::Config cfg = markit::LoadConfig(path);
  const markit::Theme& t = cfg.theme;
  EXPECT_EQ(t.link, ftxui::Color::Yellow);
  EXPECT_EQ(t.heading_h1, ftxui::Color::RGB(0xff, 0x00, 0x00));
  // Unset fields keep defaults.
  EXPECT_EQ(t.heading_h2, ftxui::Color::Yellow);
  EXPECT_EQ(t.inline_code_bg, ftxui::Color::GrayDark);
  std::remove(path.c_str());
}

TEST(Config, LoadConfig_NamedAndHex) {
  const std::string path = TempYaml(
      "theme:\n"
      "  inline_code:\n"
      "    fg: green\n"
      "    bg: '#202020'\n"
      "  code_block:\n"
      "    fg: '#c0c0c0'\n"
      "    bg: graydark\n"
      "  quote_marker: blue\n");
  const markit::Config cfg = markit::LoadConfig(path);
  const markit::Theme& t = cfg.theme;
  EXPECT_EQ(t.inline_code_fg, ftxui::Color::Green);
  EXPECT_EQ(t.inline_code_bg, ftxui::Color::RGB(0x20, 0x20, 0x20));
  EXPECT_EQ(t.code_block_fg, ftxui::Color::RGB(0xc0, 0xc0, 0xc0));
  EXPECT_EQ(t.code_block_bg, ftxui::Color::GrayDark);
  EXPECT_EQ(t.quote_marker, ftxui::Color::Blue);
  std::remove(path.c_str());
}

TEST(Config, LoadConfig_UnknownTopLevelKeyThrows) {
  const std::string path = TempYaml("theme:\n  bogus: red\n");
  EXPECT_THROW(markit::LoadConfig(path), std::runtime_error);
  std::remove(path.c_str());
}

TEST(Config, LoadConfig_UnknownHeadingKeyThrows) {
  const std::string path = TempYaml("theme:\n  heading:\n    h5: red\n");
  EXPECT_THROW(markit::LoadConfig(path), std::runtime_error);
  std::remove(path.c_str());
}

TEST(Config, LoadConfig_InvalidColorThrows) {
  const std::string path = TempYaml("theme:\n  link: notacolor\n");
  EXPECT_THROW(markit::LoadConfig(path), std::runtime_error);
  std::remove(path.c_str());
}

TEST(Config, LoadConfig_WrongTypeThrows) {
  const std::string path = TempYaml("theme:\n  link:\n    fg: red\n");
  EXPECT_THROW(markit::LoadConfig(path), std::runtime_error);
  std::remove(path.c_str());
}

TEST(Config, LoadConfig_MalformedYamlThrows) {
  const std::string path = TempYaml("theme:\n  link: [unclosed\n");
  EXPECT_THROW(markit::LoadConfig(path), std::runtime_error);
  std::remove(path.c_str());
}

TEST(Config, DumpDefaultConfig_HeaderListsColorsAndHex) {
  std::ostringstream out;
  markit::DumpDefaultConfig(out);
  const std::string s = out.str();

  EXPECT_NE(s.find("named palette color"), std::string::npos);
  EXPECT_NE(s.find("#rrggbb"), std::string::npos);
  EXPECT_NE(s.find("#rgb"), std::string::npos);

  // Every supported named color is listed in the header.
  for (const std::string& name : {"black", "red", "green", "yellow", "blue",
                                  "magenta", "cyan", "white", "redlight",
                                  "greenlight", "yellowlight", "bluelight",
                                  "magentalight", "cyanlight", "graylight",
                                  "graydark"}) {
    EXPECT_NE(s.find(name), std::string::npos) << "missing color " << name;
  }
}

TEST(Config, DumpDefaultConfig_RoundTripMatchesDefaults) {
  std::ostringstream out;
  markit::DumpDefaultConfig(out);

  const YAML::Node root = YAML::Load(out.str());
  const YAML::Node theme = root["theme"];
  ASSERT_TRUE(theme.IsMap());

  const markit::Theme defaults;
  const markit::Theme dumped = {
      markit::ParseColor(theme["heading"]["h1"].Scalar()).value(),
      markit::ParseColor(theme["heading"]["h2"].Scalar()).value(),
      markit::ParseColor(theme["heading"]["h3"].Scalar()).value(),
      markit::ParseColor(theme["heading"]["h4"].Scalar()).value(),
      markit::ParseColor(theme["link"].Scalar()).value(),
      markit::ParseColor(theme["inline_code"]["fg"].Scalar()).value(),
      markit::ParseColor(theme["inline_code"]["bg"].Scalar()).value(),
      markit::ParseColor(theme["code_block"]["fg"].Scalar()).value(),
      markit::ParseColor(theme["code_block"]["bg"].Scalar()).value(),
      markit::ParseColor(theme["quote_marker"].Scalar()).value(),
  };

  EXPECT_EQ(dumped.heading_h1, defaults.heading_h1);
  EXPECT_EQ(dumped.heading_h2, defaults.heading_h2);
  EXPECT_EQ(dumped.heading_h3, defaults.heading_h3);
  EXPECT_EQ(dumped.heading_h4, defaults.heading_h4);
  EXPECT_EQ(dumped.link, defaults.link);
  EXPECT_EQ(dumped.inline_code_fg, defaults.inline_code_fg);
  EXPECT_EQ(dumped.inline_code_bg, defaults.inline_code_bg);
  EXPECT_EQ(dumped.code_block_fg, defaults.code_block_fg);
  EXPECT_EQ(dumped.code_block_bg, defaults.code_block_bg);
  EXPECT_EQ(dumped.quote_marker, defaults.quote_marker);

  // The display section round-trips: the dumped document reloads to the
  // default Wrap mode and a visible nav bar.
  const std::string h = root["display"]["horizontal"].Scalar();
  const std::string path = TempYaml(h == "wrap" ? "display:\n  horizontal: wrap\n"
                                                  : "display:\n  horizontal: scroll\n");
  EXPECT_EQ(markit::LoadConfig(path).horizontal_wrap, markit::Config{}.horizontal_wrap);
  std::remove(path.c_str());

  const std::string n = root["display"]["navigation"].Scalar();
  const std::string nav_path = TempYaml("display:\n  navigation: " + n + "\n");
  EXPECT_EQ(markit::LoadConfig(nav_path).nav_visible, markit::Config{}.nav_visible);
  std::remove(nav_path.c_str());
}

// An empty config file yields defaults instead of a yaml-cpp exception.
TEST(Config, LoadConfig_EmptyFileReturnsDefaults) {
  const std::string path = TempYaml("");
  const markit::Config cfg = markit::LoadConfig(path);
  EXPECT_EQ(cfg.theme.heading_h1, ftxui::Color::Red);
  EXPECT_EQ(cfg.horizontal_wrap, markit::WrapMode::Wrap);
  std::remove(path.c_str());
}

// A defined-but-not-a-mapping root is a typed error, not an untyped
// yaml-cpp exception.
TEST(Config, LoadConfig_NonMapRootThrows) {
  const std::string path = TempYaml("- just\n- a\n- list\n");
  EXPECT_THROW(markit::LoadConfig(path), std::runtime_error);
  std::remove(path.c_str());
}

// Non-scalar mapping keys are rejected with a typed error at every level.
TEST(Config, LoadConfig_NonScalarKeyThrows) {
  const std::string top = TempYaml("? [a, b]\n: 1\n");
  EXPECT_THROW(markit::LoadConfig(top), std::runtime_error);
  std::remove(top.c_str());

  const std::string nested = TempYaml("theme:\n  ? [a]\n  : red\n");
  EXPECT_THROW(markit::LoadConfig(nested), std::runtime_error);
  std::remove(nested.c_str());
}

// A default-constructed KeyBindings preserves the historical hardcoded
// mappings (spot-checked across all four areas).
TEST(Config, KeyBindings_DefaultsMatchHistoricalMappings) {
  const markit::KeyBindings kb;
  EXPECT_EQ(kb.scroll_up,
            (std::vector<ftxui::Event>{ftxui::Event::ArrowUp,
                                       ftxui::Event::Character('k')}));
  EXPECT_EQ(kb.scroll_down,
            (std::vector<ftxui::Event>{ftxui::Event::ArrowDown,
                                       ftxui::Event::Character('j')}));
  EXPECT_EQ(kb.page_up, (std::vector<ftxui::Event>{ftxui::Event::PageUp}));
  EXPECT_EQ(kb.page_down,
            (std::vector<ftxui::Event>{ftxui::Event::PageDown,
                                       ftxui::Event::Character(' ')}));
  EXPECT_EQ(kb.goto_top, (std::vector<ftxui::Event>{ftxui::Event::Home}));
  EXPECT_EQ(kb.goto_bottom, (std::vector<ftxui::Event>{ftxui::Event::End}));
  EXPECT_EQ(kb.pan_left,
            (std::vector<ftxui::Event>{ftxui::Event::ArrowLeft,
                                       ftxui::Event::Character('h')}));
  EXPECT_EQ(kb.pan_right,
            (std::vector<ftxui::Event>{ftxui::Event::ArrowRight,
                                       ftxui::Event::Character('l')}));
  EXPECT_EQ(kb.search_open,
            (std::vector<ftxui::Event>{ftxui::Event::Character('/')}));
  EXPECT_EQ(kb.search_next,
            (std::vector<ftxui::Event>{ftxui::Event::Character('n')}));
  EXPECT_EQ(kb.search_prev,
            (std::vector<ftxui::Event>{ftxui::Event::Character('N')}));
  EXPECT_EQ(kb.search_accept,
            (std::vector<ftxui::Event>{ftxui::Event::Return}));
  EXPECT_EQ(kb.search_cancel,
            (std::vector<ftxui::Event>{ftxui::Event::Escape}));
  EXPECT_EQ(kb.quit, (std::vector<ftxui::Event>{ftxui::Event::Character('q'),
                                                ftxui::Event::Escape,
                                                ftxui::Event::CtrlC}));
  EXPECT_EQ(kb.focus_switch, (std::vector<ftxui::Event>{ftxui::Event::Tab}));
  EXPECT_EQ(kb.toggle_wrap,
            (std::vector<ftxui::Event>{ftxui::Event::Character('w')}));
  EXPECT_EQ(kb.toggle_nav, (std::vector<ftxui::Event>{ftxui::Event::CtrlN}));
  EXPECT_EQ(kb.nav_up, kb.scroll_up);
  EXPECT_EQ(kb.nav_down, kb.scroll_down);
  EXPECT_EQ(kb.nav_page_up, kb.page_up);
  EXPECT_EQ(kb.nav_page_down, kb.page_down);
  EXPECT_EQ(kb.nav_top, kb.goto_top);
  EXPECT_EQ(kb.nav_bottom, kb.goto_bottom);
  EXPECT_EQ(kb.nav_activate, kb.search_accept);
}

// Specifying an action replaces its whole default list; the rest keeps
// defaults and other sections are untouched.
TEST(Config, LoadConfig_KeybindingsOverrideReplacesList) {
  const std::string path = TempYaml("keybindings:\n  scroll_down: [x]\n");
  const markit::Config cfg = markit::LoadConfig(path);
  EXPECT_EQ(cfg.keybindings.scroll_down,
            (std::vector<ftxui::Event>{ftxui::Event::Character('x')}));
  EXPECT_EQ(cfg.keybindings.scroll_up, markit::KeyBindings{}.scroll_up);
  EXPECT_EQ(cfg.theme.heading_h1, ftxui::Color::Red);  // theme untouched
  std::remove(path.c_str());
}

// An empty list unbinds the action.
TEST(Config, LoadConfig_KeybindingsEmptyListUnbinds) {
  const std::string path = TempYaml("keybindings:\n  quit: []\n");
  const markit::Config cfg = markit::LoadConfig(path);
  EXPECT_TRUE(cfg.keybindings.quit.empty());
  EXPECT_FALSE(markit::MatchesKey(ftxui::Event::Character('q'),
                                  cfg.keybindings.quit));
  std::remove(path.c_str());
}

// Special names are case-insensitive and aliases fold: UP/k reloads to the
// exact default scroll_up list (single characters themselves stay
// case-sensitive, so K/Q below would be different keys).
TEST(Config, LoadConfig_KeybindingsCaseAndAliases) {
  const std::string path =
      TempYaml("keybindings:\n  scroll_up: [UP, k]\n  quit: [q, Escape, CTRL+C]\n");
  const markit::Config cfg = markit::LoadConfig(path);
  EXPECT_EQ(cfg.keybindings.scroll_up, markit::KeyBindings{}.scroll_up);
  EXPECT_EQ(cfg.keybindings.quit, markit::KeyBindings{}.quit);
  std::remove(path.c_str());
}

// Single characters stay case-sensitive: n and N coexist in one file.
TEST(Config, LoadConfig_KeybindingsCharCaseSensitive) {
  const std::string path =
      TempYaml("keybindings:\n  search_next: [n]\n  search_prev: [N]\n");
  const markit::Config cfg = markit::LoadConfig(path);
  EXPECT_TRUE(markit::MatchesKey(ftxui::Event::Character('n'),
                                 cfg.keybindings.search_next));
  EXPECT_FALSE(markit::MatchesKey(ftxui::Event::Character('N'),
                                  cfg.keybindings.search_next));
  std::remove(path.c_str());
}

// Ctrl chords map to the raw control byte (== FTXUI's predefined Ctrl keys).
TEST(Config, LoadConfig_KeybindingsCtrlChord) {
  const std::string path = TempYaml("keybindings:\n  toggle_nav: [ctrl+x]\n");
  const markit::Config cfg = markit::LoadConfig(path);
  EXPECT_EQ(cfg.keybindings.toggle_nav,
            (std::vector<ftxui::Event>{ftxui::Event::CtrlX}));
  std::remove(path.c_str());
}

// Modal overlaps across contexts are allowed: Esc is both quit and
// search_cancel, Enter both search_accept and nav_activate — by design.
TEST(Config, LoadConfig_KeybindingsModalOverlapAllowed) {
  const std::string path = TempYaml(
      "keybindings:\n"
      "  quit: [q, esc]\n"
      "  search_cancel: [escape]\n"
      "  search_accept: [return]\n"
      "  nav_activate: [enter]\n"
      "  scroll_up: [up]\n"
      "  nav_up: [up, k]\n");
  const markit::Config cfg = markit::LoadConfig(path);
  EXPECT_TRUE(markit::MatchesKey(ftxui::Event::Escape, cfg.keybindings.quit));
  EXPECT_TRUE(markit::MatchesKey(ftxui::Event::Escape,
                                 cfg.keybindings.search_cancel));
  EXPECT_TRUE(markit::MatchesKey(ftxui::Event::Return,
                                 cfg.keybindings.search_accept));
  EXPECT_TRUE(markit::MatchesKey(ftxui::Event::Return,
                                 cfg.keybindings.nav_activate));
  std::remove(path.c_str());
}

// A key shared by two actions in the same context fails fast — including via
// alias folding (RETURN == enter, already nav_activate's key).
TEST(Config, LoadConfig_KeybindingsSameContextDuplicateThrows) {
  const std::string clash = TempYaml("keybindings:\n  scroll_up: [j]\n");
  EXPECT_THROW(markit::LoadConfig(clash), std::runtime_error);
  std::remove(clash.c_str());

  const std::string alias = TempYaml("keybindings:\n  nav_up: [RETURN]\n");
  EXPECT_THROW(markit::LoadConfig(alias), std::runtime_error);
  std::remove(alias.c_str());

  const std::string prompt =
      TempYaml("keybindings:\n  search_accept: [esc]\n");
  EXPECT_THROW(markit::LoadConfig(prompt), std::runtime_error);
  std::remove(prompt.c_str());
}

// Remapping away frees the old key: quit without esc lets search_cancel keep
// it, and scroll_down can then take q.
TEST(Config, LoadConfig_KeybindingsRemapFreesOldKey) {
  const std::string path =
      TempYaml("keybindings:\n  quit: [ctrl+c]\n  scroll_down: [q]\n");
  const markit::Config cfg = markit::LoadConfig(path);
  EXPECT_EQ(cfg.keybindings.quit,
            (std::vector<ftxui::Event>{ftxui::Event::CtrlC}));
  EXPECT_TRUE(markit::MatchesKey(ftxui::Event::Character('q'),
                                 cfg.keybindings.scroll_down));
  std::remove(path.c_str());
}

TEST(Config, LoadConfig_KeybindingsUnknownActionThrows) {
  const std::string path = TempYaml("keybindings:\n  scroll_sideways: [x]\n");
  EXPECT_THROW(markit::LoadConfig(path), std::runtime_error);
  std::remove(path.c_str());
}

TEST(Config, LoadConfig_KeybindingsUnknownKeyThrows) {
  const std::string path = TempYaml("keybindings:\n  scroll_up: [f13]\n");
  EXPECT_THROW(markit::LoadConfig(path), std::runtime_error);
  std::remove(path.c_str());

  const std::string chord = TempYaml("keybindings:\n  quit: [ctrl+1]\n");
  EXPECT_THROW(markit::LoadConfig(chord), std::runtime_error);
  std::remove(chord.c_str());
}

TEST(Config, LoadConfig_KeybindingsWrongShapeThrows) {
  const std::string non_map = TempYaml("keybindings: [up]\n");
  EXPECT_THROW(markit::LoadConfig(non_map), std::runtime_error);
  std::remove(non_map.c_str());

  const std::string non_list = TempYaml("keybindings:\n  scroll_up: up\n");
  EXPECT_THROW(markit::LoadConfig(non_list), std::runtime_error);
  std::remove(non_list.c_str());

  const std::string non_scalar =
      TempYaml("keybindings:\n  scroll_up:\n    - [up]\n");
  EXPECT_THROW(markit::LoadConfig(non_scalar), std::runtime_error);
  std::remove(non_scalar.c_str());

  const std::string non_scalar_key = TempYaml("keybindings:\n  ? [a]\n  : red\n");
  EXPECT_THROW(markit::LoadConfig(non_scalar_key), std::runtime_error);
  std::remove(non_scalar_key.c_str());
}

// A repeated key inside one list dedupes silently instead of throwing.
TEST(Config, LoadConfig_KeybindingsWithinActionDedupe) {
  const std::string path = TempYaml("keybindings:\n  scroll_up: [up, UP]\n");
  const markit::Config cfg = markit::LoadConfig(path);
  EXPECT_EQ(cfg.keybindings.scroll_up,
            (std::vector<ftxui::Event>{ftxui::Event::ArrowUp}));
  std::remove(path.c_str());
}

// The dumped keybindings section reloads to exactly the defaults, so the
// dump text cannot drift from the compiled-in mappings.
TEST(Config, DumpDefaultConfig_KeybindingsRoundTrip) {
  std::ostringstream out;
  markit::DumpDefaultConfig(out);
  const std::string s = out.str();
  EXPECT_NE(s.find("keybindings:"), std::string::npos);
  EXPECT_NE(s.find("scroll_up: [up, k]"), std::string::npos);
  EXPECT_NE(s.find("quit: [q, esc, ctrl+c]"), std::string::npos);

  const std::string path = TempYaml(out.str());
  const markit::Config cfg = markit::LoadConfig(path);
  EXPECT_EQ(cfg.keybindings, markit::KeyBindings{});
  EXPECT_EQ(cfg.horizontal_wrap, markit::WrapMode::Wrap);
  EXPECT_TRUE(cfg.nav_visible);
  EXPECT_FALSE(cfg.search_case_sensitive);
  std::remove(path.c_str());
}

}  // namespace
