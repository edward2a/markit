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
- 2026-09-07T00:29:46Z - mod - Unify code/HTML box width: scroll mode no longer shrink-wraps the border to the widest line; the box spans the full content width like tables and like wrap mode, panning with the document
- 2026-09-07T01:34:31Z - mod - Render HTML instead of boxing it: a tag subset (inline b/i/u/s/code/a/img/br plus block p/div/h1-6/blockquote/lists/hr, and the same inline tags inside markdown paragraphs) maps onto markdown styling and frames, while pre/script/style, comments and unknown tags stay verbatim in a single coalesced box
- 2026-09-07T01:49:25Z - add - Render details/summary statically and always expanded: the summary gets a disclosure marker (open state) plus bold, content flows as normal blocks
- 2026-09-07T02:06:35Z - mod - Restore blank row after headings: a heading owes the next block one blank row at every level; a --- rule following a heading carries that gap past itself (no blank between heading and rule, single blank after), standalone rules unchanged

## 0.0.5 - 2026-09-07T11:42:18Z

- 2026-09-07T11:42:18Z - add - Milestone 5 static chrome: status bar (basename + scroll position + wrap/scroll mode, inverted), action bar (key hints, dim), and full-height right navigation bar (TOC from document headings, "Outline" title); nav toggles live with 'n', defaults to display.navigation visible|hidden, content viewport shrinks around the chrome
- 2026-09-07T13:49:40Z - mod - Collapse HTML inline whitespace to single spaces: newlines/indentation between anchors no longer reach text() elements where hflow broke the row and stranded styled spaces as a phantom underlined line below the links carrying the same URLs
- 2026-09-07T16:55:05Z - mod - Fix HTML link spacing/underline in both modes: merge whitespace split across md4c callbacks (kills double spaces) and keep cross-fragment gaps plain unless from the same styled run (adjacent same-URL badges no longer get underlined gaps in wrap mode)
- 2026-09-07T17:31:30Z - mod - Drop link-edge formatting whitespace in HTML anchors: newlines/indentation right after <a> and before </a> no longer render as link-styled padding around lone images (no more surrounding underlines in scroll mode)
- 2026-09-07T19:19:05Z - rem - Drop the scroll-mode vscroll_indicator gutter; the full viewport width is usable again, scroll position stays in the status bar
- 2026-09-08T02:50:30Z - add - Retain scroll position across the wrap/scroll toggle: the top-of-view row is fingerprinted (first words) inside its heading-delimited section and re-located in the new layout (heading anchors switch exactly, short rows match whole, scroll trees match at natural width); falls back to proportional on no match
- 2026-09-09T00:49:56Z - mod - Make the wrap/scroll toggle near-seamless again (~1s to ~50-110ms on FTXUI_README.md): render both anchor trees narrow in a single pass (width-scaled seed fixes a guaranteed wasted re-render; scroll trees are widened only when clipping makes the match ambiguous), hand the anchor's new-tree row count to the scroller so it skips re-measuring, and walk each tree's requirements once
- 2026-09-09T02:59:30Z - mod - Fix last content row unreachable at End (README Contributors [img] blank at 100% in scroll mode): the scroller's frame focus math assumed exclusive box bounds (-1) but FTXUI boxes are inclusive, so every position showed one row too early; mirror the frame's integer-halved (viewport-1)/2 offset with +0.5 on both axes, and pop unclosed inline-HTML span entries at markdown block leave instead of only resetting their count; regression tests ContributorsTailRendersImagePlaceholder, ContributorsTailVariantsRender, UnclosedInlineHtmlDoesNotLeakIntoNextParagraph, EndShowsLastLineAtBottomRow (118/118 green, pty tail visible at 100% in wrap and scroll)
- 2026-09-09T04:15:29Z - add - Nav bar highlights the section in view: the heading containing the top visible row renders bold in the accent color (preamble maps to none, duplicates resolve by rank); heading-row map is located with the toggle anchor's matching and cached per tree/width/mode so no per-frame layout; NavBar takes an optional current index (default -1 keeps old call sites)
