/*
	mod_joystick.h
	Polls MSX joystick port 1 via BIOS GTSTCK / GTTRIG and translates
	directions and triggers into the same character codes the keyboard
	handlers in fhMOD already react to (KEY_UP, KEY_DOWN, KEY_LEFT,
	KEY_RIGHT, KEY_RETURN, KEY_ESC).
*/
#pragma once

#include <stdint.h>

/*
	Polls the joystick once. Returns a key constant if a direction or
	trigger has transitioned to "active" since the previous call (with
	a slow auto-repeat when a direction is held), or 0 if there is no
	new event. Designed to be called in the same wait loop as kbhit():

	    while (!kbhit() && !(key = joystickPoll())) { ASM_EI; ASM_HALT; }
	    if (!key) key = dos2_toupper(getch());

	Mapping:
	    direction up / up-left / up-right    -> KEY_UP
	    direction down / down-left / down-r  -> KEY_DOWN
	    direction left                       -> KEY_LEFT
	    direction right                      -> KEY_RIGHT
	    trigger A press                      -> KEY_RETURN
	    trigger B press                      -> KEY_ESC
*/
uint8_t joystickPoll(void);
