#include <algorithm>
#include <atomic>  // for atomic
#include <cstdint>  // for uint64_t
#include <cstdlib>
#include <fstream>
#include <functional>  // for function
#include <iostream>
#include <iterator>
#include <mutex>  // for mutex
#include <string>
#include <thread>  // for thread
#include <utility>  // for pair
#include <vector>

#include <ftxui/component/app.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include "chrome.hpp"
#include "anchor.hpp"
#include "config.hpp"
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
    } else if (input_file == nullptr) {
      input_file = argv[i];
    } else {
      std::cerr << "error: unexpected argument '" << arg << "'\n";
      return EXIT_FAILURE;
    }
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

  std::ifstream file(input_file);
  if (!file.is_open()) {
    std::cerr << "error: cannot open '" << input_file << "'\n";
    return EXIT_FAILURE;
  }

  std::string contents((std::istreambuf_iterator<char>(file)),
                       std::istreambuf_iterator<char>());

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
  // Pre-measured wrap height from the toggle path: the anchor already renders
  // the new tree at the viewport width, so the scroller can adopt the height
  // instead of measuring again. Width -1 disables.
  int wrap_hint_w = -1;
  int wrap_hint_h = -1;

  // Building the content tree re-parses the whole document, so cache it and
  // rebuild only when the display mode changes. The viewport size is
  // refreshed on every render instead, so a terminal resize takes effect
  // immediately (the scroller reads these refs for its layout math).
  // Declared early: the search state refresh below runs inside the content
  // renderer and reads the cached tree.
  markit::WrapMode rendered_mode = content_cfg.horizontal_wrap;
  Element cached_content;

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
  // (indices align 1:1 with scroll offsets); matches is the row list for the
  // last compiled query; `search_pos` the index of the current match (-1
  // before the first jump); `search_invalid` flags a query that failed to
  // compile; `search_pending` flags rows not yet extracted. An empty query
  // is "search inactive" and matches nothing.
  //
  // Row extraction (one full offscreen layout) runs on ONE short-lived
  // worker thread so it never blocks input. The worker owns its private
  // tree, rebuilt from the immutable `contents`; the loop shares only
  // immutable inputs plus the handoff below. The loop side uses try_lock
  // exclusively and at most one worker runs at a time; stale generations
  // discard on landing.
  std::mutex search_mu;
  struct SearchRows {
    std::vector<std::string> rows;
    int viewport = -1;
    bool scroll = false;
  };
  SearchRows search_bg;  // worker handoff, guarded by search_mu.
  bool search_rows_ready = false;  // guarded by search_mu.
  std::atomic<uint64_t> search_gen{0};  // generation of the flight/result.
  std::atomic<bool> search_worker_running{false};
  std::thread search_worker;
  uint64_t search_adopted_gen = 0;  // generation currently in search_rows.
  std::vector<std::string> search_rows;
  int search_w_viewport = -1;  // inputs the rows were extracted for.
  bool search_w_scroll = false;
  bool search_rows_valid = false;
  std::string search_compiled;
  bool search_case = false;
  bool search_invalid = false;
  bool search_pending = false;
  std::vector<int> search_matches;
  int search_pos = -1;

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
      Element fresh = markit::RenderMarkdown(contents, content_cfg);
      cached_content =
          is_empty_placeholder ? fresh | dim : std::move(fresh);
    }
    // Refresh here (not just in the status bar): the content renders before
    // the status row in the same frame, so the mark must already be current.
    // refresh_search_state is idempotent — unchanged state recomputes
    // nothing (span lookup covers one row only).
    refresh_search_state();
    if (hl_match_row >= 0 && !hl_match_spans.empty()) {
      return markit::SearchHighlight(cached_content, &hl_match_row,
                                     &hl_match_spans);
    }
    return cached_content;
  });

  // Refresh the heading-row map when the tree, its render width, or the
  // mode changed. Shared by the highlight renderer and the Enter jump so
  // both agree on section boundaries.
  auto refresh_heading_map = [&] {
    if (cached_content && viewport_width >= 1 && viewport_height >= 1 &&
        (!hl_tree || hl_tree.get() != cached_content.get() ||
         hl_width != viewport_width || hl_scroll != hscroll)) {
      hl_map = markit::LocateHeadingRows(cached_content, headings,
                                         viewport_width, viewport_height,
                                         hscroll);
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
               &wrap_hint_w, &wrap_hint_h);

  auto clamp_selected = [&] {
    const int max_offset = std::max(0, content_height - viewport_height);
    selected = std::clamp(selected, 0, max_offset);
  };

  // Jump the main view to heading `idx`: its hl_map boundary row (nearest
  // located section at or before it when the heading itself has no row,
  // e.g. an empty fingerprint), clamped like any scroll.
  auto jump_to_heading = [&](int idx) {
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

  // Spawn the extraction worker for the current tree/width, unless one is
  // already running (its result is adopted or discarded by generation when
  // it lands). Fire-and-forget: rows don't depend on the query, so typing
  // never spawns workers — only tree/width changes do. Callers ensure the
  // viewport is usable.
  auto spawn_search_worker = [&] {
    const uint64_t gen = search_gen.load() + 1;
    search_gen.store(gen);
    const markit::Config cfg = content_cfg;
    const std::string& src = contents;
    const int width =
        markit::SearchExtractWidth(cached_content, viewport_width, hscroll);
    const int vw = viewport_width;
    const bool sc = hscroll;
    const int hint = std::max(1, content_height);
    if (search_worker.joinable()) {
      search_worker.join();  // finished flight only (never a live one).
    }
    search_worker_running.store(true);
    search_worker = std::thread([&, gen, cfg, vw, sc, width, hint] {
      // Private tree: RenderMarkdown is a pure function of its inputs and
      // FTXUI renders touch no shared mutable state, so this is race-free
      // by construction. (The empty-file dim decorator changes style only,
      // never text, so it is skipped here.)
      ftxui::Element tree = markit::RenderMarkdown(src, cfg);
      std::vector<std::string> rows =
          markit::RenderTextRows(tree, width, hint);
      {
        std::lock_guard<std::mutex> lock(search_mu);
        if (gen == search_gen.load()) {
          search_bg = SearchRows{std::move(rows), vw, sc};
          search_rows_ready = true;
          // Wake the loop: FTXUI renders on demand, so without this the
          // adoption (and the "..." -> count flip) would wait for the next
          // keypress. PostEvent is thread-safe; Custom matches no binding
          // and falls through harmlessly. Stale generations stay silent.
          screen.PostEvent(Event::Custom);
        }
      }
      search_worker_running.store(false);
    });
  };

  // Ensure a rows extraction is coming for the current tree/width: spawn a
  // worker unless the rows are already valid or one is already running.
  auto ensure_search_rows = [&] {
    if (!cached_content || viewport_width < 1 || viewport_height < 1) {
      return;
    }
    if ((!search_rows_valid || search_w_viewport != viewport_width ||
         search_w_scroll != hscroll) &&
        !search_worker_running.load()) {
      spawn_search_worker();
    }
  };

  // Recompute the search match state plus the live-view mark. Rows arrive
  // from the worker (adopted without ever blocking); matches are a regex
  // re-scan of the rows on the loop (microseconds), so typing never blocks.
  // A changed query drops the match cursor; a changed tree only clamps it.
  // Matches outlive the prompt: closing it hides the UI but keeps the query
  // so n/N keep navigating.
  //
  // Split from the worker spawn (which needs `screen`, declared later):
  // assigned to the forward hook above so the content renderer can refresh
  // first. This stays spawn-free; refresh_search below adds the spawn side.
  refresh_search_state = [&] {
    if (search_query.empty()) {
      // Inactive: drop matches but keep extracted rows and any flight —
      // rows don't depend on the query, so reopening is instant.
      search_matches.clear();
      search_pos = -1;
      search_invalid = false;
      search_pending = false;
      search_compiled.clear();
      hl_match_row = -1;
      hl_match_spans.clear();
      hl_match_query.clear();
      return;
    }
    if (!cached_content || viewport_width < 1 || viewport_height < 1) {
      return;
    }
    // Adopt worker rows without ever blocking the loop; a missed adoption
    // retries next frame.
    bool rows_rebuilt = false;
    {
      std::unique_lock<std::mutex> lock(search_mu, std::try_to_lock);
      if (lock.owns_lock() && search_rows_ready &&
          search_adopted_gen != search_gen.load()) {
        search_rows = std::move(search_bg.rows);
        search_w_viewport = search_bg.viewport;
        search_w_scroll = search_bg.scroll;
        search_rows_ready = false;
        search_adopted_gen = search_gen.load();
        search_rows_valid = true;
        rows_rebuilt = true;
      }
    }
    // Rows stale for the current tree/width? The old matches belong to
    // another layout, so drop them and show pending; refresh_search (below)
    // ensures a worker, since spawning needs `screen`.
    if (!search_rows_valid || search_w_viewport != viewport_width ||
        search_w_scroll != hscroll) {
      search_rows_valid = false;
      search_matches.clear();
    }
    // The matcher compiles on the loop (microseconds): an invalid query
    // reports even while rows are still pending.
    markit::Re2Matcher matcher(search_query, config.search_case_sensitive);
    if (!matcher.ok()) {
      search_invalid = true;
      search_pending = false;
      search_matches.clear();
      search_pos = -1;
      search_compiled = search_query;
      search_case = config.search_case_sensitive;
      hl_match_row = -1;
      hl_match_spans.clear();
      hl_match_query.clear();
      return;
    }
    search_invalid = false;
    if (!search_rows_valid) {
      search_pending = true;
      hl_match_row = -1;
      hl_match_spans.clear();
      hl_match_query.clear();
      return;
    }
    search_pending = false;
    const bool query_changed = (search_compiled != search_query ||
                                search_case != config.search_case_sensitive);
    if (rows_rebuilt || query_changed) {
      search_matches = markit::FindMatches(search_rows, matcher);
      search_compiled = search_query;
      search_case = config.search_case_sensitive;
      if (query_changed) {
        search_pos = -1;
      } else {
        search_pos = std::clamp(search_pos, -1,
                                static_cast<int>(search_matches.size()) - 1);
      }
    }
    // The live-view mark follows the current match: the jumped-to row once
    // search_pos sits on a match, else the row Enter/n would land on first
    // (strict NextMatch from the top of view, mirroring goto_match).
    // Spans cover one row only, so typing costs a single-row scan.
    const int count = static_cast<int>(search_matches.size());
    int target = -1;
    if (count > 0) {
      target = (search_pos >= 0 && search_pos < count)
                   ? search_matches[search_pos]
                   : markit::NextMatch(search_matches, selected - 1, +1);
    }
    if (target < 0 || target >= static_cast<int>(search_rows.size())) {
      hl_match_row = -1;
      hl_match_spans.clear();
      hl_match_query.clear();
    } else if (target != hl_match_row || hl_match_query != search_query ||
               hl_match_case != config.search_case_sensitive ||
               rows_rebuilt) {
      hl_match_spans = matcher.FindSpans(search_rows[target]);
      hl_match_row = target;
      hl_match_query = search_query;
      hl_match_case = config.search_case_sensitive;
    }
  };

  // Full search refresh for event/status paths: match state plus worker
  // spawn for stale rows. ensure_search_rows no-ops unless rows are
  // missing/stale and no worker is flying.
  auto refresh_search = [&] {
    refresh_search_state();
    if (!search_query.empty()) {
      ensure_search_rows();
    }
  };

  // Jump to the next (dir > 0) or previous (dir < 0) match, wrapping around.
  // The first jump after a query change lands on the first match at or below
  // (above, for dir < 0) the top of view: NextMatch is strict, so the seed
  // is offset by one row to make the first jump inclusive.
  auto goto_match = [&](int dir) {
    refresh_search();
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

  auto status_bar = Renderer([&] {
    const int max_offset = std::max(0, content_height - viewport_height);
    const int current = std::clamp(selected, 0, max_offset);
    std::string search_suffix;
    if (!search_query.empty()) {
      // Persistent match counter: visible while typing and while navigating
      // with n/N after the prompt closed. `/` clears the query (fresh
      // search), which hides the counter again. "..." while the worker rows
      // are still on their way.
      refresh_search();
      if (search_pending) {
        search_suffix = "...";
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

  auto component = CatchEvent(root, [&](Event event) -> bool {
    if (debug.is_open()) {
      debug << "input: " << event.DebugString()
            << " (character: '" << event.character() << "')\n";
      debug.flush();
    }

    if (search_open && event == Event::Escape) {
      // Esc closes the prompt first (before the global quit below). The
      // query and matches are retained so n/N keep navigating; focus goes
      // back to the content. `/` starts fresh.
      search_open = false;
      scroller->TakeFocus();
      return true;
    }
    if (event == Event::q || event == Event::Escape || event == Event::CtrlC) {
      screen.Exit();
      return true;
    }
    if (event == Event::Tab) {
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
      if (event == Event::ArrowUp || event == Event::Character('k')) {
        nav_cursor = std::max(0, nav_cursor - 1);
        follow();
        return true;
      }
      if (event == Event::ArrowDown || event == Event::Character('j')) {
        nav_cursor = std::min(count - 1, nav_cursor + 1);
        follow();
        return true;
      }
      if (event == Event::Home) {
        nav_cursor = 0;
        follow();
        return true;
      }
      if (event == Event::End) {
        nav_cursor = count - 1;
        follow();
        return true;
      }
      if (event == Event::PageUp) {
        nav_cursor = std::max(0, nav_cursor - visible);
        follow();
        return true;
      }
      if (event == Event::PageDown) {
        nav_cursor = std::min(count - 1, nav_cursor + visible);
        follow();
        return true;
      }
      if (event == Event::Return) {
        jump_to_heading(nav_cursor);
        return true;
      }
    }
    if (search_open) {
      // While the prompt is open, Enter accepts the query: jump to the next
      // match and close the prompt (n/N keep navigating from there).
      // Esc cancels without jumping. Every other key (including n/N and /)
      // falls through to the Input as text.
      if (event == Event::Return) {
        goto_match(+1);
        search_open = false;
        scroller->TakeFocus();
        return true;
      }
      return false;
    }
    if (event == Event::Character('n')) {
      goto_match(+1);  // no-op without an active search.
      return true;
    }
    if (event == Event::Character('N')) {
      goto_match(-1);  // no-op without an active search.
      return true;
    }
    if (event == Event::Character('/')) {
      search_open = true;
      search_query.clear();
      search_matches.clear();
      search_pos = -1;
      search_input->TakeFocus();
      // Pre-warm the rows for the current tree on the worker: the first
      // keystroke then scans instead of extracting.
      ensure_search_rows();
      return true;
    }
    if (event == Event::Character('w')) {
      const bool old_is_scroll = hscroll;
      Element old_tree = cached_content;
      hscroll = !hscroll;
      content_cfg.horizontal_wrap =
          hscroll ? markit::WrapMode::Scroll : markit::WrapMode::Wrap;
      selected_x = 0;  // re-anchor horizontally on mode switch.
      // Rebuild the content tree for the new mode eagerly so the vertical
      // offset can be mapped from the old tree (row numbers differ per
      // mode); the content renderer below reuses this tree as-is.
      Element fresh = markit::RenderMarkdown(contents, content_cfg);
      fresh = is_empty_placeholder ? fresh | dim : std::move(fresh);
      cached_content = fresh;
      rendered_mode = content_cfg.horizontal_wrap;
      int new_height = 0;
      selected = markit::MapTogglePosition(old_tree, fresh, headings, selected,
                                           viewport_width, viewport_height,
                                           old_is_scroll, &new_height);
      if (!hscroll) {
        // Toggle target is wrap: hand the anchor's new-tree row count to the
        // scroller so it adopts the height instead of re-measuring.
        wrap_hint_w = viewport_width;
        wrap_hint_h = new_height;
      } else {
        wrap_hint_w = -1;
      }
      log("mode", hscroll ? 0 : 1, hscroll ? 1 : 0);
      return true;
    }
    if (event == Event::CtrlN) {
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

  screen.Loop(component);
  if (search_worker.joinable()) {
    search_worker.join();  // reap the extraction worker, if still flying.
  }
  return EXIT_SUCCESS;
}
