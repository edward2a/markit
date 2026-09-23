#ifndef MARKIT_KEYBINDINGS_HPP
#define MARKIT_KEYBINDINGS_HPP

#include <vector>

#include <ftxui/component/event.hpp>

namespace markit {

// Key bindings for every interactive action, one key list per action.
// Defaults preserve the historical hardcoded mappings (see DumpDefaultConfig).
// A binding list may be emptied to unbind the action; overlaps between actions
// that share an evaluation context are rejected at load, while modal overlaps
// across contexts are allowed by design and resolved by the event chain.
struct KeyBindings {
  // Main content view (handled by the scroller).
  std::vector<ftxui::Event> scroll_up;
  std::vector<ftxui::Event> scroll_down;
  std::vector<ftxui::Event> page_up;
  std::vector<ftxui::Event> page_down;
  std::vector<ftxui::Event> goto_top;
  std::vector<ftxui::Event> goto_bottom;
  std::vector<ftxui::Event> pan_left;   // scroll display mode only.
  std::vector<ftxui::Event> pan_right;  // scroll display mode only.
  // Search and toggles (handled in main.cpp).
  std::vector<ftxui::Event> search_open;
  std::vector<ftxui::Event> search_next;
  std::vector<ftxui::Event> search_prev;
  std::vector<ftxui::Event> search_accept;  // while the prompt is open.
  std::vector<ftxui::Event> search_cancel;  // while the prompt is open.
  std::vector<ftxui::Event> quit;
  std::vector<ftxui::Event> focus_switch;
  std::vector<ftxui::Event> toggle_wrap;
  std::vector<ftxui::Event> toggle_nav;
  // Outline navigation (handled in main.cpp while nav_focused).
  std::vector<ftxui::Event> nav_up;
  std::vector<ftxui::Event> nav_down;
  std::vector<ftxui::Event> nav_page_up;
  std::vector<ftxui::Event> nav_page_down;
  std::vector<ftxui::Event> nav_top;
  std::vector<ftxui::Event> nav_bottom;
  std::vector<ftxui::Event> nav_activate;

  KeyBindings();  // fills every list with its default keys.
};

bool operator==(const KeyBindings& a, const KeyBindings& b);
inline bool operator!=(const KeyBindings& a, const KeyBindings& b) {
  return !(a == b);
}

// True when `event` equals any key in `keys` (FTXUI Event equality compares
// the underlying input, so Character('q') matches the predefined Event::q).
bool MatchesKey(const ftxui::Event& event,
                const std::vector<ftxui::Event>& keys);

}  // namespace markit

#endif  // MARKIT_KEYBINDINGS_HPP
