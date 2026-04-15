/*
    mod_serverBrowser.c
    Overlay dialog for selecting the active server (F4).

    Shows a centred window listing all entries from serverList[].
    The currently active server is marked with " *" on the right.
    Pressing ENTER applies the selection and returns true so the caller
    can reload the file list via selectPanel().
    Pressing ESC discards the selection and returns false.
*/

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "msx_const.h"
#include "dos.h"
#include "conio.h"
#include "utils.h"
#include "fh.h"
#include "mod_serverBrowser.h"


// ---- Window geometry (centred, 60 cols wide, 12 rows tall) ----
#define SB_WIN_X1   11
#define SB_WIN_X2   70
#define SB_WIN_Y1   7
#define SB_WIN_Y2   18
#define SB_WIN_W    (SB_WIN_X2 - SB_WIN_X1 + 1)   // 60
#define SB_LIST_X   13
#define SB_LIST_Y   9                               // first entry row
#define SB_ENTRY_W  (SB_WIN_X2 - SB_LIST_X - 1)   // 56


// ========================================================
// External helpers defined in fhMOD.c but not yet in fh.h headers
extern void clearBlinkList(void);
extern void printTabs(void);


// ========================================================
static void sb_drawWindow(void)
{
    uint8_t y;

    // Clear the window columns on every window row
    for (y = SB_WIN_Y1; y <= SB_WIN_Y2; y++) {
        _fillVRAM((uint16_t)((y - 1) * 80 + SB_WIN_X1 - 1), SB_WIN_W, ' ');
    }

    // Apply blink to the whole window area
    fillBlink(SB_WIN_X1, SB_WIN_Y1,
              (uint8_t)(SB_WIN_Y2 - SB_WIN_Y1 + 1), SB_WIN_W, true);

    // Draw the frame
    drawFrame(SB_WIN_X1, SB_WIN_Y1, SB_WIN_X2, SB_WIN_Y2);

    // Title and key-hints embedded in the frame border
    putstrxy(SB_WIN_X1 + 2, SB_WIN_Y1, " Select Server ");
    putstrxy(SB_WIN_X1 + 1, SB_WIN_Y2,
             " UP/DOWN:Navigate  ENTER:Select  ESC:Cancel ");
}


// ========================================================
static void sb_printEntry(uint8_t idx, bool selected)
{
    uint8_t nameLen;
    uint8_t y = SB_LIST_Y + idx;

    memset(buff, ' ', SB_ENTRY_W);
    buff[SB_ENTRY_W] = '\0';

    // Copy name, leaving room for the " *" active-server marker (2 chars)
    nameLen = strlen(serverList[idx].name);
    if (nameLen > SB_ENTRY_W - 3) nameLen = SB_ENTRY_W - 3;
    memcpy(buff, serverList[idx].name, nameLen);

    // Mark the currently active server with " *" at the right edge
    if (idx == currentServer) {
        buff[SB_ENTRY_W - 2] = ' ';
        buff[SB_ENTRY_W - 1] = '*';
    }

    putlinexy(SB_LIST_X, y, SB_ENTRY_W, buff);
    textblink(SB_LIST_X, y, SB_ENTRY_W, selected);
}


// ========================================================
static void sb_printList(uint8_t curLine)
{
    uint8_t i;

    // Print all populated entries
    for (i = 0; i < serverCount; i++) {
        sb_printEntry(i, (i == curLine));
    }

    // Clear any remaining rows so old data is not visible
    for (; i < MAX_SERVERS; i++) {
        uint8_t y = SB_LIST_Y + i;
        _fillVRAM((uint16_t)((y - 1) * 80 + SB_LIST_X - 1), SB_ENTRY_W, ' ');
        textblink(SB_LIST_X, y, SB_ENTRY_W, false);
    }
}


// ========================================================
// Entry point: show the overlay and handle input.
// Returns true  → server changed  (caller calls selectPanel to reload)
// Returns false → no change       (caller redraws in place)
bool showServerBrowser(void)
{
    uint8_t curLine = currentServer;   // pre-select the active server
    bool    done    = false;
    bool    changed = false;
    char    key;

    // Flush any stale keypresses
    while (kbhit()) getch();

    setSelectedLine(false);
    sb_drawWindow();
    sb_printList(curLine);

    while (!done) {
        ASM_EI; ASM_HALT;
        if (!kbhit()) continue;

        key = dos2_toupper(getch());

        if (key == KEY_UP) {
            if (curLine > 0) {
                sb_printEntry(curLine, false);
                curLine--;
                sb_printEntry(curLine, true);
            }

        } else if (key == KEY_DOWN) {
            if ((uint8_t)(curLine + 1) < serverCount) {
                sb_printEntry(curLine, false);
                curLine++;
                sb_printEntry(curLine, true);
            }

        } else if (key == KEY_RETURN || key == KEY_SELECT) {
            if (curLine != currentServer) {
                currentServer    = curLine;
                BASEURL          = serverList[curLine].url;
                serverHasDirMode = serverList[curLine].dirMode;
                changed = true;
            }
            done = true;

        } else if (key == KEY_ESC) {
            // Wait for the physical key release so the main loop
            // does not immediately consume the ESC
            while (varNEWKEY_row7.esc == 0) { ASM_EI; ASM_HALT; }
            while (kbhit()) getch();
            done = true;
        }
    }

    // Remove blink from the window area
    fillBlink(SB_WIN_X1, SB_WIN_Y1,
              (uint8_t)(SB_WIN_Y2 - SB_WIN_Y1 + 1), SB_WIN_W, false);

    if (!changed) {
        // No server change — just repaint the underlying list
        clearBlinkList();
        printTabs();
        printRequestData();
        printList();
        setSelectedLine(true);
    }
    // If changed, the caller (fhMOD.c) calls selectPanel() which
    // resets dirMode, repaints everything and downloads the new list.

    return changed;
}
