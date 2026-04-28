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
#include "mod_launcher.h"


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
	char     name[14];	// ASCIIZ filename (up to 12.3 chars + null)
	uint8_t  isDir;		// 1 = directory, 0 = file
	uint32_t size;		// file size in bytes (0 for directories)
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
// If launchMsg is not NULL it is printed to the console after the screen
// is restored so the user sees it while the launched program is loading.
static void lb_execCommand(const char *cmd, const char *launchMsg)
{
	lb_injectCommand(cmd);
	restoreScreen();
	if (launchMsg) {
		cputs(launchMsg);
	}
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

	// No blink/black background — keep default blue background like the
	// other panels (ROM/DSK/CAS).
	fillBlink(LB_WIN_X1, LB_WIN_Y1, LB_WIN_Y2 - LB_WIN_Y1 + 1, 80, false);
	drawFrame(LB_WIN_X1, LB_WIN_Y1, LB_WIN_X2, LB_WIN_Y2);
	putstrxy(3, LB_WIN_Y2, " UP/DOWN:Navigate  ENTER:Open/Launch  ESC:Back ");

	// Replace the ┌ top-left corner with │ + blank spaces under the
	// "[L]oc" tab text (5 chars), and put a └ at col 7 so the tab's
	// right edge "opens" into the frame like the other tabs do.
	// Col 1 = │, cols 2..6 = spaces, col 7 = └, col 8+ = ─ (already drawn).
	setByteVRAM((uint16_t)((LB_WIN_Y1 - 1) * 80), 0x16);      // │
	_fillVRAM((uint16_t)((LB_WIN_Y1 - 1) * 80 + 1), 5, ' ');  // blank under "[L]oc"
	setByteVRAM((uint16_t)((LB_WIN_Y1 - 1) * 80 + 6), 0x1a);  // └
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
// Scan current directory; fill entries[]; return count.
//
// When NOT at root, two synthetic navigation entries are prepended:
//   "..": go up one level (parent directory)
//   "\" : go directly to the drive root
// They are rendered as [..] and [\] (without a trailing slash) and
// handled specially in lb_activateEntry.
static uint8_t lb_scanDir(LBEntry_t *entries)
{
	FFBLK ffblk;
	uint8_t count = 0;
	char *dot;
	char curPath[64];

	// Are we below the drive root? If so, prepend nav entries.
	dos2_getCurrentDirectory(0, curPath);
	if (curPath[0] != '\0') {
		strcpy(entries[count].name, "..");
		entries[count].isDir = 1;
		entries[count].size  = 0;
		count++;
		strcpy(entries[count].name, "\\");
		entries[count].isDir = 1;
		entries[count].size  = 0;
		count++;
	}

	// Directories first (skip . and ..)
	if (dos2_findfirst("*.*", &ffblk, ATTR_DIR) == 0) {
		do {
			if (!(ffblk.attribs & ATTR_DIR)) continue;
			if (ffblk.filename[0] == '.') continue;
			if (ffblk.filename[0] == '\0') continue;  // skip empty/volume entries
			if (count >= LB_MAX_ENTRIES) break;
			strncpy(entries[count].name, ffblk.filename, LB_NAME_MAXLEN);
			entries[count].name[LB_NAME_MAXLEN] = '\0';
			entries[count].isDir = 1;
			entries[count].size  = 0;
			count++;
		} while (lb_findnext(&ffblk) == 0);
	}

	// Then files with recognised extensions
	if (dos2_findfirst("*.*", &ffblk, 0) == 0) {
		do {
			if (ffblk.attribs & (ATTR_DIR | 0x08)) continue;
			if (ffblk.filename[0] == '\0') continue;  // skip empty/volume entries
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
				entries[count].size  = ffblk.filesize;
				count++;
			}
		} while (lb_findnext(&ffblk) == 0);
	}

	return count;
}

// ========================================================
// Format a byte count into a compact string: "512B", "64K", "1M", etc.
// Output buffer must be at least 7 bytes.
static void lb_formatSize(char *out, uint32_t sz)
{
	uint16_t v;
	if (sz < 1024UL) {
		v = (uint16_t)sz;
		csprintf(out, "%uB", v);
	} else if (sz < 1024UL * 1024UL) {
		v = (uint16_t)(sz / 1024UL);
		csprintf(out, "%uK", v);
	} else {
		v = (uint16_t)(sz / (1024UL * 1024UL));
		csprintf(out, "%uM", v);
	}
}

// ========================================================
// Print one entry row; selected=true highlights with blink
static void lb_printEntry(uint8_t y, LBEntry_t *e, bool selected)
{
	uint8_t len;
	char    sizeBuf[7];

	memset(buff, ' ', 78);
	buff[78] = '\0';

	if (e->isDir) {
		buff[0] = '[';
		len = strlen(e->name);
		memcpy(buff + 1, e->name, len);
		// Synthetic nav entries (".." and "\") render without the
		// trailing '/': [..] and [\]. Real subdirs keep [name/].
		if ((len == 2 && e->name[0] == '.' && e->name[1] == '.') ||
		    (len == 1 && e->name[0] == '\\')) {
			buff[1 + len] = ']';
		} else {
			buff[1 + len] = '/';
			buff[2 + len] = ']';
		}
	} else {
		len = strlen(e->name);
		memcpy(buff, e->name, len);
		// Right-align file size in the last 7 characters of the row
		lb_formatSize(sizeBuf, e->size);
		len = strlen(sizeBuf);
		memcpy(buff + 78 - len, sizeBuf, len);
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
			// Clear only the inner area (cols LB_LIST_X..LB_WIN_X2-1)
			// so the │ border chars at col 1 and col 80 are preserved.
			_fillVRAM((uint16_t)((y - 1) * 80 + (LB_LIST_X - 1)),
			          LB_WIN_X2 - LB_LIST_X, ' ');
			textblink(LB_LIST_X, y, LB_WIN_X2 - LB_LIST_X, false);
		}
	}

	lb_printCounter(topLine, curLine, count);
}

// ========================================================
// Message shown on the DOS console while SofaRun is loading.
// Printed after restoreScreen() so it stays visible during the load.
static const char lb_sofaRunMsg[] =
	"\r\n"
	"  Launching, please wait...\r\n"
	"\r\n"
	"  Powered by SofaRun\r\n"
	"\r\n";

// --------------------------------------------------------
// Detect which SROM /Rx mapper parameter a ROM file needs.
//
// Detection rules (checked in order):
//   1. "AB" header at offset 0     -> 0  (SROM auto-detects)
//   2. "ASCII16X" at offset 0x10   -> 1  (ASCII16-X, /R1)
//      ASCII16-X (grauw.nl/projects/ascii-x) uses 16KB banking with the
//      same register addresses as ASCII16 (6000H/7000H). For ROMs <=4MB
//      the bank number fits in 8 bits, so it is fully /R1-compatible.
//   3. No tag, size <= 64KB        -> 9  (Linear,  /R9)
//   4. No tag, size <= 128KB       -> 1  (ASCII16, /R1)
//   5. No tag, size >  128KB       -> 3  (ASCII8,  /R3)
//
// Returns 0 on any file-access error (SROM will try its own auto-detect).
static uint8_t lb_detectROMMapper(const char *filename)
{
	FILEH   fh;
	char    hdr[24];	// enough to cover AB header (0) and ASCII16X tag (0x10..0x17)
	int32_t size;

	size = dos2_filesize((char*)filename);
	if (size < 0) return 0;

	fh = dos2_fopen((char*)filename, O_RDONLY);
	if (fh > 20) return 0;
	dos2_fread(hdr, sizeof(hdr), fh);
	dos2_fclose(fh);

	// 1. Standard MSX ROM: let SROM handle auto-detection
	if (hdr[0] == 'A' && hdr[1] == 'B') return 0;

	// 2. ASCII16-X: official identifier at offset 0x10
	if (size >= (int32_t)sizeof(hdr) &&
	    memcmp(&hdr[0x10], "ASCII16X", 8) == 0) return 1;

	// 3-5. Size-based heuristics
	if (size <= 0x10000L) return 9;   // <= 64 KB  : Linear  /R9
	if (size <= 0x20000L) return 1;   // <= 128 KB : ASCII16 /R1
	return 3;                          //  > 128 KB : ASCII8  /R3
}

// Activate selected entry:
//   directories  -> chdir + return 0 (rescan)
//   .ROM         -> auto-detect mapper, inject "SROM [/Rx] <file>" + exit
//   .DSK         -> inject "SRI <file>" and exit fhMOD.com  (SofaRunIt)
//   .COM/.BAS    -> inject "<file>"     and exit fhMOD.com
//   other        -> beep, return 0
// Returns 1 if local browser should close (file action done).
// Returns 0 if browser should stay open (directory nav or error).
static uint8_t lb_activateEntry(LBEntry_t *entry)
{
	char    *dot;
	uint8_t  mapper;

	if (entry->isDir) {
		dos2_setCurrentDirectory(entry->name);
		return 0;
	}

	dot = strrchr(entry->name, '.');
	if (!dot) { putchar('\x07'); return 0; }

	if (strcmp(dot, ".ROM") == 0) {
		mapper = lb_detectROMMapper(entry->name);
		if (mapper) {
			csprintf(buff, "SROM /R%u %s", (uint16_t)mapper, entry->name);
		} else {
			csprintf(buff, "SROM %s", entry->name);
		}
		lb_execCommand(buff, lb_sofaRunMsg);
		// never reached (dos2_exit called inside)

	} else if (strcmp(dot, ".DSK") == 0) {
		// SofaRunIt has no quiet-mode flag
		csprintf(buff, "SRI %s", entry->name);
		lb_execCommand(buff, lb_sofaRunMsg);
		// never reached

	} else if (strcmp(dot, ".COM") == 0 ||
	           strcmp(dot, ".BAS") == 0) {
		// Run program directly — no launcher message
		csprintf(buff, "%s", entry->name);
		lb_execCommand(buff, NULL);
		// never reached
	}

	putchar('\x07');
	return 0;
}

// ========================================================
// Entry point: show the local file browser overlay
bool showLocalBrowser(void)
{
	LBEntry_t *entries;
	uint8_t count;
	uint8_t topLine;
	uint8_t curLine;
	bool done;
	bool tabExit;
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
	tabExit = false;
	topLine = 0;
	curLine = 0;
	count   = lb_scanDir(entries);
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
							lb_printList(entries, count, topLine, curLine);
			} else {
				done = true;		// Already at root — close browser
			}

		} else if (key == KEY_TAB) {
			// TAB closes the browser and signals the caller to advance
			// to the next panel (so a single TAB press cycles panels).
			tabExit = true;
			done    = true;

		} else if (key == '5') {
			// F5: Launch OCMINFO.COM (resolved via PATH), same as in
			// the network browser. launchOcmInfo() injects the command
			// into the BIOS keyboard buffer and exits so COMMAND.COM
			// picks it up. Only returns here on error.
			//
			// Restore working directory first so the user's original
			// CWD is in effect when OCMINFO.COM runs.
			buff[0] = '\\';
			strcpy(buff + 1, savedPath);
			dos2_setCurrentDirectory(buff);
			free(LB_MAX_ENTRIES * sizeof(LBEntry_t));
			restoreScreen();
			launchOcmInfo();
			dos2_exit(1);	// only reached if OCMINFO.COM not found
		}
	}

	// Restore original working directory
	buff[0] = '\\';
	strcpy(buff + 1, savedPath);
	dos2_setCurrentDirectory(buff);

	// Free entry list
	free(LB_MAX_ENTRIES * sizeof(LBEntry_t));

	// Clear all blink attributes over the window so no black background
	// bleeds through; caller (openLocalPanel / runLocalModeLoop) takes
	// care of redrawing tabs, frame edges and list area.
	fillBlink(LB_WIN_X1, LB_WIN_Y1, LB_WIN_Y2 - LB_WIN_Y1 + 1, 80, false);
	clearBlinkList();

	// NOTE: do NOT call printList() here — list_start may point to stale
	// remote-list data from a previous ROM/DSK/CAS panel; on a JP MSX
	// those bytes render as kana glyphs ("Japanese text flash" bug).

	return tabExit;
}
