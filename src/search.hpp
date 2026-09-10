// In-document search for markit.
//
// The Matcher interface abstracts the regex engine (RE2 today) so unit tests
// can substitute fakes and a future engine slots in without touching callers.
// RE2 gives linear-time matching, which matters because the pattern is
// recompiled and re-run on every keystroke of incremental search.
#ifndef MARKIT_SEARCH_HPP
#define MARKIT_SEARCH_HPP

#include <memory>
#include <string>
#include <vector>

namespace markit {

// Abstract regex matcher over one text row: Matches() reports whether the
// pattern occurs anywhere in the row (unanchored search).
class Matcher {
 public:
  virtual ~Matcher() = default;
  virtual bool Matches(const std::string& row) const = 0;
  // False when the pattern failed to compile (callers show "invalid pattern"
  // and treat the query as matching nothing).
  virtual bool ok() const = 0;
  // Byte spans [start, end) of each non-empty match in the row, ascending
  // and non-overlapping. Empty when the pattern is invalid or matches
  // nothing (patterns matching empty strings contribute no spans). Used to
  // mark the current match in the live view; the default reports none so
  // test fakes only override what they exercise.
  virtual std::vector<std::pair<int, int>> FindSpans(
      const std::string& row) const {
    return {};
  }
};

// RE2-backed matcher, compiled once per query revision. `case_sensitive`
// false folds ASCII case; captures are disabled (only presence is needed).
class Re2Matcher : public Matcher {
 public:
  explicit Re2Matcher(const std::string& pattern,
                      bool case_sensitive = false);
  ~Re2Matcher() override;  // out-of-line: Impl is complete only in search.cpp.
  bool Matches(const std::string& row) const override;
  bool ok() const override;
  std::vector<std::pair<int, int>> FindSpans(
      const std::string& row) const override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Row indices (into `rows`) containing at least one match, ascending.
// An invalid matcher matches nothing.
std::vector<int> FindMatches(const std::vector<std::string>& rows,
                             const Matcher& matcher);

// Next match row strictly after `from` (dir > 0) or strictly before `from`
// (dir < 0), wrapping around. Strictness means repeated "next" advances off
// the current match instead of sticking to it; callers wanting an inclusive
// first jump pass `from = selected - 1` (forward) or `selected + 1`
// (backward). -1 when `matches` is empty. `matches` must be ascending (as
// returned by FindMatches).
int NextMatch(const std::vector<int>& matches, int from, int dir);

}  // namespace markit

#endif  // MARKIT_SEARCH_HPP
