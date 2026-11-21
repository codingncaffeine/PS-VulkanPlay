#include <cmath>
#include <string>
#include <vector>

#include "app/stream_menu.h"
#include "check.h"
#include "overlay/text.h"

using namespace psvp::app;

PSVP_TEST(the_menu_opens_once_for_the_three_buttons_held_together) {
    StreamMenu menu;
    // Two of the three: nothing. The third: it opens, once.
    CHECK(!menu.chord(true, true, false));
    CHECK(menu.chord(true, true, true));
    menu.open(false, false);
    CHECK(menu.is_open());
    // Held on, or pressed again while it is open: not again.
    CHECK(!menu.chord(true, true, true));
    CHECK(!menu.chord(true, false, true));
    CHECK(!menu.chord(true, true, true));
    // Closed, let go, held again: it opens again.
    CHECK(menu.press(Press::back) == MenuAction::resume);
    CHECK(!menu.is_open());
    CHECK(!menu.chord(false, false, false));
    CHECK(menu.chord(true, true, true));
    // Options first, then the shoulders: the order does not matter.
    StreamMenu other;
    CHECK(!other.chord(false, false, true));
    CHECK(!other.chord(true, false, true));
    CHECK(other.chord(true, true, true));
}

PSVP_TEST(the_menu_chooses_round_its_ends_and_does_what_is_chosen) {
    StreamMenu menu;
    CHECK(menu.press(Press::accept) == MenuAction::none);  // closed: nothing
    menu.open(false, false);
    auto view = menu.view();
    CHECK(view && view->items.size() == StreamMenu::kItems && view->selected == 0 && view->items[0] == "Back to the game");
    CHECK(view && view->items[1] == "Statistics: hidden" && view->items[2] == "Full screen: off");
    CHECK(menu.press(Press::up) == MenuAction::none);
    CHECK(menu.view()->selected == StreamMenu::kItems - 1);
    CHECK(menu.press(Press::down) == MenuAction::none);
    CHECK(menu.view()->selected == 0);
    // A toggle keeps the menu open, and its line follows what it toggled.
    CHECK(menu.press(Press::down) == MenuAction::none);
    CHECK(menu.press(Press::accept) == MenuAction::toggle_stats);
    CHECK(menu.is_open());
    menu.set_state(true, true);
    CHECK(menu.view()->items[1] == "Statistics: shown" && menu.view()->items[2] == "Full screen: on");
    CHECK(menu.press(Press::down) == MenuAction::none);
    CHECK(menu.press(Press::accept) == MenuAction::toggle_fullscreen);
    CHECK(menu.is_open());
    // Left and right, the pages: nothing.
    CHECK(menu.press(Press::left) == MenuAction::none && menu.press(Press::next_page) == MenuAction::none);
    // Quit closes it.
    CHECK(menu.press(Press::down) == MenuAction::none);
    CHECK(menu.view()->items[3] == "Quit (the console stays on)");
    CHECK(menu.press(Press::accept) == MenuAction::quit);
    CHECK(!menu.is_open() && !menu.view());
    // Opened again: back on the first item; the last one quits and puts the console to rest.
    menu.open(false, false);
    CHECK(menu.view()->selected == 0);
    CHECK(menu.press(Press::up) == MenuAction::none);
    CHECK(menu.press(Press::accept) == MenuAction::rest_and_quit);
    CHECK(!menu.is_open());
}

PSVP_TEST(overlay_menu_panel_sits_in_the_middle_with_the_chosen_item_on_its_bar) {
    using namespace psvp::overlay;
    const std::vector<std::string> items{"Back to the game", "Statistics: hidden", "Quit"};
    const std::vector<Quad> quads = menu_panel("PS VulkanPlay", items, 1, "Cross: choose", 1280, 720);
    CHECK(quads.size() > 20);
    if (quads.size() < 3) return;
    const Quad& panel = quads.front();
    CHECK(!panel.glyph && panel.color == kMenuPanelColor);
    // In the middle of the window, inside it.
    CHECK(std::abs((panel.x0 + panel.x1) / 2 - 640.0F) < 1.5F && std::abs((panel.y0 + panel.y1) / 2 - 360.0F) < 1.5F);
    CHECK(panel.x0 > 0 && panel.x1 < 1280 && panel.y0 > 0 && panel.y1 < 720);
    // One bar, of the accent color, inside the panel; the text on it is dark, the other items light.
    std::size_t bars = 0;
    const Quad* bar = nullptr;
    for (const Quad& quad : quads) {
        if (!quad.glyph && quad.color == kMenuBarColor) {
            ++bars;
            bar = &quad;
        }
    }
    CHECK(bars == 1);
    if (bar == nullptr) return;
    CHECK(bar->x0 > panel.x0 && bar->x1 < panel.x1 && bar->y0 > panel.y0 && bar->y1 < panel.y1);
    std::size_t dark_on_bar = 0, light_off_bar = 0;
    for (const Quad& quad : quads) {
        if (!quad.glyph) continue;
        const bool on_bar = quad.y0 >= bar->y0 && quad.y1 <= bar->y1;
        if (on_bar && quad.color == kMenuChosenTextColor) ++dark_on_bar;
        if (!on_bar && quad.color == kStatsTextColor) ++light_off_bar;
    }
    CHECK(dark_on_bar == std::string("Statistics: hidden").size() - 1);  // the space draws nothing
    CHECK(light_off_bar > 10);
    // Nothing to show: no quads.
    CHECK(menu_panel("x", {}, 0, "", 1280, 720).empty());
}
