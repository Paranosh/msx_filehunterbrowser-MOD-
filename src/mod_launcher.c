/*
	Copyright (c) 2025 Natalia Pujol Cremades
	info@abitwitches.com

	See LICENSE file.
*/
#include <stdint.h>
#include <string.h>
#include "dos.h"

/*
 * Page-zero layout (safe: never overwritten when a .COM loads at 0x0100)
 *   0x0080  cmdtail: written by launchSofaRunASM (3, " /S", CR)
 *   0x0085  stub code: copied here by launchSofaRunASM
 *   0x00C0  FNAME_ADDR: ASCIIZ full path to SR.COM, written here by us
 */
#define FNAME_ADDR  ((char *)0x00C0)
#define FNAME_MAX   58       /* leaves room between 0xC0 and 0xFF */

/* Defined in mod_launcher_stub.s — copies stub to page-zero and JP 0x0085 */
void launchSofaRunASM(void);

/* ------------------------------------------------------------ */
/* launchSofaRun()                                              */
/*   1. Resolves SR.COM via current directory then PATH.        */
/*   2. Writes the full path to page-zero FNAME_ADDR (0x00C0). */
/*   3. Calls launchSofaRunASM() which sets up cmdtail, copies  */
/*      a tiny stub to 0x0085, and jumps into it.              */
/*   Never returns on success. Returns silently if SR.COM is   */
/*   not found so the caller can clean up / exit.              */
/* ------------------------------------------------------------ */
void launchSofaRun(void)
{
	static char pathbuf[128];   /* PATH env value — static: not on stack */
	char *fname = FNAME_ADDR;
	uint8_t len;
	char *p, *seg;

	/* 1. Try bare filename (searches current directory) */
	strcpy(fname, "SR.COM");
	if (dos2_fileexists(fname)) {
		launchSofaRunASM();
		return;
	}

	/* 2. Walk the PATH environment variable */
	if (dos2_getEnv("PATH", pathbuf, (uint8_t)sizeof(pathbuf))) {
		return;   /* env lookup failed — SR.COM not found */
	}

	p = pathbuf;
	while (*p) {
		seg = p;
		while (*p && *p != ';') ++p;
		len = (uint8_t)(p - seg);

		if (len > 0 && len < (uint8_t)(FNAME_MAX - 7 /* "SR.COM\0" */)) {
			memcpy(fname, seg, len);
			/* Ensure trailing backslash */
			if (fname[len - 1] != '\\' && fname[len - 1] != '/') {
				fname[len++] = '\\';
			}
			strcpy(fname + len, "SR.COM");
			if (dos2_fileexists(fname)) {
				launchSofaRunASM();
				return;
			}
		}

		if (*p == ';') ++p;
	}
	/* SR.COM not found anywhere — return silently */
}
