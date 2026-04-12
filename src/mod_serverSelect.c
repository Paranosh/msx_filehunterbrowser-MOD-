/*
    mod_serverSelect.c
    Pantalla de selección de servidor al inicio de la aplicación.
    Permite elegir entre File-Hunter (file-hunter.com) o un servidor
    personalizado (p.ej. el NAS propio).

    Copyright (c) 2025
*/

#include <string.h>
#include "conio.h"
#include "conio_aux.h"
#include "fh.h"

// ── Constantes de URL ────────────────────────────────────────────────────────

#define FILEHUNTER_URL \
    "http://api.file-hunter.com/index4.php?base=1BA0&type=%s&msx=%s&char=%s&download="

// NOTA: los %% se convierten en % tras el csprintf, dejando los %s para
// que formatURL() los sustituya más tarde en el bucle principal.
#define CUSTOM_URL_FMT \
    "http://%s/index4.php?base=1BA0&type=%%s&msx=%%s&char=%%s&download="

// ── Dimensiones del cuadro de selección ─────────────────────────────────────
// Pantalla: 80 cols x 24 filas.  Área de contenido: filas 5-22.
#define BOX_X   12
#define BOX_Y    7
#define BOX_W   56
#define BOX_H   10

// ── Funciones internas ───────────────────────────────────────────────────────

// Dibuja un rectángulo simple con esquinas + y lados - / |
static void drawBox(void)
{
    uint8_t i;
    char line[BOX_W + 1];

    // Línea superior
    line[0] = '+';
    for (i = 1; i < BOX_W - 1; i++) line[i] = '-';
    line[BOX_W - 1] = '+';
    line[BOX_W] = '\0';
    gotoxy(BOX_X, BOX_Y);
    cputs(line);

    // Línea inferior
    gotoxy(BOX_X, BOX_Y + BOX_H - 1);
    cputs(line);

    // Laterales
    for (i = 1; i < BOX_H - 1; i++) {
        gotoxy(BOX_X, BOX_Y + i);
        putch('|');
        gotoxy(BOX_X + BOX_W - 1, BOX_Y + i);
        putch('|');
    }
}

// Escribe texto centrado dentro del cuadro en la fila indicada
static void putCentered(uint8_t row, const char *str)
{
    uint8_t len = strlen(str);
    uint8_t x   = BOX_X + 1 + (BOX_W - 2 - len) / 2;
    gotoxy(x, row);
    cputs(str);
}

// Borra el interior del cuadro (rellena con espacios)
static void clearBox(void)
{
    uint8_t i;
    char blank[BOX_W - 1];
    uint8_t innerW = BOX_W - 2;

    for (i = 0; i < innerW; i++) blank[i] = ' ';
    blank[innerW] = '\0';

    for (i = 1; i < BOX_H - 1; i++) {
        gotoxy(BOX_X + 1, BOX_Y + i);
        cputs(blank);
    }
}

// Lee una cadena desde teclado, con eco y soporte de borrado (Backspace/DEL)
static void readString(char *buf, uint8_t maxLen)
{
    uint8_t i = 0;
    uint8_t c;
    buf[0] = '\0';

    while (1) {
        c = getch();
        if (c == 13) break;                          // Enter -> fin
        if ((c == 8 || c == 127) && i > 0) {        // Backspace / DEL
            i--;
            buf[i] = '\0';
            cputs("\x08 \x08");
        } else if (c >= 32 && c < 127 && i < maxLen - 1) {
            buf[i++] = c;
            buf[i]   = '\0';
            putch(c);
        }
    }
}

// ── Función pública ──────────────────────────────────────────────────────────

void selectServer(void)
{
    uint8_t c;
    char hostBuf[48];

    // Dibujar cuadro y menú principal
    drawBox();
    putCentered(BOX_Y + 1, "FH Browser - Seleccion de servidor");
    putCentered(BOX_Y + 3, "[F]  File-Hunter  (file-hunter.com)");
    putCentered(BOX_Y + 4, "[C]  Servidor personalizado");
    putCentered(BOX_Y + 6, "ENTER = File-Hunter por defecto");
    putCentered(BOX_Y + 8, "-> ");

    // Esperar tecla válida
    do {
        c = getch();
        if (c >= 'a' && c <= 'z') c -= 32;  // a minúscula -> mayúscula
    } while (c != 'F' && c != 'C' && c != 13);

    if (c == 'C') {
        // ── Servidor personalizado ─────────────────────────────────────────
        clearBox();
        putCentered(BOX_Y + 1, "Servidor personalizado");
        putCentered(BOX_Y + 3, "Introduce host:puerto");
        putCentered(BOX_Y + 4, "Ejemplo: 192.168.0.251:8580");

        gotoxy(BOX_X + 3, BOX_Y + 6);
        cputs("> ");
        readString(hostBuf, sizeof(hostBuf));

        if (hostBuf[0] != '\0') {
            // Construir URL completa con el host introducido
            csprintf(BASEURL, CUSTOM_URL_FMT, hostBuf);
            return;
        }
        // Sin input -> usar File-Hunter
    }

    // ── File-Hunter (por defecto) ──────────────────────────────────────────
    strcpy(BASEURL, FILEHUNTER_URL);
}
