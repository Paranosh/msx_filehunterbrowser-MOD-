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
#include "fh.h"

/* ------------------------------------------------------------ */
/* launchSofaRun()                                              */
/*                                                              */
/*   Inject "SR.COM /S" into the BIOS keyboard buffer and exit */
/*   fhMOD.com. COMMAND.COM will then pick it up from the       */
/*   buffer and execute it, letting MSX-DOS resolve SR.COM via */
/*   its own PATH lookup (far more reliable than our own).     */
/*                                                              */
/*   The caller (F5 handler) is expected to restoreScreen()     */
/*   before calling this. Never returns on success.            */
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
