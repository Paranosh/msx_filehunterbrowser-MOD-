/*
	mod_joystick.c — see mod_joystick.h.
*/
#include <stdint.h>
#include "msx_const.h"
#include "conio.h"
#include "mod_joystick.h"

/* Slow-repeat counter for held directions. Tuned with the HALT loop
   pulsing once per VDP interrupt (~60 Hz NTSC / 50 Hz PAL): 5 ticks
   ≈ 80–100 ms between repeats — fast enough to feel responsive but
   slow enough to avoid skipping list entries. */
#define JOY_REPEAT_TICKS	5

/* GTSTCK BIOS call (0x00D5). A = stick number (1 = joystick port 1).
   Returns A = direction (0..8, 0 = no input, clockwise from up). */
static uint8_t bios_gtstck(uint8_t stick) __naked __sdcccall(1)
{
	stick;
	__asm
		ld   ix, #0x00D5
		ld   iy, (#EXPTBL-1)
		call CALSLT
		ld   l, a
		ret
	__endasm;
}

/* GTTRIG BIOS call (0x00D8). A = trigger:
       0 = space bar
       1 = joy1 trigger A
       2 = joy2 trigger A
       3 = joy1 trigger B
       4 = joy2 trigger B
   Returns A = 0 (released) or 0xFF (pressed). */
static uint8_t bios_gttrig(uint8_t trig) __naked __sdcccall(1)
{
	trig;
	__asm
		ld   ix, #0x00D8
		ld   iy, (#EXPTBL-1)
		call CALSLT
		ld   l, a
		ret
	__endasm;
}

/* Map a stick direction (1..8) to one of the four cursor key codes.
   Diagonals collapse to vertical so menu navigation feels predictable. */
static uint8_t dirToKey(uint8_t dir)
{
	switch (dir) {
		case 1: case 2: case 8: return KEY_UP;
		case 3:                  return KEY_RIGHT;
		case 4: case 5: case 6: return KEY_DOWN;
		case 7:                  return KEY_LEFT;
		default:                 return 0;
	}
}

uint8_t joystickPoll(void)
{
	static uint8_t prevDir   = 0;
	static uint8_t prevTrigA = 0;
	static uint8_t prevTrigB = 0;
	static uint8_t holdCount = 0;

	uint8_t dir   = bios_gtstck(1);
	uint8_t trigA = bios_gttrig(1);
	uint8_t trigB = bios_gttrig(3);

	/* Trigger A press transition -> ENTER */
	if (trigA && !prevTrigA) { prevTrigA = trigA; return KEY_RETURN; }
	prevTrigA = trigA;

	/* Trigger B press transition -> ESC */
	if (trigB && !prevTrigB) { prevTrigB = trigB; return KEY_ESC; }
	prevTrigB = trigB;

	/* Direction transition fires immediately, then auto-repeats
	   every JOY_REPEAT_TICKS HALT-ticks while held. */
	if (dir != prevDir) {
		prevDir   = dir;
		holdCount = 0;
		return dirToKey(dir);
	}
	if (dir != 0) {
		if (++holdCount >= JOY_REPEAT_TICKS) {
			holdCount = 0;
			return dirToKey(dir);
		}
	}
	return 0;
}
