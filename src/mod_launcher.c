/*
	Copyright (c) 2025 Natalia Pujol Cremades
	info@abitwitches.com

	See LICENSE file.
*/
#include <stdint.h>
#include <string.h>
#include "msx_const.h"
#include "dos.h"
#include "conio.h"
#include "utils.h"
#include "fh.h"

/* ------------------------------------------------------------ */
/* launchSofaRun()                                              */
/*                                                              */
/*   Inject "SR.COM /S" into the BIOS keyboard buffer and exit */
/*   fhMOD.com. COMMAND.COM will then pick it up from the       */
/*   buffer and execute it, letting MSX-DOS resolve SR.COM via */
/*   its own PATH lookup (far more reliable than our own).     */
/*                                                              */
/*   The caller (F5 handler) is expected to call paintSplash()  */
/*   before this so the user sees a loading screen instead of   */
/*   the COMMAND.COM prompt while SR.COM loads.                 */
/*   Never returns on success.                                  */
/* ------------------------------------------------------------ */

/* BIOS keyboard circular buffer */
#define KEYBUF_START	((uint16_t)KEYBUF)
#define KEYBUF_END		((uint16_t)(KEYBUF + 40 - 1))

static void injectCommand(const char *cmd)
{
	uint16_t putpnt;

	/* Flush any pending keypresses so the buffer is empty */
	while (kbhit()) getch();

	putpnt = varPUTPNT;

	while (*cmd) {
		*((char*)putpnt) = *cmd++;
		if (++putpnt > KEYBUF_END) putpnt = KEYBUF_START;
	}
	/* Append CR so COMMAND.COM treats it as a complete line */
	*((char*)putpnt) = '\r';
	if (++putpnt > KEYBUF_END) putpnt = KEYBUF_START;

	varPUTPNT = putpnt;
}

void launchSofaRun(void)
{
	injectCommand("SR.COM /S");
	dos2_exit(0);
}

/* ------------------------------------------------------------ */
/* paintSplash()                                                */
/*                                                              */
/*   Replacement for restoreScreen() to be used right before    */
/*   launchSofaRun() / lb_execCommand(). It performs the same   */
/*   cleanup (network lib, function keys, abort routine, screen */
/*   mode) and then paints a "fhMOD - Loading..." splash that   */
/*   stays visible while COMMAND.COM is digesting the queued    */
/*   command and SR.COM (or SROM/SRI) hasn't taken over yet.    */
/*                                                              */
/*   Also hides the text cursor so the COMMAND.COM prompt and   */
/*   the echoed command line don't show on top of the splash.   */
/* ------------------------------------------------------------ */
void paintSplash(void)
{
	uint8_t  cols;
	uint8_t  cx;
	uint8_t  cy;
	volatile uint8_t *csrsw = (volatile uint8_t*)CSRSW;

	/* Get a clean baseline: original screen mode + colors + abort routine */
	restoreScreen();

	/* Apply splash palette (white on dark blue) */
	varFORCLR = 15;
	varBAKCLR = 4;
	varBDRCLR = 4;

	__asm
		ld   ix, #CHGCLR
		BIOSCALL
	__endasm;

	/* Clear the screen so the new background colour fills it */
	__asm
		xor  a
		ld   ix, #CLSSCR
		BIOSCALL
	__endasm;

	/* Hide cursor so the COMMAND.COM prompt + echoed command line   */
	/* don't show on top of the splash banner.                       */
	*csrsw = 0;

	/* Centre the 24-char banner box on either 40-col or 32-col mode. */
	cols = varLINL40;
	if (cols >= 40)      cx = 9;	/* (40 - 24) / 2 + 1 */
	else if (cols >= 32) cx = 5;	/* (32 - 24) / 2 + 1 */
	else                 cx = 1;
	cy = 9;

	putstrxy(cx, cy,     "+----------------------+");
	putstrxy(cx, cy + 1, "|                      |");
	putstrxy(cx, cy + 2, "|        fhMOD         |");
	putstrxy(cx, cy + 3, "|                      |");
	putstrxy(cx, cy + 4, "|     Loading...       |");
	putstrxy(cx, cy + 5, "|                      |");
	putstrxy(cx, cy + 6, "+----------------------+");
}
