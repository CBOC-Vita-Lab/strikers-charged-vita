#include "../vita/vgx_display_rect.h"
#include <cassert>
#include <cstdio>

int main() {
    const auto full = vgxvita::GetDisplayRect(true);
    assert(full.x == 0 && full.y == 0);
    assert(full.width == 960 && full.height == 544);
    const auto boxed = vgxvita::GetDisplayRect(false);
    assert(boxed.x == 117 && boxed.y == 0);
    assert(boxed.width == 725 && boxed.height == 544);
    assert(boxed.x + boxed.width == 842);
    std::puts("display viewport: full screen and exact integer 4:3 blit bounds pass");
}
