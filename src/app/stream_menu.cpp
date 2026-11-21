#include "app/stream_menu.h"

namespace psvp::app {

bool StreamMenu::chord(bool l1, bool r1, bool options) {
    const bool down = l1 && r1 && options;
    const bool began = down && !chord_down_;
    chord_down_ = down;
    return began && !open_;
}

void StreamMenu::open(bool stats_shown, bool fullscreen) {
    open_ = true;
    selected_ = 0;
    set_state(stats_shown, fullscreen);
}

MenuAction StreamMenu::action(std::size_t item) const {
    switch (item) {
    case 0: return MenuAction::resume;
    case 1: return MenuAction::toggle_stats;
    case 2: return MenuAction::toggle_fullscreen;
    case 3: return MenuAction::quit;
    case 4: return MenuAction::rest_and_quit;
    default: return MenuAction::none;
    }
}

MenuAction StreamMenu::press(Press press) {
    if (!open_) return MenuAction::none;
    switch (press) {
    case Press::up: selected_ = (selected_ + kItems - 1) % kItems; return MenuAction::none;
    case Press::down: selected_ = (selected_ + 1) % kItems; return MenuAction::none;
    case Press::back: open_ = false; return MenuAction::resume;
    case Press::accept: {
        const MenuAction chosen = action(selected_);
        if (chosen == MenuAction::resume || chosen == MenuAction::quit || chosen == MenuAction::rest_and_quit) open_ = false;
        return chosen;
    }
    default: return MenuAction::none;
    }
}

std::optional<MenuView> StreamMenu::view() const {
    if (!open_) return std::nullopt;
    MenuView view;
    view.title = "PS VulkanPlay";
    view.items = {"Back to the game", stats_shown_ ? "Statistics: shown" : "Statistics: hidden", fullscreen_ ? "Full screen: on" : "Full screen: off",
                  "Quit (the console stays on)", "Quit and put the console to rest"};
    view.selected = selected_;
    view.hint = "Cross or Enter: choose      Circle or Esc: back to the game";
    return view;
}

}  // namespace psvp::app
