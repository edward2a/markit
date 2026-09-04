// Tests for config file handling and color parsing.
#include <gtest/gtest.h>

#include <cstdio>        // for remove
#include <fstream>       // for ofstream
#include <string>
#include <unistd.h>      // for getpid

#include "config.hpp"

namespace {

std::string TempYaml(const std::string& body) {
  std::string path = "/tmp/markit-config-test-" +
                     std::to_string(getpid()) + ".yml";
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
  const markit::Theme t =
      markit::LoadConfig("/nonexistent/markit-missing.yml");
  EXPECT_EQ(t.heading_h1, ftxui::Color::Red);
  EXPECT_EQ(t.link, ftxui::Color::CyanLight);
  EXPECT_EQ(t.inline_code_fg, ftxui::Color::Green);
  EXPECT_EQ(t.inline_code_bg, ftxui::Color::GrayDark);
  EXPECT_EQ(t.code_block_fg, ftxui::Color::GrayLight);
  EXPECT_EQ(t.code_block_bg, ftxui::Color::GrayDark);
  EXPECT_EQ(t.quote_marker, ftxui::Color::GrayDark);
}

TEST(Config, LoadConfig_EmptyPathReturnsDefaults) {
  const markit::Theme t = markit::LoadConfig("");
  EXPECT_EQ(t.heading_h1, ftxui::Color::Red);
}

TEST(Config, LoadConfig_PartialOverride) {
  const std::string path = TempYaml(
      "theme:\n"
      "  link: yellow\n"
      "  heading:\n"
      "    h1: '#ff0000'\n");
  const markit::Theme t = markit::LoadConfig(path);
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
  const markit::Theme t = markit::LoadConfig(path);
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

}  // namespace