# CHANGELOG

# Change log
Changes are recorded with the following format:
Version tag (add only when current branch is version sync with master):
<semver version, 0.0.1 if first)> - <date/timestamp>
Change tag:
<date/timestamp> - <op:add|mod|rem> - <change text>

0.0.1 - 2026-09-03T22:57:51Z
2026-09-03T22:57:51Z - add - Initial commit (project scaffolding)
2026-09-03T22:57:51Z - add - Initial viewer for milestone 1 (FTXUI-based terminal markdown viewer)
2026-09-03T22:57:51Z - mod - Fix scroll rendering mapping and add debug input logging
2026-09-03T22:57:51Z - add - FTXUI README (827-line test content)
2026-09-03T22:57:51Z - add - Top-anchored Scroller pager with gtest functional tests
2026-09-03T22:57:51Z - mod - Remove heading blank line

0.0.2 - 2026-09-04T02:30:14Z
2026-09-04T02:30:14Z - mod - Fix first-frame event race and add terminal resize support (viewport_height is Ref<int>)
2026-09-04T02:30:14Z - add - Milestone 2: markdown parsing & rendering via md4c (headings, emphasis, code, quote, lists/task-lists, links, tables, HR)
2026-09-04T02:30:14Z - add - Markdown renderer functional tests (gtest)
2026-09-04T02:30:14Z - mod - Pin BUILD_SHARED_LIBS=OFF so markit links FTXUI/md4c statically (binary stayed ~1.7-2.2MB instead of shrinking to 280KB)
2026-09-04T02:30:14Z - mod - Fix frame-stack desync dropping list item text after preceding blocks; add table column gutter
