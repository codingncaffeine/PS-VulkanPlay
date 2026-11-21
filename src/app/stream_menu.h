// The menu over the stream: what it holds and what a press in it does. Reading the controller
// and drawing are not in here.
//
// It opens with F1, or with L1, R1 and Options held together on a controller (three buttons no
// game asks for at once). While it is open, nothing that is pressed goes to the console.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "app/controller_presses.h"

namespace psvp::app {

enum class MenuAction { none, resume, toggle_stats, toggle_fullscreen, quit, rest_and_quit };

// What is drawn: a title, the items, which one is chosen, and a line on how to use it.
struct MenuView {
    std::string title;
    std::vector<std::string> items;
    std::size_t selected = 0;
    std::string hint;
    friend bool operator==(const MenuView&, const MenuView&) = default;
};

class StreamMenu {
public:
    // The three buttons of the controller that open it, as they are now: true once, when the
    // last of them went down while the menu was closed.
    [[nodiscard]] bool chord(bool l1, bool r1, bool options);

    void open(bool stats_shown, bool fullscreen);
    void close() { open_ = false; }
    [[nodiscard]] bool is_open() const noexcept { return open_; }
    // What the toggles say: kept up to date while the menu is open.
    void set_state(bool stats_shown, bool fullscreen) {
        stats_shown_ = stats_shown;
        fullscreen_ = fullscreen;
    }

    // A press while it is open. Up and down choose (round the ends); accept does the chosen item;
    // back is "back to the game". The quits and "back to the game" close the menu, the toggles keep it.
    [[nodiscard]] MenuAction press(Press press);
    [[nodiscard]] std::optional<MenuView> view() const;

    static constexpr std::size_t kItems = 5;

private:
    [[nodiscard]] MenuAction action(std::size_t item) const;
    bool open_ = false;
    std::size_t selected_ = 0;
    bool stats_shown_ = false, fullscreen_ = false;
    bool chord_down_ = false;
};

}  // namespace psvp::app
