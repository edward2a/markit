#include "search.hpp"

#include <re2/re2.h>

namespace markit {

struct Re2Matcher::Impl {
  explicit Impl(const std::string& pattern, bool case_sensitive) {
    RE2::Options options;
    options.set_case_sensitive(case_sensitive);
    options.set_never_capture(true);  // presence only; skips capture overhead.
    re = std::make_unique<RE2>(pattern, options);
  }
  std::unique_ptr<RE2> re;
};

Re2Matcher::Re2Matcher(const std::string& pattern, bool case_sensitive)
    : impl_(std::make_unique<Impl>(pattern, case_sensitive)) {}

Re2Matcher::~Re2Matcher() = default;

bool Re2Matcher::Matches(const std::string& row) const {
  return impl_->re->ok() && RE2::PartialMatch(row, *impl_->re);
}

bool Re2Matcher::ok() const { return impl_->re->ok(); }

std::vector<int> FindMatches(const std::vector<std::string>& rows,
                             const Matcher& matcher) {
  std::vector<int> out;
  if (!matcher.ok()) {
    return out;
  }
  for (size_t i = 0; i < rows.size(); ++i) {
    if (matcher.Matches(rows[i])) {
      out.push_back(static_cast<int>(i));
    }
  }
  return out;
}

int NextMatch(const std::vector<int>& matches, int from, int dir) {
  if (matches.empty()) {
    return -1;
  }
  if (dir >= 0) {
    for (int row : matches) {
      if (row > from) {
        return row;
      }
    }
    return matches.front();  // wrap around.
  }
  for (auto it = matches.rbegin(); it != matches.rend(); ++it) {
    if (*it < from) {
      return *it;
    }
  }
  return matches.back();  // wrap around.
}

}  // namespace markit
