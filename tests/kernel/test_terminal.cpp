// Terminal scrollback: the history ring, scrolling limits, a scrolled-back
// view staying on its text while output continues, and resizing keeping
// the text. Logic only; nothing is painted.
#include <gui/terminal.h>
#include <kernel/ktest.h>
#include <lib/kprintf.h>
#include <mm/kheap.h>

namespace {

void lines(Terminal& t, int n) {
    for (int i = 0; i < n; i++) {
        t.putc('x');
        t.putc('\n');
    }
}

} // namespace

int ktest_terminal(int, char**) {
    constexpr int COLS = 80, HISTORY = 100;
    u8* cells = (u8*)kmalloc((usize)COLS * HISTORY);
    KTEST_CHECK(cells != nullptr);
    gfx::Font font;
    font.width = 8;
    font.height = 16;
    Terminal t;
    t.init(cells, COLS, HISTORY, font);
    t.resize(COLS, 24);

    // A fresh screen has nothing to scroll to.
    KTEST_CHECK(t.scrollable() == 0);
    t.scroll(10);
    KTEST_CHECK(t.scroll_offset() == 0);

    // 30 lines on a 24-row screen: the cursor reached row 23 after 23 of
    // them, and the other 7 pushed 7 lines into the history.
    lines(t, 30);
    KTEST_CHECK(t.scrollable() == 7);
    t.scroll(1000);
    KTEST_CHECK(t.scroll_offset() == 7 && t.scrolled_back());
    t.scroll(-3);
    KTEST_CHECK(t.scroll_offset() == 4);

    // Output while scrolled back keeps the view on the same text.
    lines(t, 2);
    KTEST_CHECK(t.scroll_offset() == 6 && t.scrollable() == 9);
    t.scroll_to_bottom();
    KTEST_CHECK(t.scroll_offset() == 0 && !t.scrolled_back());
    kprintf("  history: 32 lines on a 24-row screen leave 9 to scroll back to; a scrolled view holds still\n");

    // The ring keeps the last HISTORY lines: everything above the screen.
    lines(t, 500);
    KTEST_CHECK(t.scrollable() == HISTORY - 24);
    t.scroll(1 << 20);
    KTEST_CHECK(t.scroll_offset() == HISTORY - 24);
    t.scroll_to_bottom();

    // A shorter window shows less but loses nothing; a taller one shows more.
    t.resize(COLS, 10);
    KTEST_CHECK(t.rows() == 10 && t.scrollable() == HISTORY - 10);
    t.resize(COLS, 24);
    KTEST_CHECK(t.rows() == 24 && t.scrollable() == HISTORY - 24);
    kprintf("  ring: after 532 lines the last %d stay reachable; resizing keeps them\n", HISTORY);

    kfree(cells);
    return 0;
}
