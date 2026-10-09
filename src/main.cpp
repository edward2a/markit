#include <algorithm>
#include <atomic>  // for atomic
#include <chrono>  // for milliseconds (foreground poll interval)
#include <condition_variable>  // for condition_variable
#include <cstdint>  // for uint64_t
#include <cstdlib>
#include <filesystem>  // for is_directory
#include <fstream>
#include <functional>  // for function
#include <iostream>
#include <iterator>
#include <memory>  // for shared_ptr, make_shared
#include <mutex>  // for mutex, recursive_mutex
#include <optional>  // for optional
#include <string>
#include <thread>  // for thread
#include <utility>  // for pair
#include <vector>

#include <ftxui/component/app.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/loop.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include "chrome.hpp"
#include "anchor.hpp"
#include "config.hpp"
#include "layout_snapshot.hpp"
#include "markdown.hpp"
#include "scroller.hpp"
#include "search.hpp"

using namespace ftxui;

namespace {

void PrintUsage(std::ostream& out) {
  out << "Usage: markit [options] <file>\n"
      << "\n"
      << "Options:\n"
      << "  -h, --help          Show this help message and exit.\n"
      << "  --version           Show the version and exit.\n"
      << "  --config <file>     Path to a YAML config file (default: "
         "~/.config/markit/markit.yml).\n"
      << "  --dump-config       Print the default config (as YAML) to stdout "
         "and exit.\n"
      << "  --debug <file>      Write debug event/scroll logs to <file>.\n"
      << "\n"
      << "Arguments:\n"
      << "  <file>              Markdown file to display.\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::string debug_file;
  std::string config_file;
  bool dump_config = false;
  bool help = false;
  bool version = false;
  const char* input_file = nullptr;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--config" && i + 1 < argc) {
      config_file = argv[++i];
    } else if (arg == "--debug" && i + 1 < argc) {
      debug_file = argv[++i];
    } else if (arg == "--dump-config") {
      dump_config = true;
    } else if (arg == "-h" || arg == "--help") {
      help = true;
    } else if (arg == "--version") {
      version = true;
    } else if (input_file == nullptr) {
      input_file = argv[i];
    } else {
      std::cerr << "error: unexpected argument '" << arg << "'\n";
      return EXIT_FAILURE;
    }
  }

  if (version) {
    std::cout << "markit " << MARKIT_VERSION << "\n";
    return EXIT_SUCCESS;
  }

  if (help) {
    PrintUsage(std::cout);
    return EXIT_SUCCESS;
  }

  if (dump_config) {
    markit::DumpDefaultConfig(std::cout);
    return EXIT_SUCCESS;
  }

  if (input_file == nullptr) {
    PrintUsage(std::cerr);
    return EXIT_FAILURE;
  }

  const std::string config_path =
      config_file.empty() ? markit::DefaultConfigPath() : config_file;
  markit::Config config;
  try {
    config = markit::LoadConfig(config_path);
  } catch (const std::exception& e) {
    std::cerr << e.what() << "\n";
    return EXIT_FAILURE;
  }

  std::error_code input_error;
  if (std::filesystem::is_directory(input_file, input_error)) {
    std::cerr << "error: cannot read '" << input_file << "': is a directory\n";
    return EXIT_FAILURE;
  }

  std::ifstream file(input_file);
  if (!file.is_open()) {
    std::cerr << "error: cannot open '" << input_file << "'\n";
    return EXIT_FAILURE;
  }

  std::string contents;
  try {
    contents.assign(std::istreambuf_iterator<char>(file),
                    std::istreambuf_iterator<char>());
  } catch (const std::exception& e) {
    std::cerr << "error: cannot read '" << input_file << "': " << e.what()
              << "\n";
    return EXIT_FAILURE;
  }
  if (file.bad()) {
    std::cerr << "error: cannot read '" << input_file << "'\n";
    return EXIT_FAILURE;
  }

  if (contents.empty()) {
    contents = "(empty file)";
  }

  std::ofstream debug;
  if (!debug_file.empty()) {
    debug.open(debug_file);
    if (!debug.is_open()) {
      std::cerr << "error: cannot open debug file '" << debug_file << "'\n";
      return EXIT_FAILURE;
    }
  }

  auto log = [&](const std::string& what, int before, int after) {
    if (debug.is_open()) {
      debug << what << ": " << before << " -> " << after << "\n";
      debug.flush();
    }
  };

  // Display mode state: rendered from `content_cfg`, toggled live with `w`;
  // `hscroll` tells the scroller to enable horizontal panning (scroll mode).
  markit::Config content_cfg = config;
  bool hscroll = (config.horizontal_wrap == markit::WrapMode::Scroll);
  const bool is_empty_placeholder = (contents == "(empty file)");

  int selected = 0;
  int selected_x = 0;
  int viewport_height = 0;
  int viewport_width = 0;
  int content_height = 0;
  bool nav_visible = config.nav_visible;
  // Nav keyboard focus (Tab switches main view <-> nav bar): cursor is the
  // keyboard-selected heading index, offset the first visible heading row
  // (cursor follow-scroll; the outline clips beyond the viewport height).
  bool nav_focused = false;
  int nav_cursor = -1;
  int nav_offset = 0;
  // In-document search (`/` opens, Esc closes): the query text being typed.
  // Matches/match cursor live in the search cache below.
  bool search_open = false;
  std::string search_query;
  // Wrap-mode content height for the current viewport width, supplied by the
  // document layout snapshot below and consumed by the scroller. -1 disables
  // the override (scroll mode / content that does not reflow).
  int wrap_height = -1;
  // Immutable layout snapshot of `cached_content` at the current width. It is
  // the authoritative wrap-mode content height, complete beyond the old
  // 65,536-row measurement cap, and the source of structural heading spans.
  // Rebuilt only when the tree or width changes. Held by shared_ptr so a mode
  // toggle can build the new tree against a fresh snapshot while the old one
  // is still needed for offset mapping.
  std::shared_ptr<markit::LayoutSnapshot> content_snapshot =
      std::make_shared<markit::LayoutSnapshot>();
  int content_snapshot_width = -1;

  // Building the content tree re-parses the whole document, so cache it and
  // rebuild only when the display mode changes. The viewport size is
  // refreshed on every render instead, so a terminal resize takes effect
  // immediately (the scroller reads these refs for its layout math).
  // Declared early: the search state refresh below runs inside the content
  // renderer and reads the cached tree.
  markit::WrapMode rendered_mode = content_cfg.horizontal_wrap;
  Element cached_content;
  Element cached_highlight;
  Element highlighted_base;

  // Static nav content: headings extracted once (no interaction yet).
  const std::vector<markit::Heading> headings =
      markit::ExtractHeadings(contents);

  // Nav-highlight cache: heading (row, heading-index) pairs for the live
  // content tree. Locating headings needs an offscreen layout, so it runs
  // only when the tree, its render width, or the mode changes — never per
  // frame. The tree pointer is the cache key: `cached_content` is rebuilt
  // only on mode toggle. `selected` then maps to the last boundary at or
  // above the top of view (preamble, before the first boundary, is -1).
  ftxui::Element hl_tree;
  int hl_width = -1;
  bool hl_scroll = false;
  std::vector<std::pair<int, int>> hl_map;

  // Search state. Rows are plain-text copies of the live tree's visible rows
  // (indices align 1:1 with scroll offsets); matches and the compiled matcher
  // are immutable worker results; `search_pos` is the index of the current
  // match (-1 before the first jump); `search_invalid` flags a query that
  // failed to compile; `search_pending` flags a result not yet adopted. An
  // empty query is "search inactive" and matches nothing.
  //
  // Row extraction (one full offscreen layout) runs on ONE short-lived
  // worker thread so it never blocks input. The worker owns its private
  // tree, rebuilt from the immutable `contents`; the loop shares only
  // immutable inputs plus the handoff below. The loop side uses try_lock
  // exclusively and at most one worker runs at a time; stale query, layout,
  // and document generations are discarded on landing.
  // App-owned render guard: one recursive mutex covers the foreground
  // RunOnce() step (including FTXUI's actual draw) and every offscreen
  // screen/layout/cell-extraction transaction. Ordinary idle waits, matching,
  // publication and worker joins stay outside it. FTXUI's own lifecycle waits
  // and suspend/resume remain inside it as the accepted exception.
  std::recursive_mutex render_mu;

  // Worker completion notification: the worker clears its running flag and
  // posts Event::Custom on every ending (result, stale discard, no match,
  // invalid pattern, cancellation), then signals this predicate. The
  // foreground polls with a bounded timeout as a fallback for keyboard input
  // because this FTXUI API exposes no public input wait handle.
  std::mutex completion_mu;
  std::condition_variable completion_cv;
  bool worker_completed = false;  // guarded by completion_mu.

  std::mutex search_mu;
  struct SearchResult {
    uint64_t generation = 0;
    uint64_t document_generation = 0;
    uint64_t layout_generation = 0;
    int viewport = -1;
    bool scroll = false;
    std::string query;
    bool case_sensitive = false;
    std::shared_ptr<const std::vector<std::string>> rows;
    std::shared_ptr<const markit::Re2Matcher> matcher;
    std::vector<int> matches;
    bool invalid = false;
    bool error = false;  // worker failed or thread start failed.
  };
  struct SearchRowsResult {
    uint64_t document_generation = 0;
    uint64_t layout_generation = 0;
    int viewport = -1;
    bool scroll = false;
    std::shared_ptr<const std::vector<std::string>> rows;
  };
  std::optional<SearchResult> search_bg;  // guarded by search_mu.
  bool search_result_ready = false;  // guarded by search_mu.
  std::optional<SearchRowsResult> search_rows_bg;  // guarded by search_mu.
  bool search_rows_result_ready = false;  // guarded by search_mu.
  std::atomic<uint64_t> search_gen{0};  // generation of the flight/result.
  std::atomic<uint64_t> search_document_gen{1};
  std::atomic<uint64_t> search_layout_gen{0};
  std::atomic<bool> search_worker_running{false};
  std::thread search_worker;
  std::shared_ptr<const std::vector<std::string>> search_rows;
  int search_w_viewport = -1;  // inputs the rows were extracted for.
  bool search_w_scroll = false;
  bool search_rows_valid = false;
  std::string search_compiled;
  bool search_case = false;
  std::shared_ptr<const markit::Re2Matcher> search_matcher;
  bool search_matcher_ready = false;
  bool search_invalid = false;
  bool search_error = false;  // last adopted worker result failed.
  bool search_pending = false;
  std::vector<int> search_matches;
  int search_pos = -1;
  std::string search_requested_query;
  bool search_requested_case = false;
  uint64_t search_requested_document = 0;
  int search_requested_viewport = -1;
  bool search_requested_scroll = false;
  bool search_request_seen = false;

  // One deferred Enter jump (pending-Enter option 2:A). It is keyed by the
  // accepted query and geometry so it only fires against matching current
  // results; a superseding query/layout or intervening navigation cancels it.
  bool pending_jump_active = false;
  std::string pending_jump_query;
  bool pending_jump_case = false;
  uint64_t pending_jump_document = 0;
  uint64_t pending_jump_layout = 0;
  int pending_jump_from_row = 0;

  // Focus held before the search prompt opened, restored by Esc (focus option
  // 1:B). Enter always closes the prompt and focuses the document.
  bool pre_search_nav_focused = false;

  // Live-view mark for the current match (SearchHighlight overlay): the
  // target row plus its match byte spans, recomputed in
  // refresh_search_state only when the query, target, or rows change —
  // never per frame. -1/empty renders the content untouched.
  int hl_match_row = -1;
  std::vector<std::pair<int, int>> hl_match_spans;
  std::string hl_match_query;
  bool hl_match_case = false;

  // Forward hook so the content renderer (below) can refresh match state
  // before drawing; assigned beside the worker code further down (the
  // spawn side needs `screen`, declared later). Content draws before the
  // status bar in the same pass and renders happen on demand, so a mark
  // refreshed only in the status phase would display one stale frame.
  std::function<void()> refresh_search_state;

  // The chrome takes screen space the content must not use: one separator
  // row + action row + status row vertically, and the nav column (plus its
  // separator) horizontally when visible. The search prompt lives in the
  // action-bar row, so opening it never resizes the viewport.
  constexpr int kChromeRows = 3;

  // Building the content tree re-parses the whole document, so cache it and
  // rebuild only when the display mode changes. (Declarations live above,
  // beside the search state that reads the cache.)
  auto content = Renderer([&] {
    const auto term_size = Terminal::Size();
    viewport_width = std::max(
        1, term_size.dimx - (nav_visible ? markit::kNavWidth + 1 : 0));
    viewport_height = std::max(1, term_size.dimy - kChromeRows);
    if (!cached_content || rendered_mode != content_cfg.horizontal_wrap) {
      rendered_mode = content_cfg.horizontal_wrap;
      Element fresh = markit::RenderMarkdownRecording(
          contents, content_cfg.theme, content_cfg.horizontal_wrap,
          *content_snapshot);
      cached_content =
          is_empty_placeholder ? fresh | dim : std::move(fresh);
      content_snapshot_width = -1;  // tree changed; recapture below.
    }
    // Capture the wrap-mode height from the snapshot (one layout pass, no
    // full-height screen) and hand it to the scroller, so a tall document is
    // no longer capped at 65,536 rows. Scroll mode has no wrapping, so the
    // natural requirement height is already complete; the snapshot is still
    // captured there for structural heading spans.
    if (content_snapshot_width != viewport_width) {
      content_snapshot->Build(cached_content, viewport_width);
      content_snapshot_width = viewport_width;
    }
    if (!hscroll) {
      wrap_height = content_snapshot->height();
    } else {
      wrap_height = -1;
    }
    // Match state is prepared by the service step before this draw; the
    // renderer only reads the already-computed row/span pointers, so no RE2
    // work or worker join happens inside a render callback.
    if (hl_match_row >= 0 && !hl_match_spans.empty()) {
      // The highlight node stores pointers to the stable row/span state above.
      // Reusing the wrapper keeps the scroller's requirement cache valid while
      // the match moves within the same content tree.
      if (!cached_highlight || highlighted_base.get() != cached_content.get()) {
        cached_highlight = markit::SearchHighlight(
            cached_content, &hl_match_row, &hl_match_spans);
        highlighted_base = cached_content;
      }
      return cached_highlight;
    }
    cached_highlight.reset();
    highlighted_base.reset();
    return cached_content;
  });

  // Refresh the heading-row map when the tree, its render width, or the
  // mode changed. Shared by the highlight renderer and the Enter jump so
  // both agree on section boundaries.
  auto refresh_heading_map = [&] {
    if (cached_content && viewport_width >= 1 && viewport_height >= 1 &&
        content_snapshot->height() > 0 &&
        (!hl_tree || hl_tree.get() != cached_content.get() ||
         hl_width != viewport_width || hl_scroll != hscroll)) {
      hl_map = markit::LocateHeadingRows(*content_snapshot);
      hl_tree = cached_content;
      hl_width = viewport_width;
      hl_scroll = hscroll;
    }
  };

  // Index of the section containing the top of view (-1 in the preamble).
  auto current_heading = [&] {
    refresh_heading_map();
    int current = -1;
    for (const auto& [row, idx] : hl_map) {
      if (row <= selected) {
        current = idx;
      } else {
        break;
      }
    }
    return current;
  };

  // Visible nav rows below the title + separator.
  auto nav_window_height = [&] { return std::max(1, viewport_height - 2); };

  auto scroller =
      Scroller(std::move(content), &selected, &viewport_height,
               [&](int before, int after) { log("scroll", before, after); },
               &selected_x, &viewport_width, &hscroll, &content_height,
               &wrap_height, &config.keybindings);

  auto clamp_selected = [&] {
    const int max_offset = std::max(0, content_height - viewport_height);
    selected = std::clamp(selected, 0, max_offset);
  };

  // Jump the main view to heading `idx`: its hl_map boundary row (nearest
  // located section at or before it when the heading itself has no row,
  // e.g. an empty fingerprint), clamped like any scroll.
  auto jump_to_heading = [&](int idx) {
    pending_jump_active = false;  // heading navigation cancels a deferred jump.
    refresh_heading_map();
    int row = 0;
    for (const auto& [r, i] : hl_map) {
      if (i <= idx) {
        row = r;
      }
    }
    const int before = selected;
    selected = row;
    selected_x = 0;
    clamp_selected();
    log("goto", before, selected);
  };

  auto screen = App::Fullscreen();

  // Keep the worker generation tied to every input that can change its
  // result. Query typing and terminal resize invalidate an older flight even
  // when the replacement worker cannot start until the old one exits.
  auto sync_search_request = [&] {
    const uint64_t document = search_document_gen.load();
    const bool layout_changed =
        !search_request_seen || search_requested_document != document ||
        search_requested_viewport != viewport_width ||
        search_requested_scroll != hscroll;
    if (!layout_changed && search_request_seen &&
        search_requested_query == search_query &&
        search_requested_case == config.search_case_sensitive) {
      return;
    }
    search_request_seen = true;
    search_requested_query = search_query;
    search_requested_case = config.search_case_sensitive;
    search_requested_document = document;
    search_requested_viewport = viewport_width;
    search_requested_scroll = hscroll;
    if (layout_changed) {
      search_layout_gen.fetch_add(1);
    }
    search_gen.fetch_add(1);
    std::lock_guard<std::mutex> lock(search_mu);
    if (layout_changed) {
      search_rows_bg.reset();
      search_rows_result_ready = false;
    } else if (search_result_ready && search_bg && search_bg->rows) {
      // A completed scan for the previous query still produced reusable rows.
      // Preserve only that immutable layout result; its query/matches are
      // stale and are intentionally discarded.
      search_rows_bg = SearchRowsResult{
          search_bg->document_generation, search_bg->layout_generation,
          search_bg->viewport, search_bg->scroll, search_bg->rows};
      search_rows_result_ready = true;
    }
    search_bg.reset();
    search_result_ready = false;
  };

  // The worker owns both layout extraction and query matching. Results carry
  // immutable rows and the compiled matcher, so the loop never copies rows or
  // rescans the document while handling input.
  auto spawn_search_worker = [&] {
    sync_search_request();
    const uint64_t gen = search_gen.load();
    const uint64_t document = search_document_gen.load();
    const uint64_t layout = search_layout_gen.load();
    const markit::Theme worker_theme = content_cfg.theme;
    const markit::WrapMode worker_mode = content_cfg.horizontal_wrap;
    const std::string* source = &contents;
    const int vw = viewport_width;
    const bool sc = hscroll;
    const std::string query = search_query;
    const bool case_sensitive = config.search_case_sensitive;
    const bool rows_current =
        search_rows && search_rows_valid && search_w_viewport == vw &&
        search_w_scroll == sc;
    // A query-only revision reuses immutable rows and must not recompute the
    // tree requirement on the UI loop. Width is ignored in that case.
    const int width = rows_current
                          ? std::max(1, viewport_width)
                          : markit::SearchExtractWidth(cached_content, vw, sc);
    const std::shared_ptr<const std::vector<std::string>> input_rows =
        rows_current ? search_rows : nullptr;
    // The caller reaps any finished flight before spawning, so no join here.
    search_worker_running.store(true);
    try {
      search_worker = std::thread(
        [&, gen, document, layout, source, worker_theme, worker_mode, vw, sc,
         width, query, case_sensitive, input_rows] {
          // A flight is obsolete once its generation or layout/document
          // changes; the foreground bumps these on query/layout/shutdown.
          auto cancelled = [&] {
            return gen != search_gen.load() ||
                   document != search_document_gen.load() ||
                   layout != search_layout_gen.load();
          };
          SearchResult result;
          result.generation = gen;
          result.document_generation = document;
          result.layout_generation = layout;
          result.viewport = vw;
          result.scroll = sc;
          result.query = query;
          result.case_sensitive = case_sensitive;
          bool aborted = false;
          bool layout_failed = false;
          try {
            std::shared_ptr<const std::vector<std::string>> rows = input_rows;
            if (!rows && !cancelled()) {
              // Build a complete immutable snapshot instead of rendering
              // bounded screen tiles: one layout pass, no 65,536-row cap and
              // no full-height screen allocation. The tree is a pure function
              // of the immutable source and settings, so it is safe off the
              // foreground loop and needs no shared render guard.
              markit::LayoutSnapshot snapshot;
              bool aborted_build = false;
              markit::BuildMarkdownSnapshot(*source, worker_theme, worker_mode,
                                            width, snapshot, cancelled,
                                            &aborted_build);
              if (snapshot.failed()) {
                layout_failed = true;  // checked limit exceeded; report error.
              } else if (!aborted_build && !cancelled()) {
                rows = std::make_shared<const std::vector<std::string>>(
                    snapshot.TextRows());
              }
            }
            if (cancelled()) {
              aborted = true;  // rows (if any) may be a partial extraction.
            } else if (layout_failed) {
              result.error = true;
            } else {
              result.rows = std::move(rows);
              std::shared_ptr<const markit::Re2Matcher> matcher;
              std::vector<int> matches;
              bool invalid = false;
              if (!query.empty()) {
                matcher = std::make_shared<markit::Re2Matcher>(query,
                                                               case_sensitive);
                invalid = !matcher->ok();
                if (!invalid) {
                  matches = markit::FindMatches(*result.rows, *matcher,
                                                cancelled);
                }
              }
              if (cancelled()) {
                aborted = true;  // match scan may be incomplete.
              } else {
                result.matcher = std::move(matcher);
                result.matches = std::move(matches);
                result.invalid = invalid;
              }
            }
          } catch (...) {
            // Publish an explicit failure instead of leaving the UI pending.
            result = SearchResult{};
            result.generation = gen;
            result.document_generation = document;
            result.layout_generation = layout;
            result.viewport = vw;
            result.scroll = sc;
            result.query = query;
            result.case_sensitive = case_sensitive;
            result.error = true;
            aborted = false;
          }
          {
            std::lock_guard<std::mutex> lock(search_mu);
            const bool layout_current =
                document == search_document_gen.load() &&
                layout == search_layout_gen.load();
            if (!aborted && gen == search_gen.load() && layout_current) {
              search_bg = std::move(result);
              search_result_ready = true;
            } else if (!aborted && layout_current && result.rows &&
                       !result.error) {
              // The query became stale while layout work was running. Keep
              // the immutable rows for the latest query, but never publish
              // the stale matcher or match indices.
              search_rows_bg = SearchRowsResult{
                  result.document_generation, result.layout_generation,
                  result.viewport, result.scroll, std::move(result.rows)};
              search_rows_result_ready = true;
            }
          }
          // Every ending clears running and wakes the loop unconditionally so
          // a stale/cancelled/zero-result/error job can be replaced without
          // another keypress. Publish under the mailbox mutex first.
          search_worker_running.store(false);
          screen.PostEvent(Event::Custom);
          {
            std::lock_guard<std::mutex> lock(completion_mu);
            worker_completed = true;
          }
          completion_cv.notify_one();
        }
      );
    } catch (...) {
      // Thread construction failed: record an explicit error result and wake
      // the loop so the prompt does not stay pending forever.
      search_worker_running.store(false);
      {
        std::lock_guard<std::mutex> lock(search_mu);
        SearchResult result;
        result.generation = gen;
        result.document_generation = document;
        result.layout_generation = layout;
        result.viewport = vw;
        result.scroll = sc;
        result.query = query;
        result.case_sensitive = case_sensitive;
        result.error = true;
        search_bg = std::move(result);
        search_result_ready = true;
      }
      screen.PostEvent(Event::Custom);
      {
        std::lock_guard<std::mutex> lock(completion_mu);
        worker_completed = true;
      }
      completion_cv.notify_one();
    }
  };

  // Ensure a current row/match result is coming. A blank query still permits
  // a row-only prewarm when the prompt opens; non-blank queries include the
  // matcher and full scan in the same worker result.
  auto ensure_search_rows = [&] {
    sync_search_request();
    if (!cached_content || viewport_width < 1 || viewport_height < 1) {
      return;
    }
    const bool rows_current =
        search_rows && search_rows_valid &&
        search_w_viewport == viewport_width && search_w_scroll == hscroll;
    const bool query_current =
        search_query.empty() ||
        (search_compiled == search_query &&
         search_case == config.search_case_sensitive &&
         (search_invalid || search_error || search_matcher_ready));
    bool result_ready = false;
    bool rows_result_ready = false;
    {
      std::lock_guard<std::mutex> lock(search_mu);
      result_ready = search_result_ready;
      rows_result_ready = search_rows_result_ready;
    }
    if ((!rows_current || !query_current) &&
        !search_worker_running.load() && !result_ready && !rows_result_ready) {
      spawn_search_worker();
    }
  };

  // Adopt the worker result and refresh only the single-row highlight on the
  // loop. Matching itself is generation-guarded and never runs here.
  refresh_search_state = [&] {
    sync_search_request();
    bool rows_rebuilt = false;
    std::optional<SearchRowsResult> rows_result;
    std::optional<SearchResult> result;
    {
      std::unique_lock<std::mutex> lock(search_mu, std::try_to_lock);
      if (lock.owns_lock() && search_result_ready) {
        result = std::move(search_bg);
        search_bg.reset();
        search_result_ready = false;
      }
      if (lock.owns_lock() && search_rows_result_ready) {
        rows_result = std::move(search_rows_bg);
        search_rows_bg.reset();
        search_rows_result_ready = false;
      }
    }
    // The worker publishes before returning and is reaped by the service step
    // before adoption, so the immutable matcher is safe to use here and no
    // join or RE2 work happens inside a render callback.
    if (rows_result &&
        rows_result->document_generation == search_document_gen.load() &&
        rows_result->layout_generation == search_layout_gen.load() &&
        rows_result->viewport == viewport_width &&
        rows_result->scroll == hscroll) {
      search_rows = std::move(rows_result->rows);
      search_w_viewport = viewport_width;
      search_w_scroll = hscroll;
      search_rows_valid = static_cast<bool>(search_rows);
      rows_rebuilt = true;
    }
    if (result && result->generation == search_gen.load() &&
        result->document_generation == search_document_gen.load() &&
        result->layout_generation == search_layout_gen.load() &&
        result->viewport == viewport_width && result->scroll == hscroll &&
        result->query == search_query &&
        result->case_sensitive == config.search_case_sensitive) {
      const bool query_changed =
          search_compiled != result->query ||
          search_case != result->case_sensitive;
      search_rows = std::move(result->rows);
      search_w_viewport = result->viewport;
      search_w_scroll = result->scroll;
      search_rows_valid = static_cast<bool>(search_rows);
      rows_rebuilt = true;
      search_matcher = std::move(result->matcher);
      search_matcher_ready = static_cast<bool>(search_matcher);
      search_matches = std::move(result->matches);
      search_invalid = result->invalid;
      search_error = result->error;
      search_compiled = result->query;
      search_case = result->case_sensitive;
      search_pending = false;
      if (query_changed) {
        search_pos = -1;
      } else {
        search_pos = std::clamp(
            search_pos, -1, static_cast<int>(search_matches.size()) - 1);
      }
    }

    if (search_query.empty()) {
      // Inactive: drop matches but keep extracted rows and any flight so
      // reopening the prompt can reuse the immutable row set.
      search_matches.clear();
      search_pos = -1;
      search_invalid = false;
      search_error = false;
      search_pending = false;
      search_compiled.clear();
      search_case = false;
      search_matcher.reset();
      search_matcher_ready = false;
      hl_match_row = -1;
      hl_match_spans.clear();
      hl_match_query.clear();
      return;
    }
    if (!cached_content || viewport_width < 1 || viewport_height < 1) {
      return;
    }
    // Rows stale for the current tree/width? The old matches belong to
    // another layout, so drop them and show pending; refresh_search below
    // ensures a worker for the current request.
    if (!search_rows_valid || !search_rows ||
        search_w_viewport != viewport_width || search_w_scroll != hscroll) {
      search_rows_valid = false;
      search_matches.clear();
      search_matcher.reset();
      search_matcher_ready = false;
    }
    const bool query_current =
        search_compiled == search_query &&
        search_case == config.search_case_sensitive &&
        (search_invalid || search_error || search_matcher_ready);
    if (search_error) {
      search_pending = false;
      hl_match_row = -1;
      hl_match_spans.clear();
      hl_match_query.clear();
      return;
    }
    if (!search_rows_valid || !query_current) {
      search_pending = true;
      hl_match_row = -1;
      hl_match_spans.clear();
      hl_match_query.clear();
      return;
    }
    search_pending = false;
    const int count = static_cast<int>(search_matches.size());
    int target = -1;
    if (count > 0) {
      target = (search_pos >= 0 && search_pos < count)
                   ? search_matches[search_pos]
                   : markit::NextMatch(search_matches, selected - 1, +1);
    }
    if (target < 0 || target >= static_cast<int>(search_rows->size()) ||
        !search_matcher) {
      hl_match_row = -1;
      hl_match_spans.clear();
      hl_match_query.clear();
    } else if (target != hl_match_row || hl_match_query != search_query ||
               hl_match_case != config.search_case_sensitive ||
               rows_rebuilt) {
      hl_match_spans = search_matcher->FindSpans((*search_rows)[target]);
      hl_match_row = target;
      hl_match_query = search_query;
      hl_match_case = config.search_case_sensitive;
    }
  };

  // Perform a match jump (no pending-intent side effects) from the current
  // `selected`/`search_pos` state.
  auto perform_match_jump = [&](int dir) {
    if (search_matches.empty()) {
      return;
    }
    const int count = static_cast<int>(search_matches.size());
    int row = search_matches.front();
    if (search_pos < 0 || search_pos >= count) {
      row = markit::NextMatch(search_matches,
                              dir > 0 ? selected - 1 : selected + 1, dir);
      for (int i = 0; i < count; ++i) {
        if (search_matches[i] == row) {
          search_pos = i;
          break;
        }
      }
    } else {
      search_pos = (search_pos + dir + count) % count;
      row = search_matches[search_pos];
    }
    const int before = selected;
    selected = row;
    selected_x = 0;
    clamp_selected();
    log("search", before, selected);
  };

  // Jump to the next (dir > 0) or previous (dir < 0) match, wrapping around.
  // The first jump after a query change lands on the first match at or below
  // (above, for dir < 0) the top of view: NextMatch is strict, so the seed
  // is offset by one row to make the first jump inclusive. Any navigation
  // cancels a deferred Enter jump.
  auto goto_match = [&](int dir) {
    pending_jump_active = false;
    perform_match_jump(dir);
  };

  // Service the search worker outside every render/mailbox lock and outside
  // the render guard: reap a finished flight, adopt its immutable result,
  // execute at most one deferred Enter jump, then schedule the latest needed
  // work. Called from the foreground loop between RunOnce steps.
  auto service_search = [&] {
    if (search_worker.joinable() && !search_worker_running.load()) {
      search_worker.join();  // posting already stopped; never a live flight.
    }
    // Adopt any published result and recompute the single-row mark outside
    // every lock and the render guard. Running this every iteration keeps the
    // mark current after navigation, not only after a worker completion.
    const int prev_row = hl_match_row;
    const std::vector<std::pair<int, int>> prev_spans = hl_match_spans;
    const bool prev_pending = search_pending;
    const bool prev_invalid = search_invalid;
    const bool prev_error = search_error;
    const size_t prev_match_count = search_matches.size();
    refresh_search_state();
    if (pending_jump_active) {
      const bool superseded =
          search_query != pending_jump_query ||
          config.search_case_sensitive != pending_jump_case ||
          search_document_gen.load() != pending_jump_document ||
          search_layout_gen.load() != pending_jump_layout;
      if (superseded) {
        pending_jump_active = false;
      } else if (search_rows_valid && !search_pending &&
                 search_compiled == pending_jump_query &&
                 search_case == pending_jump_case) {
        // Results for the accepted query/layout are adopted. Consume the
        // intent exactly once; zero matches consume without movement.
        const int from_row = pending_jump_from_row;
        pending_jump_active = false;
        if (!search_matches.empty()) {
          selected = from_row;
          search_pos = -1;
          perform_match_jump(+1);
        }
      }
    }
    if (hl_match_row != prev_row || hl_match_spans != prev_spans ||
        search_pending != prev_pending || search_invalid != prev_invalid ||
        search_error != prev_error ||
        search_matches.size() != prev_match_count) {
      // The mark or the status changed without an input event (navigation,
      // deferred jump, or worker adoption): ask for one more draw so neither
      // is left stale, including the "..." -> count/"search error" flip.
      screen.PostEvent(Event::Custom);
    }
    // Only run worker work when a search is active: an idle viewer must not
    // prewarm an expensive whole-document extraction.
    if (search_open || !search_query.empty()) {
      ensure_search_rows();
    }
  };

  auto status_bar = Renderer([&] {
    const int max_offset = std::max(0, content_height - viewport_height);
    const int current = std::clamp(selected, 0, max_offset);
    std::string search_suffix;
    if (!search_query.empty()) {
      // Persistent match counter: visible while typing and while navigating
      // with n/N after the prompt closed. `/` clears the query (fresh
      // search), which hides the counter again. "..." while the worker rows
      // are still on their way. State is prepared by the service step; the
      // renderer never adopts or schedules work.
      if (search_pending) {
        search_suffix = "...";
      } else if (search_error) {
        search_suffix = "search error";
      } else {
        search_suffix = markit::FormatSearchStatus(
            search_pos, static_cast<int>(search_matches.size()),
            search_invalid);
      }
    }
    return markit::StatusBar(input_file, current, max_offset,
                             content_cfg.horizontal_wrap, search_suffix);
  });
  auto search_input = Input(&search_query);
  // The prompt lives in the action-bar row while open: opening/closing never
  // resizes the viewport, so the scroller never re-lays-out the content for
  // search chrome. Renderer-with-child keeps Input focus handling.
  auto action_bar = Renderer(search_input, [&]() -> Element {
    if (search_open) {
      return hbox({text("/ "), search_input->Render() | flex,
                   text("  Enter:jump  Esc:close")});
    }
    return markit::ActionBar(nav_focused);
  });
  auto nav_bar = Renderer([&]() -> Element {
    if (!nav_visible) {
      return emptyElement();
    }
    const int current = current_heading();
    // The outline clips beyond the viewport: slice the visible window and
    // map indices relative to it (out-of-window -> -1, no highlight).
    const int visible = nav_window_height();
    nav_offset = markit::ClampNavOffset(
        nav_offset, static_cast<int>(headings.size()), visible);
    std::vector<markit::Heading> window;
    for (int i = nav_offset;
         i < static_cast<int>(headings.size()) && i < nav_offset + visible;
         ++i) {
      window.push_back(headings[i]);
    }
    const auto rel = [&](int idx) {
      const int r = idx - nav_offset;
      return (r >= 0 && r < static_cast<int>(window.size())) ? r : -1;
    };
    return markit::NavBar(window, rel(current),
                          nav_focused ? rel(nav_cursor) : -1, nav_focused);
  });
  auto nav_separator = Renderer([&]() -> Element {
    return nav_visible ? separator() : emptyElement();
  });

  auto left_column = Container::Vertical({
      scroller | flex,
      Renderer([] { return separator(); }),
      action_bar,
      status_bar,
  });
  auto root = Container::Horizontal({
      left_column | flex,
      nav_separator,
      nav_bar,
  });
  if (config.theme.background != ftxui::Color::Default) {
    root = root | ftxui::bgcolor(config.theme.background);
  }

  // All keys below come from the `keybindings:` config section (defaults
  // preserve the historical mappings). Structural order is the precedence:
  // search_cancel before quit, nav keys before the prompt/content fallthrough.
  const markit::KeyBindings& kb = config.keybindings;
  auto component = CatchEvent(root, [&](Event event) -> bool {
    if (debug.is_open()) {
      debug << "input: " << event.DebugString()
            << " (character: '" << event.character() << "')\n";
      debug.flush();
    }

    // Universal Ctrl+C: it must not be consumed by the prompt as query input
    // or downgraded to a search-only cancel. App's forced Ctrl+C handling
    // records SIGINT even though this returns true, so the terminal is
    // restored and the interrupt is re-raised after teardown.
    if (event == Event::CtrlC) {
      pending_jump_active = false;
      screen.Exit();
      return true;
    }

    // An open prompt owns every other key before global/nav shortcuts, so
    // q, w, n, slash and navigation letters become query input.
    if (search_open) {
      if (markit::MatchesKey(event, kb.search_cancel)) {
        // Esc restores the focus held before the prompt opened (1:B). The
        // query and matches are retained so n/N keep navigating.
        search_open = false;
        nav_focused =
            pre_search_nav_focused && nav_visible && !headings.empty();
        if (nav_focused) {
          const int current = current_heading();
          nav_cursor = current >= 0 ? current : 0;
          nav_offset = markit::FollowNavOffset(
              nav_offset, nav_cursor, static_cast<int>(headings.size()),
              nav_window_height());
        } else {
          scroller->TakeFocus();
        }
        return true;
      }
      if (markit::MatchesKey(event, kb.search_accept)) {
        // Enter closes the prompt and focuses the document now. If the
        // accepted query's results are not ready, retain exactly one jump
        // against this query and geometry (pending-Enter option 2:A).
        search_open = false;
        nav_focused = false;
        scroller->TakeFocus();
        const std::string accepted = search_query;
        const bool accepted_case = config.search_case_sensitive;
        const bool ready =
            !accepted.empty() && search_rows_valid && !search_pending &&
            search_compiled == accepted && search_case == accepted_case;
        if (ready) {
          perform_match_jump(+1);
        } else if (!accepted.empty()) {
          pending_jump_active = true;
          pending_jump_query = accepted;
          pending_jump_case = accepted_case;
          pending_jump_document = search_document_gen.load();
          pending_jump_layout = search_layout_gen.load();
          pending_jump_from_row = selected;
        }
        return true;
      }
      return false;  // everything else is query text.
    }

    if (markit::MatchesKey(event, kb.quit)) {
      pending_jump_active = false;
      screen.Exit();
      return true;
    }
    if (markit::MatchesKey(event, kb.focus_switch)) {
      // Tab is the only focus switch: main view <-> nav bar. No-op when
      // the nav is hidden or the document has no headings.
      if (nav_visible && !headings.empty()) {
        nav_focused = !nav_focused;
        if (nav_focused) {
          // Sync the cursor to the section in view (preamble -> first).
          const int current = current_heading();
          nav_cursor = current >= 0 ? current : 0;
          nav_offset = markit::FollowNavOffset(
              nav_offset, nav_cursor, static_cast<int>(headings.size()),
              nav_window_height());
        }
        log("focus", nav_focused ? 0 : 1, nav_focused ? 1 : 0);
      }
      return true;
    }
    if (nav_focused) {
      // Nav keys are consumed before the scroller: arrows/k/j move the
      // keyboard cursor, Enter jumps the main view (focus stays in nav).
      const int count = static_cast<int>(headings.size());
      const int visible = nav_window_height();
      auto follow = [&] {
        nav_offset = markit::FollowNavOffset(nav_offset, nav_cursor, count,
                                             visible);
      };
      if (markit::MatchesKey(event, kb.nav_up)) {
        nav_cursor = std::max(0, nav_cursor - 1);
        follow();
        return true;
      }
      if (markit::MatchesKey(event, kb.nav_down)) {
        nav_cursor = std::min(count - 1, nav_cursor + 1);
        follow();
        return true;
      }
      if (markit::MatchesKey(event, kb.nav_top)) {
        nav_cursor = 0;
        follow();
        return true;
      }
      if (markit::MatchesKey(event, kb.nav_bottom)) {
        nav_cursor = count - 1;
        follow();
        return true;
      }
      if (markit::MatchesKey(event, kb.nav_page_up)) {
        nav_cursor = std::max(0, nav_cursor - visible);
        follow();
        return true;
      }
      // Space still types into the search prompt while it is open, so the
      // nav page-down binding skips a literal space in that state.
      if (markit::MatchesKey(event, kb.nav_page_down) &&
          !(search_open && event == Event::Character(' '))) {
        nav_cursor = std::min(count - 1, nav_cursor + visible);
        follow();
        return true;
      }
      if (markit::MatchesKey(event, kb.nav_activate)) {
        jump_to_heading(nav_cursor);
        return true;
      }
    }
    // Any intervening navigation cancels a deferred Enter jump, including
    // boundary no-ops and horizontal panning.
    if (markit::MatchesKey(event, kb.scroll_up) ||
        markit::MatchesKey(event, kb.scroll_down) ||
        markit::MatchesKey(event, kb.page_up) ||
        markit::MatchesKey(event, kb.page_down) ||
        markit::MatchesKey(event, kb.goto_top) ||
        markit::MatchesKey(event, kb.goto_bottom) ||
        markit::MatchesKey(event, kb.pan_left) ||
        markit::MatchesKey(event, kb.pan_right)) {
      pending_jump_active = false;
    }
    if (markit::MatchesKey(event, kb.search_next)) {
      goto_match(+1);  // no-op without an active search.
      return true;
    }
    if (markit::MatchesKey(event, kb.search_prev)) {
      goto_match(-1);  // no-op without an active search.
      return true;
    }
    if (markit::MatchesKey(event, kb.search_open)) {
      // Remember the focus to restore on Esc (1:B); the prompt owns input
      // until it closes. The service step pre-warms the current rows.
      pending_jump_active = false;
      pre_search_nav_focused = nav_focused;
      nav_focused = false;
      search_open = true;
      search_query.clear();
      search_matches.clear();
      search_pos = -1;
      search_input->TakeFocus();
      return true;
    }
    if (markit::MatchesKey(event, kb.toggle_wrap)) {
      const bool old_is_scroll = hscroll;
      // Ensure the old snapshot is captured at the old mode's mapping width:
      // wrap uses the viewport width, scroll uses the natural width so the
      // anchor row text is complete (no clipping ambiguity).
      if (old_is_scroll) {
        const int old_width =
            markit::SearchExtractWidth(cached_content, viewport_width, true);
        content_snapshot->Build(cached_content, old_width);
        content_snapshot_width = old_width;
      } else if (content_snapshot_width != viewport_width) {
        content_snapshot->Build(cached_content, viewport_width);
        content_snapshot_width = viewport_width;
      }

      hscroll = !hscroll;
      search_document_gen.fetch_add(1);
      content_cfg.horizontal_wrap =
          hscroll ? markit::WrapMode::Scroll : markit::WrapMode::Wrap;
      selected_x = 0;  // re-anchor horizontally on mode switch.

      // Build the new tree against a fresh snapshot so the old snapshot stays
      // valid for the offset mapping, then map the offset between the two
      // complete snapshots (no 65,536-row cap).
      auto next_snapshot = std::make_shared<markit::LayoutSnapshot>();
      Element fresh = markit::RenderMarkdownRecording(
          contents, content_cfg.theme, content_cfg.horizontal_wrap,
          *next_snapshot);
      fresh = is_empty_placeholder ? fresh | dim : std::move(fresh);
      const int new_width =
          hscroll ? markit::SearchExtractWidth(fresh, viewport_width, true)
                  : viewport_width;
      next_snapshot->Build(fresh, new_width);

      selected = markit::MapTogglePosition(*content_snapshot, *next_snapshot,
                                           selected, viewport_height,
                                           old_is_scroll, nullptr);
      content_snapshot = std::move(next_snapshot);
      cached_content = std::move(fresh);
      rendered_mode = content_cfg.horizontal_wrap;
      content_snapshot_width = new_width;
      wrap_height = hscroll ? -1 : content_snapshot->height();
      log("mode", hscroll ? 0 : 1, hscroll ? 1 : 0);
      return true;
    }
    if (markit::MatchesKey(event, kb.toggle_nav)) {
      nav_visible = !nav_visible;
      if (!nav_visible) {
        nav_focused = false;  // focus cannot stay in a hidden nav.
      }
      clamp_selected();  // the content column changed size; stay in range.
      log("nav", nav_visible ? 0 : 1, nav_visible ? 1 : 0);
      return true;
    }
    return false;
  });

  // App-owned loop: one recursive render guard covers Loop construction/
  // destruction (terminal install/uninstall), each foreground RunOnce step,
  // and the worker's offscreen render. Ordinary waits, servicing, matching
  // and joins stay outside the guard.
  screen.ForceHandleCtrlC(true);

  std::unique_ptr<Loop> loop;
  {
    std::lock_guard<std::recursive_mutex> guard(render_mu);
    loop = std::make_unique<Loop>(&screen, component);
  }

  while (!loop->HasQuitted()) {
    // Adopt/reap/start worker work outside every lock and the render guard.
    service_search();
    {
      std::lock_guard<std::recursive_mutex> guard(render_mu);
      loop->RunOnce();
    }
    if (loop->HasQuitted()) {
      break;
    }
    // Bounded foreground poll: a worker completion shortens the wait, and the
    // timed fallback polls keyboard input and deferred signals because this
    // FTXUI API exposes no public input wait handle.
    std::unique_lock<std::mutex> lock(completion_mu);
    completion_cv.wait_for(lock, std::chrono::milliseconds(8),
                           [&] { return worker_completed; });
    worker_completed = false;
  }

  // Shutdown: invalidate wanted work, join the worker outside all guards,
  // then destroy Loop under the guard while App and captured state stay alive
  // so terminal restoration runs before SIGINT is re-raised.
  search_gen.fetch_add(1);
  pending_jump_active = false;
  if (search_worker.joinable()) {
    search_worker.join();
  }
  {
    std::lock_guard<std::recursive_mutex> guard(render_mu);
    loop.reset();  // ~Loop -> PostMain -> Uninstall.
  }
  return EXIT_SUCCESS;
}
