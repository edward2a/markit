// Unit tests for private HTML entity decoding.
#include <gtest/gtest.h>

#include <string>

#include "entity_decode.hpp"

namespace {

const std::string kReplacement = "\xEF\xBF\xBD";  // U+FFFD.

TEST(EntityDecode, NamedEntities) {
  EXPECT_EQ(markit::DecodeEntityForDisplay("&amp;"), "&");
  EXPECT_EQ(markit::DecodeEntityForDisplay("&copy;"), "\u00A9");
  EXPECT_EQ(markit::DecodeEntityForDisplay("&lt;"), "<");
}

// Two-code-point entries emit both code points.
TEST(EntityDecode, TwoCodePointEntity) {
  // U+2242 followed by U+0338.
  EXPECT_EQ(markit::DecodeEntityForDisplay("&NotEqualTilde;"),
            "\u2242\u0338");
}

TEST(EntityDecode, NumericEntities) {
  EXPECT_EQ(markit::DecodeEntityForDisplay("&#65;"), "A");
  EXPECT_EQ(markit::DecodeEntityForDisplay("&#x41;"), "A");
  EXPECT_EQ(markit::DecodeEntityForDisplay("&#x1F600;"), "\U0001F600");
}

// Unknown and malformed references keep their original spelling.
TEST(EntityDecode, UnknownAndMalformedPreserved) {
  EXPECT_EQ(markit::DecodeEntityForDisplay("&bogus;"), "&bogus;");
  EXPECT_EQ(markit::DecodeEntityForDisplay("&amp"), "&amp");
  EXPECT_EQ(markit::DecodeEntityForDisplay("&#;"), "&#;");
  EXPECT_EQ(markit::DecodeEntityForDisplay("&#x;"), "&#x;");
  EXPECT_EQ(markit::DecodeEntityForDisplay("&#xZZ;"), "&#xZZ;");
}

// Zero, surrogates, out-of-range and overflow produce U+FFFD in display text.
TEST(EntityDecode, InvalidNumericScalars) {
  EXPECT_EQ(markit::DecodeEntityForDisplay("&#0;"), kReplacement);
  EXPECT_EQ(markit::DecodeEntityForDisplay("&#xD800;"), kReplacement);
  EXPECT_EQ(markit::DecodeEntityForDisplay("&#x110000;"), kReplacement);
  EXPECT_EQ(markit::DecodeEntityForDisplay("&#99999999999999999999;"),
            kReplacement);
}

// Legitimate decoded whitespace maps to a space for layout; other controls are
// visibly replaced so no live terminal control is emitted.
TEST(EntityDecode, WhitespaceAndControls) {
  EXPECT_EQ(markit::DecodeEntityForDisplay("&Tab;"), " ");
  EXPECT_EQ(markit::DecodeEntityForDisplay("&NewLine;"), " ");
  EXPECT_EQ(markit::DecodeEntityForDisplay("&#27;"), kReplacement);
  EXPECT_EQ(markit::DecodeEntityForDisplay("&#x7F;"), kReplacement);
}

// Text scanning decodes exactly once: an escaped entity spelling is not
// recursively decoded.
TEST(EntityDecode, TextScanDecodesOnce) {
  EXPECT_EQ(markit::DecodeEntityText("a &amp; b"), "a & b");
  EXPECT_EQ(markit::DecodeEntityText("&amp;amp;"), "&amp;");
  EXPECT_EQ(markit::DecodeEntityText("incomplete &amp end"), "incomplete &amp end");
}

// An unknown or incomplete candidate must not swallow a later valid reference.
TEST(EntityDecode, ScanDoesNotSwallowLaterReference) {
  EXPECT_EQ(markit::DecodeEntityText("&bogus &amp;"), "&bogus &");
  EXPECT_EQ(markit::DecodeEntityText("& &amp;"), "& &");
  const markit::DecodedDestination dest =
      markit::DecodeDestination("https://a.test/&bogus &amp;");
  EXPECT_EQ(dest.text, "https://a.test/&bogus &");
  EXPECT_FALSE(dest.invalid);
}

// Destinations preserve decoded code points and flag invalid scalars.
TEST(EntityDecode, DestinationDecoding) {
  const markit::DecodedDestination ok =
      markit::DecodeDestination("https://a.test/&amp;b");
  EXPECT_FALSE(ok.invalid);
  EXPECT_EQ(ok.text, "https://a.test/&b");

  const markit::DecodedDestination control =
      markit::DecodeDestination("https://a.test/&#27;");
  EXPECT_FALSE(control.invalid);
  EXPECT_EQ(control.text, std::string("https://a.test/") + '\x1b');

  const markit::DecodedDestination bad =
      markit::DecodeDestination("https://a.test/&#xD800;");
  EXPECT_TRUE(bad.invalid);
}

}  // namespace
