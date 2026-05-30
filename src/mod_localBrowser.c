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

// Last (drive, path) the user navigated to inside the local browser,
// remembered between Local-browser sessions so jumping out to a
// network tab and back lands them at the same place. 0xFF means
// "never visited yet — use whatever CWD the process happens to have".
// Size matches dos2_getCurrentDirectory's 64-byte spec.
static uint8_t lb_savedDrive    = 0xFF;
static char    lb_savedPath[64] = "";

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
// Message shown on the DOS console while the external launcher
// (SROM / SRI) takes over. Printed after restoreScreen() so it stays
// visible during the load. Kept neutral so it works regardless of
// which launcher actually picks up.
static const char lb_sofaRunMsg[] =
	"\r\n"
	"  Launching, please wait...\r\n"
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

#define LOADCAX_DEST_NAME	"LOADCAX"
// LOADCAX source and the residual-file manifest are both expected to
// live next to fhMOD.com itself. We resolve their paths at runtime
// with getProgramPath() (same trick mod_serverSelect uses for
// REPOS.TXT) so they work regardless of which drive fhMOD was
// installed on and regardless of the local browser's current dir.
// LB_PATH_BUFLEN is sized for a Nextor MAX_PATH_SIZE + small filename.
#define LB_PATH_BUFLEN		80
static char lb_loadcaxSrcPath[LB_PATH_BUFLEN];
static char lb_manifestPath  [LB_PATH_BUFLEN];

// Matches the BUFF_SIZE used to malloc 'buff' in fhMOD.c. Keep in sync.
#define LB_BUFF_SIZE		200

// Build "<fhMOD's dir>\<filename>" into `out` (LB_PATH_BUFLEN bytes).
// Falls back to just the bare filename if getProgramPath fails (which
// it does on MSX-DOS 1), in which case operations on `out` go through
// the current drive's working directory — same fallback REPOS.TXT
// uses.
static void lb_buildProgramRelativePath(char *out, const char *filename)
{
	char progPath[64];
	char *slash;
	uint8_t dirLen;
	uint8_t fnLen = (uint8_t)strlen(filename);

	if (!getProgramPath(progPath)) {
		strncpy(out, filename, LB_PATH_BUFLEN - 1);
		out[LB_PATH_BUFLEN - 1] = '\0';
		return;
	}
	slash = strrchr(progPath, '\\');
	if (!slash) {
		strncpy(out, filename, LB_PATH_BUFLEN - 1);
		out[LB_PATH_BUFLEN - 1] = '\0';
		return;
	}
	dirLen = (uint8_t)((slash - progPath) + 1);	/* include trailing '\' */
	if ((uint16_t)dirLen + fnLen + 1 > LB_PATH_BUFLEN) {
		strncpy(out, filename, LB_PATH_BUFLEN - 1);
		out[LB_PATH_BUFLEN - 1] = '\0';
		return;
	}
	memcpy(out, progPath, dirLen);
	strcpy(out + dirLen, filename);
}

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

	fhSrc = dos2_fopen(lb_loadcaxSrcPath, O_RDONLY);
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
// Previously this used a 1 KB static buffer per cleanup function. Two
// of those (one here, one in lb_cleanupResiduals) pushed BSS over the
// 0x8000 boundary that fhMOD uses as the heap floor, corrupting hget's
// TCP buffers and breaking network operations. Now both functions
// reuse the existing 200 B 'buff' global so BSS stays the same as
// upstream fhMOD's. As a side effect the manifest is capped at what
// fits in 'buff' (~2 entries per launch); the next run cleans those
// and any further launches keep adding fresh ones, so multi-launch
// sessions get tidied incrementally rather than all at once.
static void lb_appendManifest(const char *filename)
{
	char        absPath[80];
	char        curPath[64];
	uint8_t     drive;
	uint16_t    used = 0;
	uint16_t    n;
	uint16_t    pl;
	FILEH       fh;

	/* Build "<drive>:\<cwd>\<filename>". */
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

	/* Slurp whatever fits of the existing manifest into 'buff'. */
	fh = dos2_fopen(lb_manifestPath, O_RDONLY);
	if (fh < ERR_FIRST) {
		used = (uint16_t)dos2_fread(buff, LB_BUFF_SIZE - 1, fh);
		dos2_fclose(fh);
		if (used >= LB_BUFF_SIZE) used = LB_BUFF_SIZE - 1;
	}

	/* Append "<absPath>\r\n". Bail if it doesn't fit; the entry will be
	   picked up by the cleanup of a later run instead. */
	pl = (uint16_t)strlen(absPath);
	if (used + pl + 2 >= LB_BUFF_SIZE) return;
	memcpy(buff + used, absPath, pl); used += pl;
	buff[used++] = '\r';
	buff[used++] = '\n';

	dos2_remove(lb_manifestPath);
	fh = dos2_fcreate(lb_manifestPath, O_WRONLY, ATTR_ARCHIVE);
	if (fh >= ERR_FIRST) return;
	dos2_fwrite(buff, used, fh);
	dos2_fclose(fh);
}

void lb_cleanupResiduals(void)
{
	char        path[80];
	uint16_t    used;
	uint16_t    i;
	uint16_t    lineStart;
	uint16_t    len;
	FILEH       fh;

	/* Resolve LOADCAX + manifest paths from fhMOD's own program path.
	   Done here (called once at startup from main()) rather than in a
	   separate init function. */
	lb_buildProgramRelativePath(lb_loadcaxSrcPath, LOADCAX_DEST_NAME);
	lb_buildProgramRelativePath(lb_manifestPath,   "FHCLEAN.LST");

	fh = dos2_fopen(lb_manifestPath, O_RDONLY);
	if (fh >= ERR_FIRST) return;
	used = (uint16_t)dos2_fread(buff, LB_BUFF_SIZE - 1, fh);
	dos2_fclose(fh);
	if (used >= LB_BUFF_SIZE) used = LB_BUFF_SIZE - 1;
	buff[used] = '\0';

	lineStart = 0;
	for (i = 0; i <= used; i++) {
		if (i == used || buff[i] == '\r' || buff[i] == '\n') {
			len = i - lineStart;
			if (len > 0 && len < sizeof(path)) {
				memcpy(path, buff + lineStart, len);
				path[len] = '\0';
				dos2_remove(path);
			}
			while (i < used && (buff[i] == '\r' || buff[i] == '\n'))
				i++;
			lineStart = i;
			if (i < used) i--;
		}
	}

	dos2_remove(lb_manifestPath);
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

// Centred cursor-driven menu used by lb_confirmExit, lb_chooseRomLauncher
// and anything else that needs a "pick one of N labels" popup. Mirrors
// the look + controls of the F4 Server Browser:
//
//   UP/DOWN   move selection (joystick too — joystickPoll() polled here)
//   ENTER     return the selected index
//   ESC       return -1 (cancel)
//
// Width is auto-fit to the title or longest option, capped to a 26-col
// minimum so short prompts still look like dialogs.
static int8_t lb_pickFromMenu(const char *title,
                               const char *const opts[],
                               uint8_t count)
{
	uint8_t i;
	uint8_t labelLen;
	uint8_t maxW = (uint8_t)strlen(title);
	uint8_t winW;
	uint8_t innerW;
	uint8_t x1, x2, y1, y2;
	uint8_t totalRows;
	uint8_t firstItemY;
	int8_t  cur = 0;
	char    key;
	uint8_t y;

	for (i = 0; i < count; i++) {
		labelLen = (uint8_t)strlen(opts[i]);
		if (labelLen > maxW) maxW = labelLen;
	}
	winW = maxW + 8;	/* 4-col padding each side */
	if (winW < 26) winW = 26;

	x1 = (uint8_t)((80 - winW) / 2) + 1;
	x2 = x1 + winW - 1;
	/* top border + title + blank + count items + bottom border */
	totalRows = count + 4;
	y1 = (uint8_t)(12 - totalRows / 2);
	y2 = y1 + totalRows - 1;
	firstItemY = y1 + 3;
	innerW = winW - 4;

	/* Wipe interior and draw the frame using the same chars as everything
	   else in the app. fillBlink(false) clears any leftover blink so the
	   only blinking cells will be the textblink we set per row. */
	for (y = y1 + 1; y < y2; y++) {
		_fillVRAM((uint16_t)((y - 1) * 80 + (x1 - 1)),
		          (uint16_t)winW, ' ');
	}
	fillBlink(x1, y1, totalRows, winW, false);
	drawFrame(x1, y1, x2, y2);

	/* Title, centred on row y1+1. */
	{
		uint8_t titleLen = (uint8_t)strlen(title);
		uint8_t tx = x1 + (uint8_t)((winW - titleLen) / 2);
		putstrxy(tx, y1 + 1, (char*)title);
	}

	/* Items: each centred inside innerW, with blink on the selected one. */
	for (i = 0; i < count; i++) {
		labelLen = (uint8_t)strlen(opts[i]);
		if (labelLen > innerW) labelLen = innerW;
		memset(buff, ' ', innerW);
		buff[innerW] = '\0';
		memcpy(buff + (innerW - labelLen) / 2, opts[i], labelLen);
		putlinexy(x1 + 2, firstItemY + i, innerW, buff);
		textblink(x1 + 2, firstItemY + i, innerW, (i == cur));
	}

	while (kbhit()) getch();
	for (;;) {
		uint8_t joyKey;
		ASM_EI; ASM_HALT;
		joyKey = joystickPoll();
		if (!kbhit() && !joyKey) continue;
		key = joyKey ? (char)joyKey : dos2_toupper(getch());

		if (key == KEY_UP) {
			if (cur > 0) {
				textblink(x1 + 2, firstItemY + cur, innerW, false);
				cur--;
				textblink(x1 + 2, firstItemY + cur, innerW, true);
			}
		} else if (key == KEY_DOWN) {
			if ((uint8_t)(cur + 1) < count) {
				textblink(x1 + 2, firstItemY + cur, innerW, false);
				cur++;
				textblink(x1 + 2, firstItemY + cur, innerW, true);
			}
		} else if (key == KEY_RETURN || key == KEY_SELECT) {
			return cur;
		} else if (key == KEY_ESC) {
			return -1;
		}
	}
}

// "Exit fhMOD?" confirmation popup. Yes/No cursor menu. Returns true
// only if the user picked Yes; ESC or No both return false. Caller
// repaints whatever was underneath.
bool lb_confirmExit(void)
{
	static const char *const opts[] = { "Yes", "No" };
	int8_t r = lb_pickFromMenu("Exit fhMOD?", opts, 2);
	return (r == 0);
}

// Centred error / info popup. Shows up to three message lines and a
// "Press any key..." footer; blocks until the user acknowledges.
// Used to surface "X.COM not found" conditions before they cause a
// silent failure inside COMMAND.COM after fhMOD exits.
static void lb_showError(const char *line1, const char *line2, const char *line3)
{
	uint8_t maxW;
	uint8_t winW;
	uint8_t x1, x2, y1, y2;
	uint8_t totalRows;
	uint8_t lines;
	uint8_t row;
	uint8_t y;
	uint8_t len;
	const char *footer = "Press any key...";

	maxW = (uint8_t)strlen(line1);
	if (line2) { len = (uint8_t)strlen(line2); if (len > maxW) maxW = len; }
	if (line3) { len = (uint8_t)strlen(line3); if (len > maxW) maxW = len; }
	len = (uint8_t)strlen(footer);
	if (len > maxW) maxW = len;

	winW = maxW + 8;	/* 4-col padding each side */
	if (winW < 30) winW = 30;

	lines = 1 + (line2 ? 1 : 0) + (line3 ? 1 : 0);
	/* top + lines + blank + footer + bottom */
	totalRows = lines + 4;

	x1 = (uint8_t)((80 - winW) / 2) + 1;
	x2 = x1 + winW - 1;
	y1 = (uint8_t)(12 - totalRows / 2);
	y2 = y1 + totalRows - 1;

	for (y = y1 + 1; y < y2; y++) {
		_fillVRAM((uint16_t)((y - 1) * 80 + (x1 - 1)),
		          (uint16_t)winW, ' ');
	}
	fillBlink(x1, y1, totalRows, winW, false);
	drawFrame(x1, y1, x2, y2);

	row = y1 + 1;
	len = (uint8_t)strlen(line1);
	putstrxy(x1 + (uint8_t)((winW - len) / 2), row++, (char*)line1);
	if (line2) {
		len = (uint8_t)strlen(line2);
		putstrxy(x1 + (uint8_t)((winW - len) / 2), row++, (char*)line2);
	}
	if (line3) {
		len = (uint8_t)strlen(line3);
		putstrxy(x1 + (uint8_t)((winW - len) / 2), row++, (char*)line3);
	}
	row++;	/* blank line */
	len = (uint8_t)strlen(footer);
	putstrxy(x1 + (uint8_t)((winW - len) / 2), row, (char*)footer);

	while (kbhit()) getch();
	while (!kbhit()) { ASM_EI; ASM_HALT; }
	getch();
}

// Check whether an external tool (LOADCAX, SROM.COM, …) sits next to
// fhMOD.com. Always returns the resolved path in `outPath` so callers
// can include it in an error popup. Pass NULL for outPath if not
// interested in the path.
static bool lb_toolExists(const char *toolName, char *outPath)
{
	char  scratch[LB_PATH_BUFLEN];
	char *target = outPath ? outPath : scratch;
	lb_buildProgramRelativePath(target, toolName);
	return dos2_fileexists(target);
}

// Pop up the standard "<tool> not found" message and wait for ack.
static void lb_warnMissingTool(const char *toolName,
                                const char *expectedPath,
                                bool        fatal)
{
	static char line1[40];
	static char line2[80];
	csprintf(line1, "%s not found", toolName);
	csprintf(line2, "%s", expectedPath);
	if (fatal) {
		lb_showError(line1, line2, "Cannot continue.");
	} else {
		lb_showError(line1, line2, "Make sure it is on PATH.");
	}
}

// Centred popup that asks the user which launcher to use for a .ROM.
// Cursor-driven menu (same controls as the F4 Server Browser).
// Returns:
//   'S' = SofaROM (SROM) — uses fhMOD's Linear/Linear0/LinearC mapper
//                          detection so the right /Rx flag is passed.
//   'M' = mglOcm        — closed-source ToughkidCST OCM-native loader.
//                          Has its own mapper auto-detect; no flags
//                          needed. Must be on the DOS PATH.
//    0  = ESC or Cancel — caller restores list and returns to browser.
static char lb_chooseRomLauncher(void)
{
	static const char *const opts[] = {
		"SROM (SofaROM)",
		"mglOcm",
		"Cancel"
	};
	int8_t r = lb_pickFromMenu("Launch ROM with:", opts, 3);
	if (r == 0) return 'S';
	if (r == 1) return 'M';
	return 0;	/* -1 (ESC) or 2 (Cancel) */
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
		/* Verify the chosen launcher binary is somewhere reachable.
		   We check next to fhMOD.com first (most common install), and
		   issue a warning popup (non-blocking) if absent — the file
		   might still be on PATH elsewhere, so we proceed anyway. */
		{
			char path[LB_PATH_BUFLEN];
			const char *toolName = (launcher == 'M') ? "mglOcm.com" : "SROM.COM";
			if (!lb_toolExists(toolName, path)) {
				lb_warnMissingTool(toolName, path, false);
			}
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
		/* Quick SRI presence check — non-blocking warning. */
		{
			char path[LB_PATH_BUFLEN];
			if (!lb_toolExists("SRI.COM", path)) {
				lb_warnMissingTool("SRI.COM", path, false);
			}
		}
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
		/* LOADCAX is mandatory for .CAS launching — without it the
		   later BASIC BLOAD will fail and the user will be stuck in
		   BASIC. Fail FAST and tell them why, instead of letting it
		   crash later. */
		if (!dos2_fileexists(lb_loadcaxSrcPath)) {
			lb_warnMissingTool("LOADCAX", lb_loadcaxSrcPath, true);
			return 0;
		}
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

	// Restore the last (drive, path) we remembered from a previous
	// Local-browser session. This works even if something else (UNAPI,
	// hget, BDOS side-effects from drive enumeration, …) clobbered
	// CWD while we were on a network tab. First entry of the process
	// has lb_savedDrive == 0xFF and we just use whatever CWD is.
	if (lb_savedDrive != 0xFF) {
		lb_selectDrive(lb_savedDrive);
		if (lb_savedPath[0]) {
			buff[0] = '\\';
			strcpy(buff + 1, lb_savedPath);
			dos2_setCurrentDirectory(buff);
		} else {
			dos2_setCurrentDirectory("\\");
		}
	}

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

	// Remember the (drive, path) we're leaving on so the next time the
	// user opens the local browser they land back in the same place,
	// even if CWD got changed in the meantime. CWD is also left where
	// the user navigated so downloads from network tabs ideally land
	// here too — but the saved (drive, path) is the source of truth.
	lb_savedDrive = getCurrentDrive();
	dos2_getCurrentDirectory(0, lb_savedPath);

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
