/*
    mod_serverSelect.c
    Reads REPOS.TXT (same directory as FH.COM) to configure the server URL
    and optionally enable directory-navigation mode.

    REPOS.TXT format:
        Line 1 – Base URL with %s placeholders for type/msx/char parameters.
        Line 2 – (optional) Write "DIRMODE" to enable server-side directory
                 navigation (the |path syntax).  Omit or write anything else
                 to keep the browser in plain text-search mode (compatible
                 with the original NataliaPC File-Hunter server).

    Examples:
        # Original NataliaPC server (text search only):
        http://original-server.com/index3.php?...

        # Extended server with directory navigation:
        http://extended-server.com/index4.php?base=1BA0&type=%s&msx=%s&char=%s&download=
        DIRMODE

    If REPOS.TXT is absent the built-in default URL is used and DIRMODE is
    enabled (current behaviour, server supports directory navigation).
*/

#include <string.h>
#include "dos.h"
#include "fh.h"

static char urlBuf[128];
static char lineBuf[16];

void selectServer(void)
{
    FILEH fh;
    char *p;

    if (!dos2_fileexists("REPOS.TXT")) return;

    fh = dos2_fopen("REPOS.TXT", 0x01);
    if (fh > 20) return;

    // Line 1: base URL
    if (dos2_fgets(urlBuf, sizeof(urlBuf), fh)) {
        p = urlBuf;
        while (*p && *p != '\r' && *p != '\n') p++;
        *p = '\0';

        if (urlBuf[0]) {
            BASEURL = (const char *)urlBuf;
        }
    }

    // A custom server defaults to text-search-only mode; only enable
    // directory navigation if the second line explicitly says "DIRMODE".
    serverHasDirMode = false;

    // Line 2 (optional): "DIRMODE" flag
    if (dos2_fgets(lineBuf, sizeof(lineBuf), fh)) {
        p = lineBuf;
        while (*p && *p != '\r' && *p != '\n') p++;
        *p = '\0';
        if (strcmp(lineBuf, "DIRMODE") == 0) {
            serverHasDirMode = true;
        }
    }

    dos2_fclose(fh);
}
