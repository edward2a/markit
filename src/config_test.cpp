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

}  // namespace
