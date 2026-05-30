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
#include "mod_joystick.h"


// ========================================================
#define LB_WIN_X1		1
#define LB_WIN_Y1		(PANEL_FIRSTY - 1)		// row 4 (1-based)
#define LB_WIN_X2		80
#define LB_WIN_Y2		(PANEL_LASTY + 1)		// row 23 (1-based)
#define LB_LIST_X		2
// First list entry row: LB_WIN_Y1+1 sits right under the frame top (row 5),
// matching the layout of the ROM/DSK/CAS network panels (PANEL_FIRSTY=5).
// Earlier this was LB_WIN_Y1+2 which left an empty row 5 between the frame
// and the first item — the prior "Japanese flash" patch only stopped the
// stale-glyph rendering on that row, the blank gap stayed.
#define LB_LIST_Y		(LB_WIN_Y1 + 1)
#define LB_LIST_ROWS	(LB_WIN_Y2 - LB_LIST_Y - 1)	// visible rows for entries

#define LB_MAX_ENTRIES	50				// max entries per directory scan
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
extern void showHelpWindow();
// restoreScreen() is declared in fh.h (already included)

// Forward decl — defined further down. Called by lb_copyLoadcax,
// lb_buildCasStub and lb_buildDskBat to record a residual file's
// absolute path in the FHCLEAN.LST manifest so the next fhMOD launch
// can wipe it.
static void lb_appendManifest(const char *filename);

// BDOS 0x0E (SELDRV) — switch the active drive.
//
// MSX-DOS 2's CHDIR (used by dos2_setCurrentDirectory) only sets the
// "current directory" attribute of a drive; it does NOT change the
// active drive. So passing "B:\\" to dos2_setCurrentDirectory updates
// B:'s remembered directory but the user still sees A:. We need
// SELDRV first, then CHDIR to "\\" inside the new active drive.
//
// IX/IY are caller-saved under __sdcccall(1) so push/pop them around
// the BDOS call.
static void lb_selectDrive(uint8_t drive) __naked __sdcccall(1)
{
	drive;	/* A = drive number, 0 = A:, 1 = B:, ... */
	__asm
		push ix
		push iy
		ld   e, a
		ld   c, #0x0E
		call 5
		pop  iy
		pop  ix
		ret
	__endasm;
}

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
	// "[L]ocal" tab text (7 chars), and put a └ at col 9 so the tab's
	// right edge "opens" into the frame like the other tabs do.
	// Col 1 = │, cols 2..8 = spaces, col 9 = └, col 10+ = ─ (already drawn).
	setByteVRAM((uint16_t)((LB_WIN_Y1 - 1) * 80), 0x16);      // │
	_fillVRAM((uint16_t)((LB_WIN_Y1 - 1) * 80 + 1), 7, ' ');  // blank under "[L]ocal"
	setByteVRAM((uint16_t)((LB_WIN_Y1 - 1) * 80 + 8), 0x1a);  // └
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
// Synthetic entries are always prepended at the top:
//   "A:" .. "H:"  every mounted drive (probed with getCurrentDirectory)
//   ".."          parent directory (only when not at drive root)
//   "\"           drive root        (only when not at drive root)
// They render as [A:], [..], [\] and are handled specially in
// lb_activateEntry.
static uint8_t lb_scanDir(LBEntry_t *entries)
{
	FFBLK ffblk;
	uint8_t count = 0;
	uint8_t d;
	char *dot;
	char curPath[64];
	char tmpPath[64];

	// 1) Drives: probe A:..H:. dos2_getCurrentDirectory returns 0 on
	//    success; any other code means the drive is not available.
	//    The probe also auto-logs the drive in, which is cheap.
	for (d = 1; d <= 8 && count < LB_MAX_ENTRIES; d++) {
		if (dos2_getCurrentDirectory(d, tmpPath) == 0) {
			entries[count].name[0] = (char)('A' + d - 1);
			entries[count].name[1] = ':';
			entries[count].name[2] = '\0';
			entries[count].isDir   = 1;
			entries[count].size    = 0;
			count++;
		}
	}

	// 2) Are we below the drive root? If so, append nav entries.
	dos2_getCurrentDirectory(0, curPath);
	if (curPath[0] != '\0' && count < LB_MAX_ENTRIES) {
		strcpy(entries[count].name, "..");
		entries[count].isDir = 1;
		entries[count].size  = 0;
		count++;
	}
	if (curPath[0] != '\0' && count < LB_MAX_ENTRIES) {
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
		// Synthetic nav entries render without the trailing '/':
		//   "..", "\", drive letters "X:"   ->   [..], [\], [X:]
		// Real subdirectories keep [name/].
		if ((len == 2 && e->name[0] == '.' && e->name[1] == '.') ||
		    (len == 1 && e->name[0] == '\\') ||
		    (len == 2 && e->name[1] == ':')) {
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
//   1. "AB" header at offset 0 and size <= 64 KB
//        -> read init address from bytes 2..3 (little-endian) and pick:
//             init in 0x0000-0x3FFF -> 10 (Linear0,  /R10)
//             init in 0x4000-0xBFFF ->  9 (Linear,   /R9)
//             init in 0xC000-0xFFFF -> 11 (LinearC,  /R11)
//      SROM's "auto" mode (/R0) misclassifies several linear ROMs
//      (the docs at louthrax.com/mgr/sofarom_usage.html explicitly
//      list Linear0 and LinearC as "not natively supported" on some
//      flashcarts) so we pass the explicit flag.
//   2. "AB" header at offset 0 and size > 64 KB    -> 0 (auto-detect)
//      Too big for a flat ROM, must be a MegaROM with the signature
//      in bank 0; let SROM's auto-detection pick the right mapper.
//   3. "ASCII16X" at offset 0x10                    -> 1  (ASCII16-X /R1)
//      Same register addresses (6000/7000H) as ASCII16; works on /R1
//      for ROMs up to 4 MB (bank index still fits in 8 bits).
//   4. No tag, size <= 64KB                         -> 9  (Linear  /R9)
//   5. No tag, size <= 128KB                        -> 1  (ASCII16 /R1)
//   6. No tag, size >  128KB                        -> 3  (ASCII8  /R3)
//
// Returns 0 on any file-access error (SROM will try its own auto-detect).
static uint8_t lb_detectROMMapper(const char *filename)
{
	FILEH    fh;
	char     hdr[24];	// covers AB header (0..7) + ASCII16X tag (0x10..0x17)
	int32_t  size;
	uint16_t initAddr;

	size = dos2_filesize((char*)filename);
	if (size < 0) return 0;

	fh = dos2_fopen((char*)filename, O_RDONLY);
	if (fh > 20) return 0;
	dos2_fread(hdr, sizeof(hdr), fh);
	dos2_fclose(fh);

	// 1/2. Standard MSX ROM with the "AB" signature.
	if (hdr[0] == 'A' && hdr[1] == 'B') {
		if (size > 0x10000L) return 0;	// MegaROM, auto-detect
		initAddr = (uint16_t)((uint8_t)hdr[2] | ((uint8_t)hdr[3] << 8));
		if (initAddr >= 0xC000)      return 11;	// LinearC  /R11
		if (initAddr <  0x4000)      return 10;	// Linear0  /R10
		return 9;                                  // Linear   /R9
	}

	// 3. ASCII16-X: official identifier at offset 0x10
	if (size >= (int32_t)sizeof(hdr) &&
	    memcmp(&hdr[0x10], "ASCII16X", 8) == 0) return 1;

	// 4-6. Size-based heuristics for ROMs without a recognised header
	if (size <= 0x10000L) return 9;   // <= 64 KB  : Linear  /R9
	if (size <= 0x20000L) return 1;   // <= 128 KB : ASCII16 /R1
	return 3;                          //  > 128 KB : ASCII8  /R3
}

// Path of the canonical LOADCAX binary. Must live alongside fhMOD.com.
#define LOADCAX_SRC_PATH	"\\UTILS\\LOADCAX"
#define LOADCAX_DEST_NAME	"LOADCAX"
// Manifest of residual files dropped into game dirs (LOADCAX, FHCAS.BAS,
// FHRUN.BAT). Read + wiped once at fhMOD startup.
#define LB_MANIFEST_PATH	"\\UTILS\\FHCLEAN.LST"
#define LB_MANIFEST_BUFLEN	1024
// Matches the BUFF_SIZE used to malloc 'buff' in fhMOD.c. Keep in sync.
#define LB_BUFF_SIZE		200

// Copy A:\UTILS\LOADCAX into the current directory if it isn't already
// there. LOADCAX (~1.6 KB) must sit next to the .CAS or it errors out
// with "cas file not found or broken". When a previous launch already
// dropped a copy in this directory we skip the IO entirely. Re-uses
// 'buff' (200 B) as the IO buffer; a fresh copy is ~9 round trips.
static bool lb_copyLoadcax(void)
{
	FILEH    fhSrc;
	FILEH    fhDst;
	int16_t  n;

	// Reuse an existing copy if one is already next to the .CAS.
	if (dos2_fileexists(LOADCAX_DEST_NAME)) return true;

	fhSrc = dos2_fopen(LOADCAX_SRC_PATH, O_RDONLY);
	if (fhSrc >= ERR_FIRST) return false;

	fhDst = dos2_fcreate(LOADCAX_DEST_NAME, O_WRONLY, ATTR_ARCHIVE);
	if (fhDst >= ERR_FIRST) {
		dos2_fclose(fhSrc);
		return false;
	}

	for (;;) {
		n = (int16_t)dos2_fread(buff, LB_BUFF_SIZE, fhSrc);
		if (n <= 0) break;
		dos2_fwrite(buff, (uint16_t)n, fhDst);
	}

	dos2_fclose(fhDst);
	dos2_fclose(fhSrc);
	lb_appendManifest(LOADCAX_DEST_NAME);
	return true;
}

// Append the absolute path of `filename` (which is created in CWD) to
// the cleanup manifest. Used so the next fhMOD launch can wipe every
// residual LOADCAX / FHCAS.BAS / FHRUN.BAT we ever drop on game dirs.
//
// The manifest is rewritten in full each time — appending via seek-to-
// end would need raw BDOS 0x4A which we don't have wrapped here. Cost
// is irrelevant: at most a handful of paths, < 1 KB total.
static void lb_appendManifest(const char *filename)
{
	static char manifest[LB_MANIFEST_BUFLEN];
	char        absPath[80];
	char        curPath[64];
	uint8_t     drive;
	uint16_t    used = 0;
	uint16_t    n;
	FILEH       fh;

	/* Build "<drive>:\<cwd>\<filename>". CWD is returned without a
	   leading backslash and without a drive prefix. */
	drive = getCurrentDrive();
	dos2_getCurrentDirectory(0, curPath);
	absPath[0] = (char)('A' + drive);
	absPath[1] = ':';
	absPath[2] = '\\';
	absPath[3] = '\0';
	n = 3;
	if (curPath[0]) {
		uint16_t cl = (uint16_t)strlen(curPath);
		if (n + cl + 1 >= sizeof(absPath)) return;
		memcpy(absPath + n, curPath, cl); n += cl;
		absPath[n++] = '\\';
		absPath[n] = '\0';
	}
	{
		uint16_t fl = (uint16_t)strlen(filename);
		if (n + fl + 1 >= sizeof(absPath)) return;
		memcpy(absPath + n, filename, fl); n += fl;
		absPath[n] = '\0';
	}

	/* Slurp existing manifest (if any). */
	fh = dos2_fopen(LB_MANIFEST_PATH, O_RDONLY);
	if (fh < ERR_FIRST) {
		used = (uint16_t)dos2_fread(manifest, LB_MANIFEST_BUFLEN - 1, fh);
		dos2_fclose(fh);
		if (used >= LB_MANIFEST_BUFLEN) used = LB_MANIFEST_BUFLEN - 1;
	}

	/* Append "<absPath>\r\n" — bail if it would overflow. */
	{
		uint16_t pl = (uint16_t)strlen(absPath);
		if (used + pl + 2 >= LB_MANIFEST_BUFLEN) return;
		memcpy(manifest + used, absPath, pl); used += pl;
		manifest[used++] = '\r';
		manifest[used++] = '\n';
	}

	/* Rewrite from scratch (dos2_fcreate refuses to overwrite). */
	dos2_remove(LB_MANIFEST_PATH);
	fh = dos2_fcreate(LB_MANIFEST_PATH, O_WRONLY, ATTR_ARCHIVE);
	if (fh >= ERR_FIRST) return;
	dos2_fwrite(manifest, used, fh);
	dos2_fclose(fh);
}

void lb_cleanupResiduals(void)
{
	static char manifest[LB_MANIFEST_BUFLEN];
	char        path[80];
	uint16_t    used;
	uint16_t    i;
	uint16_t    lineStart;
	uint16_t    len;
	FILEH       fh;

	fh = dos2_fopen(LB_MANIFEST_PATH, O_RDONLY);
	if (fh >= ERR_FIRST) return;
	used = (uint16_t)dos2_fread(manifest, LB_MANIFEST_BUFLEN - 1, fh);
	dos2_fclose(fh);
	if (used >= LB_MANIFEST_BUFLEN) used = LB_MANIFEST_BUFLEN - 1;
	manifest[used] = '\0';

	/* Walk lines and delete each. Empty lines and lines that don't fit
	   in `path` are silently skipped. */
	lineStart = 0;
	for (i = 0; i <= used; i++) {
		if (i == used || manifest[i] == '\r' || manifest[i] == '\n') {
			len = i - lineStart;
			if (len > 0 && len < sizeof(path)) {
				memcpy(path, manifest + lineStart, len);
				path[len] = '\0';
				dos2_remove(path);
			}
			/* Skip the rest of the CRLF / multiple newlines. */
			while (i < used && (manifest[i] == '\r' || manifest[i] == '\n'))
				i++;
			lineStart = i;
			if (i < used) i--;	/* offset the loop ++ */
		}
	}

	dos2_remove(LB_MANIFEST_PATH);
}

// Paint a centred "Loading game..." box on top of the local browser so
// the user gets immediate feedback while we copy LOADCAX, write the
// stub, and queue the BASIC command. Uses the same line-drawing chars
// as the rest of the app via drawFrame().
static void lb_showLoadingBox(void)
{
	const uint8_t x1 = 26;	// (80 - 28) / 2 + 1
	const uint8_t y1 = 10;
	const uint8_t x2 = 53;	// x1 + 27
	const uint8_t y2 = 14;
	uint8_t y;

	// Wipe the inside of the box (rows y1+1..y2-1, cols x1..x2-1).
	for (y = y1 + 1; y < y2; y++) {
		_fillVRAM((uint16_t)((y - 1) * 80 + (x1 - 1)),
		          (uint16_t)(x2 - x1 + 1), ' ');
	}
	drawFrame(x1, y1, x2, y2);
	putstrxy(x1 + 8, y1 + 2, "Loading game...");
}

// Build a stub FHCAS.BAS in the current directory and inject the
// command "BASIC FHCAS.BAS" so MSX BASIC autoruns it on next boot.
//
// Why this dance?  A .CAS file isn't an executable: it is a raw dump
// of cassette bytes that only MSX BIOS/BASIC's tape routines know how
// to feed to a program. LOADCAX (k0gaMSX/legacy/APPS/CASUTILS) is a
// BASIC binary that hooks the cassette BIOS routines and replays the
// bytes from a .CAS file when BASIC executes its standard CLOAD/RUN
// commands. The README says verbatim:
//
//     BLOAD"LOADCAX",R'<basename>
//
// where <basename> is the .CAS filename without the extension. LOADCAX
// requires that BOTH the loader binary AND the .CAS sit in the SAME
// directory (it opens "<basename>.CAS" relative to CWD), so before
// writing the stub we copy LOADCAX from A:\UTILS into the CAS's dir.
// Returns false on any file-IO error so the caller can beep + stay.
static bool lb_buildCasStub(const char *casFilename)
{
	char    base[16];
	uint8_t baseLen;
	FILEH   fh;
	uint16_t len;
	const char *dot = strrchr(casFilename, '.');

	baseLen = (uint8_t)(dot ? (uint8_t)(dot - casFilename) : strlen(casFilename));
	if (baseLen >= sizeof(base)) baseLen = sizeof(base) - 1;
	memcpy(base, casFilename, baseLen);
	base[baseLen] = '\0';

	// 1) Make sure LOADCAX is right next to the .CAS.
	if (!lb_copyLoadcax()) return false;

	// 2) Drop any leftover stub from a previous launch so the create
	//    always succeeds (dos2_fcreate refuses to overwrite).
	dos2_remove("FHCAS.BAS");

	fh = dos2_fcreate("FHCAS.BAS", O_WRONLY, ATTR_ARCHIVE);
	if (fh >= ERR_FIRST) return false;

	// MSX BASIC accepts ASCII source files. CR+LF line ending.
	csprintf(buff, "10 BLOAD\"LOADCAX\",R'%s\r\n", base);
	len = (uint16_t)strlen(buff);
	dos2_fwrite(buff, len, fh);
	dos2_fclose(fh);
	lb_appendManifest("FHCAS.BAS");
	return true;
}

// Detect a multi-disk set sharing the selected file's basename, e.g.
// MEMOIRS1.DSK / MEMOIRS2.DSK / MEMOIRS3.DSK. Strips the trailing run
// of digits from the basename of `selected` and probes the disk for
// every <base><N>.DSK with N = 1..20 (SofaRunIt's hard limit). Fills
// out[] with the matches in numeric order and returns the count.
//
// Returns 0 if the filename does not end in a digit, or if probing
// finds only a single file — the caller falls back to a single-disk
// launch in that case.
#define LB_MAX_DISKS 20
#define LB_DSK_NAMELEN 14   // 8.3 + null

static uint8_t lb_findMultiDsk(const char *selected,
                                char        out[LB_MAX_DISKS][LB_DSK_NAMELEN])
{
	char        base[16];
	const char *dot = strrchr(selected, '.');
	uint8_t     dotIdx;
	uint8_t     firstDigit;
	uint8_t     count = 0;
	uint8_t     i;
	char        candidate[LB_DSK_NAMELEN];

	if (!dot) return 0;
	dotIdx = (uint8_t)(dot - selected);

	// Walk back from the dot while the previous char is a digit.
	firstDigit = dotIdx;
	while (firstDigit > 0 && selected[firstDigit - 1] >= '0'
	                      && selected[firstDigit - 1] <= '9')
		firstDigit--;
	if (firstDigit == dotIdx) return 0;	// no trailing digit
	if (firstDigit >= sizeof(base)) return 0;

	memcpy(base, selected, firstDigit);
	base[firstDigit] = '\0';

	// Enumerate <base><N>.DSK for N=1..20.
	for (i = 1; i <= LB_MAX_DISKS && count < LB_MAX_DISKS; i++) {
		csprintf(candidate, "%s%u.DSK", base, (uint16_t)i);
		if (dos2_fileexists(candidate)) {
			strcpy(out[count], candidate);
			count++;
		}
	}
	return (count >= 2) ? count : 0;
}

// Build a FHRUN.BAT in the current directory containing the full SRI
// command line. Used for multi-disk launches because the BIOS keyboard
// buffer is only 40 bytes — way too small to inject 4+ filenames. We
// inject "FHRUN.BAT" (9 bytes) instead and COMMAND.COM executes the
// .BAT, which can be arbitrarily long.
//
// Written byte-by-chunk via dos2_fwrite, NOT through 'buff' (only 200
// bytes; with 14+ disks the SRI line would overflow).
static bool lb_buildDskBat(char dsks[LB_MAX_DISKS][LB_DSK_NAMELEN],
                            uint8_t count)
{
	FILEH   fh;
	uint8_t i;
	uint16_t n;

	dos2_remove("FHRUN.BAT");
	fh = dos2_fcreate("FHRUN.BAT", O_WRONLY, ATTR_ARCHIVE);
	if (fh >= ERR_FIRST) return false;

	dos2_fwrite("SRI", 3, fh);
	for (i = 0; i < count; i++) {
		dos2_fwrite(" ", 1, fh);
		n = (uint16_t)strlen(dsks[i]);
		dos2_fwrite(dsks[i], n, fh);
	}
	dos2_fwrite("\r\n", 2, fh);
	dos2_fclose(fh);
	lb_appendManifest("FHRUN.BAT");
	return true;
}

// Centred "Exit fhMOD? Y / N" confirmation popup. Blocks until Y/ENTER
// (-> true) or N/ESC (-> false). Caller redraws underneath.
bool lb_confirmExit(void)
{
	const uint8_t x1 = 28;
	const uint8_t y1 = 10;
	const uint8_t x2 = 51;	/* 24 cols wide */
	const uint8_t y2 = 14;
	uint8_t y;
	char    ch;

	for (y = y1 + 1; y < y2; y++) {
		_fillVRAM((uint16_t)((y - 1) * 80 + (x1 - 1)),
		          (uint16_t)(x2 - x1 + 1), ' ');
	}
	drawFrame(x1, y1, x2, y2);
	putstrxy(x1 + 5, y1 + 1, "Exit fhMOD?");
	putstrxy(x1 + 3, y1 + 3, "Y = yes    N = no");

	/* Drain any pending keys so a stale Y/N doesn't auto-trigger. */
	while (kbhit()) getch();
	for (;;) {
		ASM_EI; ASM_HALT;
		if (!kbhit()) continue;
		ch = dos2_toupper(getch());
		if (ch == 'Y' || ch == KEY_RETURN) return true;
		if (ch == 'N' || ch == KEY_ESC)    return false;
	}
}

// Centred popup that asks the user which launcher to use for a .ROM.
// Returns:
//   'S' = SofaROM (SROM) — uses fhMOD's Linear/Linear0/LinearC mapper
//                          detection so the right /Rx flag is passed.
//   'M' = mglOcm        — closed-source ToughkidCST OCM-native loader.
//                          Has its own mapper auto-detect; no flags
//                          needed. Must be on the DOS PATH.
//    0  = ESC / cancel  — caller restores list and returns to browser.
static char lb_chooseRomLauncher(void)
{
	const uint8_t x1 = 25;
	const uint8_t y1 = 9;
	const uint8_t x2 = 54;	/* 30 cols wide */
	const uint8_t y2 = 16;
	uint8_t y;
	char    ch;

	for (y = y1 + 1; y < y2; y++) {
		_fillVRAM((uint16_t)((y - 1) * 80 + (x1 - 1)),
		          (uint16_t)(x2 - x1 + 1), ' ');
	}
	drawFrame(x1, y1, x2, y2);
	putstrxy(x1 + 6, y1 + 1, "Launch ROM with:");
	putstrxy(x1 + 4, y1 + 3, "S = SROM (SofaROM)");
	putstrxy(x1 + 4, y1 + 4, "M = mglOcm");
	putstrxy(x1 + 4, y1 + 5, "ESC = cancel");

	/* Flush stale keys so a previous keystroke doesn't auto-pick. */
	while (kbhit()) getch();
	for (;;) {
		ASM_EI; ASM_HALT;
		if (!kbhit()) continue;
		ch = dos2_toupper(getch());
		if (ch == 'S' || ch == 'M') return ch;
		if (ch == KEY_ESC)          return 0;
	}
}

// Centred popup that tells the user how many disks were detected and
// blocks until any key is pressed. Drawn with drawFrame() so it shares
// the look of the loading box.
static void lb_showMultiDiskBox(uint8_t count)
{
	const uint8_t x1 = 22;
	const uint8_t y1 = 10;
	const uint8_t x2 = 57;
	const uint8_t y2 = 15;
	uint8_t y;

	for (y = y1 + 1; y < y2; y++) {
		_fillVRAM((uint16_t)((y - 1) * 80 + (x1 - 1)),
		          (uint16_t)(x2 - x1 + 1), ' ');
	}
	drawFrame(x1, y1, x2, y2);

	csprintf(buff, "%u DSK images detected.", (uint16_t)count);
	putstrxy(x1 + 4, y1 + 1, buff);
	putstrxy(x1 + 3, y1 + 3, "Press any key to launch...");

	while (kbhit()) getch();		// flush
	while (!kbhit()) { ASM_EI; ASM_HALT; }
	getch();
}

// Activate selected entry:
//   directories  -> chdir + return 0 (rescan)
//   .ROM         -> auto-detect mapper, inject "SROM [/Rx] <file>" + exit
//   .DSK         -> single: inject "SRI <file>". Multi-disk (basename
//                   ends in a digit and siblings exist): build FHRUN.BAT
//                   with "SRI disk1 disk2 ..." and inject FHRUN.BAT.
//   .CAS         -> write FHCAS.BAS stub in CWD then inject "BASIC FHCAS.BAS"
//                   so BASIC autoruns LOADCAX on the cassette image
//   .COM/.BAS    -> inject "<file>"     and exit fhMOD.com
//   other        -> beep, return 0
// Returns 1 if local browser should close (file action done).
// Returns 0 if browser should stay open (directory nav or error).
static uint8_t lb_activateEntry(LBEntry_t *entry)
{
	char    *dot;
	uint8_t  mapper;

	if (entry->isDir) {
		// Drive entries like "A:" -> switch active drive via SELDRV,
		// then CHDIR to "\" so we land at the new drive's root.
		// Plain CHDIR("X:\\") doesn't switch the active drive on
		// MSX-DOS 2 / Nextor — it only updates X:'s remembered dir.
		if (entry->name[0] && entry->name[1] == ':' && entry->name[2] == '\0') {
			lb_selectDrive((uint8_t)(entry->name[0] - 'A'));
			dos2_setCurrentDirectory("\\");
		} else {
			dos2_setCurrentDirectory(entry->name);
		}
		return 0;
	}

	dot = strrchr(entry->name, '.');
	if (!dot) { putchar('\x07'); return 0; }

	if (strcmp(dot, ".ROM") == 0) {
		// Ask the user which launcher to use. SROM + fhMOD's mapper
		// detection is the default, but some ROMs (looking at you,
		// odd Linear variants) work better with mglOcm's native
		// OCM-aware auto-detection.
		char launcher = lb_chooseRomLauncher();
		if (launcher == 0) {
			// Cancelled — caller redraws via rescan path.
			return 0;
		}
		lb_showLoadingBox();
		if (launcher == 'M') {
			// mglOcm has its own mapper auto-detection.
			csprintf(buff, "mglOcm %s", entry->name);
		} else {
			// SROM with explicit /Rx from header detection.
			mapper = lb_detectROMMapper(entry->name);
			if (mapper) {
				csprintf(buff, "SROM /R%u %s", (uint16_t)mapper, entry->name);
			} else {
				csprintf(buff, "SROM %s", entry->name);
			}
		}
		lb_execCommand(buff, lb_sofaRunMsg);
		// never reached (dos2_exit called inside)

	} else if (strcmp(dot, ".DSK") == 0) {
		// Try multi-disk detection first. If the basename ends in a
		// digit and 2+ siblings exist (<base>1.DSK..<base>N.DSK) we
		// hand them all to SRI via a FHRUN.BAT — the BIOS keyboard
		// buffer is too small (40 bytes) for 4+ filenames inline.
		{
			char dsks[LB_MAX_DISKS][LB_DSK_NAMELEN];
			uint8_t n = lb_findMultiDsk(entry->name, dsks);
			if (n >= 2) {
				lb_showMultiDiskBox(n);
				if (!lb_buildDskBat(dsks, n)) {
					putchar('\x07');
					return 0;
				}
				lb_showLoadingBox();
				csprintf(buff, "FHRUN.BAT");
				lb_execCommand(buff, NULL);
				// never reached
			}
		}
		// Single disk — original path.
		lb_showLoadingBox();
		// SofaRunIt has no quiet-mode flag
		csprintf(buff, "SRI %s", entry->name);
		lb_execCommand(buff, lb_sofaRunMsg);
		// never reached

	} else if (strcmp(dot, ".CAS") == 0) {
		// Show feedback BEFORE we start IO — the LOADCAX copy + stub
		// write + DOS dance can take a noticeable second on slow media.
		lb_showLoadingBox();
		if (!lb_buildCasStub(entry->name)) {
			putchar('\x07');
			return 0;
		}
		csprintf(buff, "BASIC FHCAS.BAS");
		lb_execCommand(buff, NULL);
		// never reached

	} else if (strcmp(dot, ".COM") == 0) {
		// .COM: direct execution
		csprintf(buff, "%s", entry->name);
		lb_execCommand(buff, NULL);
		// never reached

	} else if (strcmp(dot, ".BAS") == 0) {
		// .BAS: hand off to BASIC. MSX-DOS's BASIC command auto-runs the
		// file given as its argument, equivalent to RUN"<name>" inside
		// the interpreter. No FHBAS.BAS stub needed because BASIC reads
		// the .BAS path directly from the command line.
		lb_showLoadingBox();
		csprintf(buff, "BASIC %s", entry->name);
		lb_execCommand(buff, NULL);
		// never reached
	}

	putchar('\x07');
	return 0;
}

// ========================================================
// Entry point: show the local file browser overlay.
// Returns one of LB_EXIT_* — the caller decides what panel to switch to.
uint8_t showLocalBrowser(void)
{
	LBEntry_t *entries;
	uint8_t count;
	uint8_t topLine;
	uint8_t curLine;
	bool done;
	uint8_t exitCode;
	char key;
	uint8_t action;
	char curPath[64];

	// Flush any pending keypresses so no stray key triggers an action immediately
	while (kbhit()) getch();

	// We do NOT chdir to "\" any more, and we no longer save+restore
	// the drive/path on exit. The CWD now follows whatever the user
	// navigates to in the local browser, so:
	//   - Re-entering the local browser shows the last visited dir.
	//   - Downloads from network tabs land in that same dir.
	// (The old behaviour reset CWD on entry and restored it on exit,
	// which made downloads go to wherever fhMOD was first launched
	// from regardless of where the user had navigated.)

	// Allocate entry list on heap (above existing list data)
	entries = (LBEntry_t *)malloc(LB_MAX_ENTRIES * sizeof(LBEntry_t));
	if (!entries) {
		return LB_EXIT_CLOSE;
	}

	setSelectedLine(false);
	lb_drawWindow();

	done     = false;
	exitCode = LB_EXIT_CLOSE;
	topLine  = 0;
	curLine  = 0;
	count    = lb_scanDir(entries);
	lb_printList(entries, count, topLine, curLine);

	while (!done) {
		ASM_EI; ASM_HALT;
		uint8_t joyKey = joystickPoll();
		if (!kbhit() && !joyKey) continue;

		key = joyKey ? (char)joyKey : dos2_toupper(getch());

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
				// Already at drive root: ask the user whether to quit
				// fhMOD entirely instead of just dropping back to an
				// empty Local panel (which would need yet another ESC
				// to actually leave). LB_EXIT_QUIT propagates to
				// menu_loop and ends the program.
				if (lb_confirmExit()) {
					exitCode = LB_EXIT_QUIT;
					done     = true;
				} else {
					// Redraw the overlay underneath the popup.
					lb_drawWindow();
					lb_printList(entries, count, topLine, curLine);
				}
			}

		} else if (key == KEY_TAB) {
			// TAB closes the browser and signals the caller to advance
			// to the next panel (so a single TAB press cycles panels).
			exitCode = LB_EXIT_TAB;
			done     = true;

		} else if (key == 'R') {
			exitCode = LB_EXIT_ROM;
			done     = true;

		} else if (key == 'D') {
			exitCode = LB_EXIT_DSK;
			done     = true;

		} else if (key == 'C') {
			exitCode = LB_EXIT_CAS;
			done     = true;

		} else if (key == '1') {
			// F1: show help. showHelpWindow() draws over the screen and
			// on exit calls printList() (the network panel renderer),
			// so we redraw our overlay on top to clean that up.
			showHelpWindow();
			lb_drawWindow();
			lb_printList(entries, count, topLine, curLine);

		} else if (key == '5') {
			// F5: Launch OCMINFO.COM (resolved via PATH), same as in
			// the network browser. launchOcmInfo() injects the command
			// into the BIOS keyboard buffer and exits so COMMAND.COM
			// picks it up. Only returns here on error.
			// CWD is left wherever the user navigated to so OCMINFO
			// runs in that dir (PATH resolves the binary regardless).
			free(LB_MAX_ENTRIES * sizeof(LBEntry_t));
			restoreScreen();
			launchOcmInfo();
			dos2_exit(1);	// only reached if OCMINFO.COM not found
		}
	}

	// CWD is intentionally left wherever the user navigated. This means
	// returning to the local browser shows the same dir, and downloads
	// from network tabs land in that dir too.

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

	return exitCode;
}
