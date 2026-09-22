// Lightweight, opt-in performance benchmarks for markit core paths.
//
// This executable intentionally uses only the C++ standard library so the
// benchmark target does not add a production dependency. Results are emitted
// as CSV for easy before/after comparison.
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "anchor.hpp"
#include "config.hpp"
#include "markdown.hpp"
#include "search.hpp"

namespace {

using Clock = std::chrono::steady_clock;

// Prevent the optimizer from removing benchmark results.
volatile std::size_t g_sink = 0;

struct Options {
  int warmup = 1;
  int samples = 5;
};

struct Fixture {
  std::string name;
  std::string markdown;
  int viewport_width;
};

struct Summary {
  long long median_ns = 0;
  long long p95_ns = 0;
};

std::string MakeFixture(int blocks, int line_width, bool code_block) {
  std::string out;
  out.reserve(static_cast<std::size_t>(blocks) *
              static_cast<std::size_t>(line_width + 80));
  for (int i = 0; i < blocks; ++i) {
    out += "# Section ";
    out += std::to_string(i);
    out += "\n\n";
    out += "This is paragraph ";
    out += std::to_string(i);
    out += " with searchable needle text and repeated words for layout.\n";
    out += "The second line keeps the document representative of normal prose.\n\n";
    if (line_width > 80) {
      out += "wide-";
      out.append(static_cast<std::size_t>(line_width - 5), 'x');
      out += " needle\n\n";
    }
    if (code_block && i % 8 == 0) {
      out += "```text\n";
      out += "code needle ";
      out.append(static_cast<std::size_t>(std::max(0, line_width - 13)), 'c');
      out += "\n```\n\n";
    }
  }
  return out;
}

std::vector<Fixture> MakeFixtures() {
  return {
      {"small", MakeFixture(32, 80, false), 80},
      {"large", MakeFixture(1200, 80, true), 80},
      {"wrapped", MakeFixture(300, 240, true), 80},
      {"wide", MakeFixture(160, 2048, true), 80},
  };
}

markit::Config ConfigFor(markit::WrapMode mode) {
  markit::Config config;
  config.horizontal_wrap = mode;
  return config;
}

Summary Measure(const Options& options, const std::function<std::size_t()>& fn) {
  for (int i = 0; i < options.warmup; ++i) {
    g_sink += fn();
  }

  std::vector<long long> samples;
  samples.reserve(static_cast<std::size_t>(options.samples));
  for (int i = 0; i < options.samples; ++i) {
    const auto start = Clock::now();
    g_sink += fn();
    const auto end = Clock::now();
    samples.push_back(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start)
            .count());
  }
  std::sort(samples.begin(), samples.end());
  const std::size_t median = samples.size() / 2;
  const std::size_t p95 =
      std::min(samples.size() - 1,
               (samples.size() * static_cast<std::size_t>(95) + 99) / 100 - 1);
  return {samples[median], samples[p95]};
}

void PrintResult(const std::string& name, const Fixture& fixture,
                 const Options& options, const Summary& summary) {
  std::cout << name << ',' << fixture.name << ',' << fixture.markdown.size()
            << ',' << options.samples << ',' << summary.median_ns << ','
            << summary.p95_ns << '\n';
}

void BenchmarkFixture(const Fixture& fixture, const Options& options) {
  const markit::Config wrap = ConfigFor(markit::WrapMode::Wrap);
  const markit::Config scroll = ConfigFor(markit::WrapMode::Scroll);

  PrintResult(
      "render_markdown", fixture, options,
      Measure(options, [&] {
        const auto tree = markit::RenderMarkdown(fixture.markdown, wrap);
        return tree ? 1U : 0U;
      }));

  const auto wrap_tree = markit::RenderMarkdown(fixture.markdown, wrap);
  wrap_tree->ComputeRequirement();
  const int wrap_hint = std::max(1, wrap_tree->requirement().min_y);
  PrintResult(
      "render_text_rows_wrap", fixture, options,
      Measure(options, [&] {
        const auto rows =
            markit::RenderTextRows(wrap_tree, fixture.viewport_width, wrap_hint);
        return rows.size();
      }));

  const auto scroll_tree = markit::RenderMarkdown(fixture.markdown, scroll);
  const int scroll_width = markit::SearchExtractWidth(
      scroll_tree, fixture.viewport_width, /*is_scroll=*/true);
  scroll_tree->ComputeRequirement();
  const int scroll_hint = std::max(1, scroll_tree->requirement().min_y);
  PrintResult(
      "render_text_rows_scroll", fixture, options,
      Measure(options, [&] {
        const auto rows =
            markit::RenderTextRows(scroll_tree, scroll_width, scroll_hint);
        return rows.size();
      }));

  const auto old_tree = scroll_tree;
  const auto new_tree = wrap_tree;
  PrintResult(
      "map_toggle_position", fixture, options,
      Measure(options, [&] {
        int new_height = 0;
        return static_cast<std::size_t>(markit::MapTogglePosition(
            old_tree, new_tree, {}, 0, fixture.viewport_width, 24,
            /*old_is_scroll=*/true, &new_height));
      }));

  const auto rows = markit::RenderTextRows(wrap_tree, fixture.viewport_width,
                                           wrap_hint);
  std::cerr << "# fixture=" << fixture.name << " wrap_hint=" << wrap_hint
            << " scroll_width=" << scroll_width
            << " scroll_hint=" << scroll_hint
            << " wrap_rows=" << rows.size() << '\n';
  PrintResult(
      "matcher_construct", fixture, options,
      Measure(options, [&] {
        markit::Re2Matcher matcher("needle");
        return matcher.ok() ? 1U : 0U;
      }));

  const markit::Re2Matcher matcher("needle");
  PrintResult(
      "find_matches", fixture, options,
      Measure(options, [&] {
        return markit::FindMatches(rows, matcher).size();
      }));
}

bool ParsePositive(const char* value, int* out) {
  char* end = nullptr;
  const long parsed = std::strtol(value, &end, 10);
  if (end == value || *end != '\0' || parsed < 1 || parsed > 100000) {
    return false;
  }
  *out = static_cast<int>(parsed);
  return true;
}

void PrintUsage(std::ostream& out) {
  out << "Usage: markit-bench [--warmup N] [--samples N]\n";
}

bool ParseOptions(int argc, char** argv, Options* options) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      PrintUsage(std::cout);
      return false;
    }
    if ((arg == "--warmup" || arg == "--samples") && i + 1 < argc) {
      int value = 0;
      if (!ParsePositive(argv[++i], &value)) {
        std::cerr << "invalid value for " << arg << '\n';
        return false;
      }
      if (arg == "--warmup") {
        options->warmup = value;
      } else {
        options->samples = value;
      }
      continue;
    }
    std::cerr << "unknown argument: " << arg << '\n';
    PrintUsage(std::cerr);
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!ParseOptions(argc, argv, &options)) {
    return argc > 1 && (std::string(argv[1]) == "--help" ||
                        std::string(argv[1]) == "-h")
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
  }

  std::cout << "case,fixture,input_bytes,samples,median_ns,p95_ns\n";
  for (const Fixture& fixture : MakeFixtures()) {
    BenchmarkFixture(fixture, options);
  }
  return EXIT_SUCCESS;
}
