# CHANGELOG

## Format

Changes are recorded with the following format:  
Version tag (add only when current branch is version sync with master):  
`## <semver version, 0.0.1 if first)> - <date/timestamp>`  
Change tag:  
`- <date/timestamp> - <op:add|mod|rem> - <change text>`  

---

## 0.0.1 - 2026-09-03T22:57:51Z

- 2026-09-03T22:57:51Z - add - Initial commit (project scaffolding)
- 2026-09-03T22:57:51Z - add - Initial viewer for milestone 1 (FTXUI-based terminal markdown viewer)
- 2026-09-03T22:57:51Z - mod - Fix scroll rendering mapping and add debug input logging
- 2026-09-03T22:57:51Z - add - FTXUI README (827-line test content)
- 2026-09-03T22:57:51Z - add - Top-anchored Scroller pager with gtest functional tests
- 2026-09-03T22:57:51Z - mod - Remove heading blank line

## 0.0.2 - 2026-09-04T02:30:14Z

- 2026-09-04T02:30:14Z - mod - Fix first-frame event race and add terminal resize support (viewport\_height is Ref\<int>)
- 2026-09-04T02:30:14Z - add - Milestone 2: markdown parsing & rendering via md4c (headings, emphasis, code, quote, lists/task-lists, links, tables, HR)
- 2026-09-04T02:30:14Z - add - Markdown renderer functional tests (gtest)
- 2026-09-04T02:30:14Z - mod - Pin BUILD\_SHARED\_LIBS=OFF so markit links FTXUI/md4c statically (binary stayed ~1.7-2.2MB instead of shrinking to 280KB)
- 2026-09-04T02:30:14Z - mod - Fix frame-stack desync dropping list item text after preceding blocks; add table column gutter
- 2026-09-04T03:35:53Z - mod - Review fixes: null-safe Attr, decorator composition via composable operator|, permissive autolinks, per-table header (thead) row tracking, per-cell alignment, empty-cell table width floor

## 0.0.3 - 2026-09-04T05:58:08Z

- 2026-09-04T05:58:08Z - add - Milestone 3: YAML config file for color scheme (Theme struct, named/hex colors, embedded schema validation, --config flag, default at ~/.config/markit/markit.yml)
- 2026-09-04T05:58:08Z - add - Config parsing/schema-validation tests (config\_test.cpp)
- 2026-09-04T05:58:08Z - mod - Thread Theme through RenderMarkdown; replace hardcoded colors with theme values
- 2026-09-04T05:58:08Z - mod - Add yaml-cpp via FetchContent (0.8.0) with CMAKE\_POLICY\_VERSION\_MINIMUM for CMake 4
- 2026-09-04T11:30:12Z - add - Milestone 3.1: --dump-config flag writes default theme YAML (with color-format comments) to stdout
- 2026-09-04T14:12:00Z - add - Milestone 3.2: -h/--help flag prints usage (stdout, exit 0); usage helper shared with error path (stderr, exit 1)
- 2026-09-06T16:56:04Z - mod - Fix list markers: unordered/ordered marker fields were misaligned in Frame aggregate initialization (bullets showed as numbers); set fields explicitly and add marker assertions to list tests
- 2026-09-06T17:41:24Z - mod - Fix hard-break rendering: trailing-space hard breaks now split paragraphs into one row per source line instead of a "\n" text node that inflated the row height and made inline-code background colors bleed into the line below
- 2026-09-06T17:53:41Z - mod - Fix inline-code background extending full screen width: FlattenInline now always returns an hbox (even for a single fragment), so a lone styled span is width-constrained instead of painting a bare vbox child full-row background

## 0.0.4 - 2026-09-06T18:23:47Z

- 2026-09-06T18:16:33Z - mod - Milestone 4 Phase 1: refactor config to load/dump a full Config container (Config wraps Theme) instead of the Theme alone; renderer still receives cfg.theme, CLI behavior unchanged
- 2026-09-06T18:52:30Z - add - Milestone 4 Phase 2: display modes — wrap (default) reflows paragraphs/headings/lists and code/HTML lines to the viewport width, edge to edge, via hflow word-splitting (styled spans preserved, table cells stay single-line); scroll keeps rows unwrapped and horizontally pannable; config display.horizontal wrap|scroll with schema validation
- 2026-09-06T18:52:30Z - add - Live 'w' toggle between wrap and scroll (re-renders content); horizontal panning ArrowLeft/ArrowRight + h/l active in scroll mode only, vertical navigation unchanged
- 2026-09-06T19:31:28Z - mod - Fix HTML block content dropped: md4c delivers raw HTML block/span markup via MD_TEXT_HTML, which was unhandled and silently discarded, so HTML blocks rendered as empty bordered boxes (FTXUI_README.md banner)
- 2026-09-06T19:31:28Z - mod - Fix code/HTML bordered boxes stretching full window width: the bordered vbox (a vbox assigns its children the full x-range) is wrapped in an hbox in scroll mode so the border hugs the widest line; in wrap mode the box spans the viewport width so reflow reaches it
- 2026-09-06T22:13:23Z - mod - Fix wrap reflow through the Scroller: a plain xframe laid the content out at its natural width and clipped it, silently defeating all wrapping in the app; wrap mode now uses only a vertical frame with no scroll indicator, so content reflows width-constrained edge to edge with the rightmost column usable, and vertical scroll bookkeeping measures the actual wrapped height once per viewport width
- 2026-09-06T22:40:46Z - mod - Fix wrap-mode leading-whitespace styling: inter-word spaces before a styled token (link, emphasis, inline code) are glued as a plain piece ahead of the word, so the underline/color no longer extends onto the space; regression test WrapLinkLeadingSpaceIsNotUnderlined added, and the test text-stripper now also consumes OSC hyperlink escapes
- 2026-09-06T23:54:28Z - mod - Fix inline raw HTML in paragraphs silently dropped: without a verbatim Code/Html frame it is now emitted verbatim with inline-code styling; tables measure each cell's computed requirement before sizing columns
- 2026-09-06T23:54:28Z - mod - Harden config loading: an empty config file yields defaults, while a non-mapping root or non-scalar keys throw typed errors with dotted paths instead of untyped yaml-cpp exceptions
- 2026-09-06T23:54:28Z - mod - Fix wrap scroll-range collapse: the measured wrapped height is re-taken on display-mode switch and restored from cache on every render, instead of falling back to the unwrapped natural height (which silently disabled scrolling for wrapped content taller than the viewport); core sources factored into a markit_core static lib shared by the app and tests
- 2026-09-07T00:04:53Z - mod - Cache the rendered content tree (rebuild only on display-mode switch instead of re-parsing per frame) and skip redundant requirement computation for identical trees; refresh the viewport size every render so terminal resizes reflow immediately
- 2026-09-07T00:04:53Z - mod - Track viewport-width changes across renders with re-measurement (regression test WrapAdaptsToViewportWidthChange)
- 2026-09-07T00:15:43Z - mod - Fix wrap mode breaking styled runs at spaces: inter-word whitespace keeps the following word's style when the preceding piece shares the same span-stack identity, so multi-word links/emphasis/code render as one continuous run like scroll mode, while boundary spaces stay plain
