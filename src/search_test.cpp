// Tests for the in-document search core: matcher behavior, FindMatches row
// collection, and NextMatch wrap-around navigation.
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "search.hpp"

namespace markit {
namespace {

// A scripted Matcher fake: matches rows containing any of the given needles.
// Always ok, so FindMatches/NextMatch logic is tested without the engine.
class FakeMatcher : public Matcher {
 public:
  explicit FakeMatcher(std::vector<std::string> needles)
      : needles_(std::move(needles)) {}
  bool Matches(const std::string& row) const override {
    for (const auto& n : needles_) {
      if (row.find(n) != std::string::npos) {
        return true;
      }
    }
    return false;
  }
  bool ok() const override { return true; }

 private:
  std::vector<std::string> needles_;
};

// An ok()==false fake: FindMatches must report no rows.
class BrokenMatcher : public Matcher {
 public:
  bool Matches(const std::string&) const override { return true; }
  bool ok() const override { return false; }
};

TEST(FindMatches, CollectsMatchingRowsAscending) {
  const std::vector<std::string> rows = {"alpha", "beta needle", "gamma",
                                         "needle delta", "epsilon"};
  EXPECT_EQ(FindMatches(rows, FakeMatcher({"needle"})), (std::vector<int>{1, 3}));
}

TEST(FindMatches, NoRowsWhenNothingMatches) {
  const std::vector<std::string> rows = {"alpha", "beta"};
  EXPECT_TRUE(FindMatches(rows, FakeMatcher({"zzz"})).empty());
}

TEST(FindMatches, InvalidMatcherMatchesNothing) {
  const std::vector<std::string> rows = {"alpha", "beta"};
  EXPECT_TRUE(FindMatches(rows, BrokenMatcher()).empty());
}

TEST(NextMatch, ForwardFromBeforeFirst) {
  EXPECT_EQ(NextMatch({2, 5, 9}, 0, +1), 2);
}

TEST(NextMatch, ForwardAdvancesOffCurrentMatch) {
  EXPECT_EQ(NextMatch({2, 5, 9}, 5, +1), 9);
}

TEST(NextMatch, ForwardWrapsPastLast) {
  EXPECT_EQ(NextMatch({2, 5, 9}, 9, +1), 2);
  EXPECT_EQ(NextMatch({2, 5, 9}, 100, +1), 2);
}

TEST(NextMatch, BackwardFromAfterLast) {
  EXPECT_EQ(NextMatch({2, 5, 9}, 100, -1), 9);
}

TEST(NextMatch, BackwardAdvancesOffCurrentMatch) {
  EXPECT_EQ(NextMatch({2, 5, 9}, 5, -1), 2);
}

TEST(NextMatch, BackwardWrapsBeforeFirst) {
  EXPECT_EQ(NextMatch({2, 5, 9}, 2, -1), 9);
  EXPECT_EQ(NextMatch({2, 5, 9}, 0, -1), 9);
}

TEST(NextMatch, EmptyMatchesGivesMinusOne) {
  EXPECT_EQ(NextMatch({}, 0, +1), -1);
  EXPECT_EQ(NextMatch({}, 0, -1), -1);
}

TEST(Re2Matcher, LiteralMatchesSubstring) {
  Re2Matcher m("needle");
  EXPECT_TRUE(m.ok());
  EXPECT_TRUE(m.Matches("a needle here"));
  EXPECT_FALSE(m.Matches("nothing here"));
}

TEST(Re2Matcher, RegexOperatorsWork) {
  Re2Matcher m("err.*timeout");
  EXPECT_TRUE(m.ok());
  EXPECT_TRUE(m.Matches("error: connection timeout"));
  EXPECT_FALSE(m.Matches("error: ok"));
}

TEST(Re2Matcher, AnchorsWork) {
  Re2Matcher m("^# ");
  EXPECT_TRUE(m.Matches("# Title"));
  EXPECT_FALSE(m.Matches("x # Title"));
}

TEST(Re2Matcher, CaseInsensitiveByDefault) {
  Re2Matcher m("needle");
  EXPECT_TRUE(m.Matches("NEEDLE"));
  Re2Matcher sensitive("needle", /*case_sensitive=*/true);
  EXPECT_TRUE(sensitive.ok());
  EXPECT_FALSE(sensitive.Matches("NEEDLE"));
  EXPECT_TRUE(sensitive.Matches("needle"));
}

TEST(Re2Matcher, InvalidPatternReportsNotOk) {
  Re2Matcher m("([");
  EXPECT_FALSE(m.ok());
  EXPECT_FALSE(m.Matches("([ anything"));
}

TEST(Re2Matcher, EmptyPatternIsOk) {
  // An empty pattern is valid RE2 (matches everything); callers treat an
  // empty query as "search inactive" and never reach the matcher.
  Re2Matcher m("");
  EXPECT_TRUE(m.ok());
}

}  // namespace
}  // namespace markit
