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
