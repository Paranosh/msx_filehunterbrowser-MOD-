/*
	Copyright (c) 2025 Natalia Pujol Cremades
	info@abitwitches.com

	See LICENSE file.
*/
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "msx_const.h"
#include "dos.h"
#include "conio.h"
#include "heap.h"
#include "utils.h"
#include "fh.h"
#include "mod_localBrowser.h"


// ========================================================
#define LB_WIN_X1		1
#define LB_WIN_Y1		(PANEL_FIRSTY - 1)		// row 4 (1-based)
#define LB_WIN_X2		80
#define LB_WIN_Y2		(PANEL_LASTY + 1)		// row 23 (1-based)
#define LB_LIST_X		2
#define LB_LIST_Y		(LB_WIN_Y1 + 2)			// first list entry row
#define LB_LIST_ROWS	(LB_WIN_Y2 - LB_LIST_Y - 1)	// visible rows for entries

#define LB_MAX_ENTRIES	30				// max entries per directory scan
#define LB_NAME_MAXLEN	13				// FFBLK filename is 13 bytes (includes null)

#define ATTR_DIR		0x10			// MSX-DOS directory attribute bit

// BIOS keyboard circular buffer
// Characters injected here will be "read" by COMMAND.COM after fhMOD.com exits
// KEYBUF (0xFBF0) and PUTPNT (0xF3F8) are defined in msx_const.h
#define KEYBUF_START	((uint16_t)KEYBUF)			// keyboard buffer start (0xFBF0)
#define KEYBUF_END		((uint16_t)(KEYBUF + 40 - 1))	// keyboard buffer end  (0xFC17, inclusive)

// ========================================================
typedef struct {
	char    name[14];	// ASCIIZ filename (up to 12.3 chars + null)
	uint8_t isDir;		// 1 = directory, 0 = file
} LBEntry_t;

// ========================================================
// Safe local wrapper for dos2_findnext.
// The library's dos2_findnext uses a DOSJP tail-call: it sets IX = &ffblk
// and then jumps into BDOS. BDOS "preserves" IX — but that means IX stays
// pointing at &ffblk on return, NOT at lb_scanDir's SDCC frame pointer.
// Every local variable access via (IX+d) in lb_scanDir is then wrong,
// causing corrupted scan results or an infinite loop that freezes the MSX.
// Workaround: save/restore IX around the BDOS call here, mirroring the
// pattern already used by dos2_findfirst (which uses DOSCALL + push/pop ix).
static ERRB lb_findnext(FFBLK *ffblk) __naked __sdcccall(1)
{
	ffblk;
	__asm
		push ix			; save caller's IX frame pointer
		push hl
		pop  ix			; IX = Param ffblk
		ld   c,#FNEXT
		call 5			; DOSCALL — proper call; preserves stack
		pop  ix			; restore caller's IX frame pointer
		ld   l, a		; Returns L (error code)
		ret
	__endasm;
}

// ========================================================
extern void clearBlinkList();
extern void printTabs();
// restoreScreen() is declared in fh.h (already included)

// ========================================================
// Inject a command string into the BIOS keyboard buffer.
// When fhMOD.com exits afterwards, COMMAND.COM will read and execute it.
static void lb_injectCommand(const char *cmd)
{
	uint16_t putpnt;

	// Flush any pending keypresses so the buffer is empty
	while (kbhit()) getch();

	putpnt = varPUTPNT;

	while (*cmd) {
		*((char*)putpnt) = *cmd++;
		if (++putpnt > KEYBUF_END) putpnt = KEYBUF_START;
	}
	// Append CR so COMMAND.COM treats it as a complete line
	*((char*)putpnt) = '\r';
	if (++putpnt > KEYBUF_END) putpnt = KEYBUF_START;

	varPUTPNT = putpnt;
}

// ========================================================
// Restore screen, inject command, and exit fhMOD.com.
// COMMAND.COM will then run the command from the keyboard buffer.
static void lb_execCommand(const char *cmd)
{
	lb_injectCommand(cmd);
	restoreScreen();
	dos2_exit(0);
}

// ========================================================
// Draw the local browser overlay window frame
static void lb_drawWindow(void)
{
	uint8_t y;

	for (y = LB_WIN_Y1; y <= LB_WIN_Y2; y++) {
		_fillVRAM((uint16_t)((y - 1) * 80), 80, ' ');
	}

	fillBlink(LB_WIN_X1, LB_WIN_Y1, LB_WIN_Y2 - LB_WIN_Y1 + 1, 80, true);
	drawFrame(LB_WIN_X1, LB_WIN_Y1, LB_WIN_X2, LB_WIN_Y2);
	putstrxy(3, LB_WIN_Y2, " UP/DOWN:Navigate  ENTER:Open/Launch  ESC:Back ");
}

// ========================================================
// Print current path in the window title bar
static void lb_printTitle(void)
{
	char path[64];
	dos2_getCurrentDirectory(0, path);
	csprintf(buff, " Local: \\%s ", path);
	if (strlen(buff) > 74) buff[74] = '\0';
	putstrxy(3, LB_WIN_Y1, buff);
}

// ========================================================
// Print entry counter in window footer
static void lb_printCounter(uint8_t topLine, uint8_t curLine, uint8_t count)
{
	csprintf(buff, "%u/%u  ",
	         count ? (uint16_t)(topLine + curLine + 1) : (uint16_t)0,
	         (uint16_t)count);
	putstrxy(66, LB_WIN_Y2, buff);
}

// ========================================================
// Scan current directory; fill entries[]; return count
static uint8_t lb_scanDir(LBEntry_t *entries)
{
	FFBLK ffblk;
	uint8_t count = 0;
	char *dot;

	// Directories first (skip . and ..)
	if (dos2_findfirst("*.*", &ffblk, ATTR_DIR) == 0) {
		do {
			if (!(ffblk.attribs & ATTR_DIR)) continue;
			if (ffblk.filename[0] == '.') continue;
			if (count >= LB_MAX_ENTRIES) break;
			strncpy(entries[count].name, ffblk.filename, LB_NAME_MAXLEN);
			entries[count].name[LB_NAME_MAXLEN] = '\0';
			entries[count].isDir = 1;
			count++;
		} while (lb_findnext(&ffblk) == 0);
	}

	// Then files with recognised extensions
	if (dos2_findfirst("*.*", &ffblk, 0) == 0) {
		do {
			if (ffblk.attribs & (ATTR_DIR | 0x08)) continue;
			if (count >= LB_MAX_ENTRIES) break;

			dot = strrchr(ffblk.filename, '.');
			if (!dot) continue;

			if (strcmp(dot, ".ROM") == 0 ||
			    strcmp(dot, ".DSK") == 0 ||
			    strcmp(dot, ".CAS") == 0 ||
			    strcmp(dot, ".VGM") == 0 ||
			    strcmp(dot, ".COM") == 0 ||
			    strcmp(dot, ".BAS") == 0) {
				strncpy(entries[count].name, ffblk.filename, LB_NAME_MAXLEN);
				entries[count].name[LB_NAME_MAXLEN] = '\0';
				entries[count].isDir = 0;
				count++;
			}
		} while (lb_findnext(&ffblk) == 0);
	}

	return count;
}

// ========================================================
// Print one entry row; selected=true highlights with blink
static void lb_printEntry(uint8_t y, LBEntry_t *e, bool selected)
{
	uint8_t len;

	memset(buff, ' ', 78);
	buff[78] = '\0';

	if (e->isDir) {
		buff[0] = '[';
		len = strlen(e->name);
		memcpy(buff + 1, e->name, len);
		buff[1 + len] = '/';
		buff[2 + len] = ']';
	} else {
		len = strlen(e->name);
		memcpy(buff, e->name, len);
	}

	putlinexy(LB_LIST_X, y, 78, buff);
	textblink(LB_LIST_X, y, 78, selected);
}

// ========================================================
// Render the visible page of entries
static void lb_printList(LBEntry_t *entries, uint8_t count,
                          uint8_t topLine, uint8_t curLine)
{
	uint8_t i;
	uint8_t idx;
	uint8_t y;

	for (i = 0; i < LB_LIST_ROWS; i++) {
		idx = topLine + i;
		y   = LB_LIST_Y + i;
		if (idx < count) {
			lb_printEntry(y, &entries[idx], (i == curLine));
		} else {
			_fillVRAM((uint16_t)((y - 1) * 80), 80, ' ');
			textblink(LB_LIST_X, y, 78, false);
		}
	}

	lb_printCounter(topLine, curLine, count);
}

// ========================================================
// Activate selected entry:
//   directories  -> chdir + return 0 (rescan)
//   .ROM         -> inject "SROM <file>" and exit fhMOD.com
//   .DSK         -> inject "MAPDRV B: <file>" and exit fhMOD.com
//   .COM/.BAS    -> inject "<file>" and exit fhMOD.com
//   other        -> beep, return 0
// Returns 1 if local browser should close (normal file action done).
// Returns 0 if browser should stay open (directory nav or error).
static uint8_t lb_activateEntry(LBEntry_t *entry)
{
	char *dot;

	if (entry->isDir) {
		dos2_setCurrentDirectory(entry->name);
		return 0;
	}

	dot = strrchr(entry->name, '.');
	if (!dot) { putchar('\x07'); return 0; }

	if (strcmp(dot, ".ROM") == 0) {
		// "SROM GAME.ROM" -> inject + exit
		csprintf(buff, "SROM %s", entry->name);
		lb_execCommand(buff);
		// never reached (dos2_exit called inside)

	} else if (strcmp(dot, ".DSK") == 0) {
		// "MAPDRV B: GAME.DSK" -> inject + exit
		// COMMAND.COM will run MAPDRV and return to prompt
		csprintf(buff, "MAPDRV B: %s", entry->name);
		lb_execCommand(buff);
		// never reached

	} else if (strcmp(dot, ".COM") == 0 ||
	           strcmp(dot, ".BAS") == 0) {
		// Run program directly
		csprintf(buff, "%s", entry->name);
		lb_execCommand(buff);
		// never reached
	}

	putchar('\x07');
	return 0;
}

// ========================================================
// Entry point: show the local file browser overlay
void showLocalBrowser(void)
{
	LBEntry_t *entries;
	uint8_t count;
	uint8_t topLine;
	uint8_t curLine;
	bool done;
	char key;
	uint8_t action;
	char savedPath[64];
	char curPath[64];

	// Flush any pending keypresses so no stray key triggers an action immediately
	while (kbhit()) getch();

	// Save current directory so we can restore it when the browser closes
	dos2_getCurrentDirectory(0, savedPath);

	// Navigate to root of current drive — provides an explicit, valid default
	// directory (fixes "no drive/directory reference" crash in Nextor).
	dos2_setCurrentDirectory("\\");

	// Allocate entry list on heap (above existing list data)
	entries = (LBEntry_t *)malloc(LB_MAX_ENTRIES * sizeof(LBEntry_t));
	if (!entries) {
		// Restore original directory and return
		buff[0] = '\\';
		strcpy(buff + 1, savedPath);
		dos2_setCurrentDirectory(buff);
		return;
	}

	setSelectedLine(false);
	lb_drawWindow();

	done    = false;
	topLine = 0;
	curLine = 0;
	count   = lb_scanDir(entries);
	lb_printTitle();
	lb_printList(entries, count, topLine, curLine);

	while (!done) {
		ASM_EI; ASM_HALT;
		if (!kbhit()) continue;

		key = dos2_toupper(getch());

		if (key == KEY_UP) {
			if (count) {
				if (curLine > 0) {
					lb_printEntry(LB_LIST_Y + curLine, &entries[topLine + curLine], false);
					curLine--;
					lb_printEntry(LB_LIST_Y + curLine, &entries[topLine + curLine], true);
					lb_printCounter(topLine, curLine, count);
				} else if (topLine > 0) {
					topLine--;
					lb_printList(entries, count, topLine, curLine);
				}
			}

		} else if (key == KEY_DOWN) {
			if (count && ((uint8_t)(topLine + curLine + 1) < count)) {
				if (curLine < LB_LIST_ROWS - 1) {
					lb_printEntry(LB_LIST_Y + curLine, &entries[topLine + curLine], false);
					curLine++;
					lb_printEntry(LB_LIST_Y + curLine, &entries[topLine + curLine], true);
					lb_printCounter(topLine, curLine, count);
				} else {
					topLine++;
					lb_printList(entries, count, topLine, curLine);
				}
			}

		} else if (key == KEY_RETURN || key == KEY_SELECT) {
			if (count) {
				action = lb_activateEntry(&entries[topLine + curLine]);
				if (action) {
					done = true;
				} else {
					// Navigated into subdir — rescan
					topLine = 0;
					curLine = 0;
					count   = lb_scanDir(entries);
					lb_printTitle();
					lb_printList(entries, count, topLine, curLine);
				}
			}

		} else if (key == KEY_ESC) {
			// Wait for ESC to be physically released
			while (varNEWKEY_row7.esc == 0) { ASM_EI; ASM_HALT; }
			while (kbhit()) getch();

			dos2_getCurrentDirectory(0, curPath);
			if (curPath[0] != '\0') {
				// Go up one level
				dos2_setCurrentDirectory("..");
				topLine = 0;
				curLine = 0;
				count   = lb_scanDir(entries);
				lb_printTitle();
				lb_printList(entries, count, topLine, curLine);
			} else {
				done = true;		// Already at root — close browser
			}
		}
	}

	// Restore original working directory
	buff[0] = '\\';
	strcpy(buff + 1, savedPath);
	dos2_setCurrentDirectory(buff);

	// Free entry list
	free(LB_MAX_ENTRIES * sizeof(LBEntry_t));

	// Restore remote browser display
	fillBlink(LB_WIN_X1, LB_WIN_Y1, LB_WIN_Y2 - LB_WIN_Y1 + 1, 80, false);
	clearBlinkList();
	printTabs();
	printRequestData();
	printList();
	setSelectedLine(true);
}
