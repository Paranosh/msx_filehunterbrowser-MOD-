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

    If REPOS.TXT is absent or has no valid entries, the built-in server
    is used as the sole entry.

    When REPOS.TXT has valid entries they REPLACE the built-in: only the
    entries from the file appear in the F4 selector.  The first entry
    becomes the active server at startup.
*/

#include <string.h>
#include "dos.h"
#include "fh.h"

// URL storage pool for custom servers
#define URL_POOL_SIZE  896   // 128 bytes x 7 entries

static char     urlPool[URL_POOL_SIZE];
static uint16_t urlPoolPos;

// Line read buffer:  Name(23) + '|' + URL(127) + '|' + "DIRMODE"(7) + CRLF + NUL
static char lineBuf[165];

// ---- Exported globals (declared in fh.h) ----
ServerEntry_t serverList[MAX_SERVERS];
uint8_t       serverCount  = 0;
uint8_t       currentServer = 0;


// ---- Set serverList[0] to the compiled-in built-in server ----
static void useBuiltIn(void)
{
    strncpy(serverList[0].name, "api.file-hunter.com", SERVER_NAME_MAXLEN - 1);
    serverList[0].name[SERVER_NAME_MAXLEN - 1] = '\0';
    serverList[0].url     = BASEURL;
    serverList[0].dirMode = true;
    serverCount    = 1;
    currentServer  = 0;
    // Keep the global pointers in sync with the active entry
    BASEURL          = serverList[0].url;
    serverHasDirMode = serverList[0].dirMode;
}


void selectServer(void)
{
    FILEH    fh;
    char    *p, *name, *url, *flag;
    uint16_t ulen;
    bool     dm;

    serverCount  = 0;
    currentServer = 0;
    urlPoolPos   = 0;

    if (!dos2_fileexists("REPOS.TXT")) {
        useBuiltIn();
        return;
    }

    fh = dos2_fopen("REPOS.TXT", 0x01);
    if (fh > 20) {
        useBuiltIn();
        return;
    }

    // Parse every valid line from REPOS.TXT
    while (serverCount < MAX_SERVERS &&
           dos2_fgets(lineBuf, sizeof(lineBuf), fh)) {

        // Strip trailing CR / LF
        p = lineBuf;
        while (*p && *p != '\r' && *p != '\n') p++;
        *p = '\0';

        // Skip blank lines and comment lines
        if (!lineBuf[0] || lineBuf[0] == '#') continue;

        // Require Name|URL separator
        name = lineBuf;
        url  = strchr(lineBuf, '|');
        if (!url) continue;
        *url++ = '\0';

        // Optional |DIRMODE suffix
        flag = strchr(url, '|');
        dm   = false;
        if (flag) {
            *flag++ = '\0';
            p = flag;
            while (*p && *p != '\r' && *p != '\n' && *p != ' ') p++;
            *p = '\0';
            dm = (strcmp(flag, "DIRMODE") == 0);
        }

        // Skip entries with empty or over-long URL
        if (!url[0]) continue;
        ulen = strlen(url);
        if (ulen >= 128) continue;

        // No room left in the URL pool
        if (urlPoolPos + ulen + 1 > URL_POOL_SIZE) break;

        // Copy name (truncate to fit the display field)
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

    // If no valid entries were found, fall back to the built-in server
    if (serverCount == 0) {
        useBuiltIn();
        return;
    }

    // Apply the first entry as the active server at startup
    BASEURL          = serverList[0].url;
    serverHasDirMode = serverList[0].dirMode;
}
