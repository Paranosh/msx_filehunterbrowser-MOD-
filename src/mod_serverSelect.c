/*
    mod_serverSelect.c
    Builds the server list from REPOS.TXT (same directory as fhMOD.com).

    REPOS.TXT format — one server per line:
        DisplayName|BaseURL
        DisplayName|BaseURL|DIRMODE

    DisplayName : up to 23 characters shown in the F4 selector dialog.
    BaseURL     : URL with %s placeholders for type/msx/char parameters.
    DIRMODE     : optional; enables server-side directory navigation
                  (the |path syntax).  Omit for plain text-search mode.

    Lines starting with '#' or empty lines are ignored.

    Examples:
        # Original NataliaPC server (text search only):
        NataliaPC original|http://original-server.com/index3.php?...

        # Extended server with directory navigation:
        My server|http://extended-server.com/index4.php?base=1BA0&type=%s&msx=%s&char=%s&download=|DIRMODE

    If REPOS.TXT is absent the built-in default server is the only entry.
    The built-in server is ALWAYS available as entry [0] regardless of
    whether REPOS.TXT exists or what it contains.
*/

#include <string.h>
#include "dos.h"
#include "fh.h"

// URL storage pool for custom servers (built-in uses the BASEURL const directly)
#define URL_POOL_SIZE  896   // 128 bytes * 7 custom server slots

static char   urlPool[URL_POOL_SIZE];
static uint16_t urlPoolPos;

// Line read buffer: must hold  Name(23) + '|' + URL(127) + '|' + "DIRMODE"(7) + LF + NUL
static char lineBuf[165];

// ---- Exported globals (declared in fh.h) ----
ServerEntry_t serverList[MAX_SERVERS];
uint8_t       serverCount  = 0;
uint8_t       currentServer = 0;


void selectServer(void)
{
    FILEH  fh;
    char  *p, *name, *url, *flag;
    uint16_t ulen;
    bool   dm;

    // ---- Entry 0: built-in default (always present) ----
    strncpy(serverList[0].name, "api.file-hunter.com", SERVER_NAME_MAXLEN - 1);
    serverList[0].name[SERVER_NAME_MAXLEN - 1] = '\0';
    serverList[0].url     = BASEURL;  // points to the const in fhMOD.c
    serverList[0].dirMode = true;
    serverCount   = 1;
    currentServer = 0;
    urlPoolPos    = 0;

    if (!dos2_fileexists("REPOS.TXT")) return;

    fh = dos2_fopen("REPOS.TXT", 0x01);
    if (fh > 20) return;

    while (serverCount < MAX_SERVERS &&
           dos2_fgets(lineBuf, sizeof(lineBuf), fh)) {

        // Strip trailing CR / LF
        p = lineBuf;
        while (*p && *p != '\r' && *p != '\n') p++;
        *p = '\0';

        // Skip blank lines and comment lines
        if (!lineBuf[0] || lineBuf[0] == '#') continue;

        // Require Name|URL  separator
        name = lineBuf;
        url  = strchr(lineBuf, '|');
        if (!url) continue;   // old-format or malformed — skip
        *url++ = '\0';

        // Optional  |DIRMODE  suffix
        flag = strchr(url, '|');
        dm   = false;
        if (flag) {
            *flag++ = '\0';
            // strip any trailing whitespace/CR/LF from the flag word
            p = flag;
            while (*p && *p != '\r' && *p != '\n' && *p != ' ') p++;
            *p = '\0';
            dm = (strcmp(flag, "DIRMODE") == 0);
        }

        // Skip entries with empty URL
        if (!url[0]) continue;

        // Truncate URL if it is unreasonably long
        ulen = strlen(url);
        if (ulen >= 128) continue;

        // No room left in the URL pool
        if (urlPoolPos + ulen + 1 > URL_POOL_SIZE) break;

        // Copy name (truncate to fit)
        strncpy(serverList[serverCount].name, name, SERVER_NAME_MAXLEN - 1);
        serverList[serverCount].name[SERVER_NAME_MAXLEN - 1] = '\0';

        // Store URL in the pool
        strcpy(urlPool + urlPoolPos, url);
        serverList[serverCount].url     = urlPool + urlPoolPos;
        serverList[serverCount].dirMode = dm;
        urlPoolPos += ulen + 1;

        serverCount++;
    }

    dos2_fclose(fh);
}
