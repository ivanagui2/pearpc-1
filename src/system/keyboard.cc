/* 
 *	PearPC
 *	keyboard.cc
 *
 *	Copyright (C) 2004 Stefan Weyergraf
 *	Copyright (C) 2003,2004 Sebastian Biallas (sb@biallas.net)
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License version 2 as
 *	published by the Free Software Foundation.
 *
 *	This program is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *	GNU General Public License for more details.
 *
 *	You should have received a copy of the GNU General Public License
 *	along with this program; if not, write to the Free Software
 *	Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "tools/snprintf.h"
#include "keyboard.h"
#include "display.h"

SystemKeyboard *gKeyboard = NULL;

static char *key_names[] = {
"A","S","D","F","H","G","Z","X","C","V",
NULL,"B","Q","W","E","R","Y","T","1","2",
"3","4","6","5","=","9","7","-","8","0","]",
"O","U","[","I","P","Return","L","J","'","K",
";","\\",",","/","N","M",".","Tab","Space",
"`", "Backspace",NULL,"Escape","Ctrl","Alt","Shift","Caps-Lock","Right-Alt","Left",
"Right","Down","Up",NULL,NULL,"Keypad-.",NULL,"Keypad-*",NULL,"Keypad-+",
NULL,"Numlock",NULL,NULL,NULL,"Keypad-/","Keypad-Enter",NULL,"Keypad--",NULL,
NULL,NULL,"Keypad-0","Keypad-1","Keypad-2","Keypad-3","Keypad-4","Keypad-5","Keypad-6","Keypad-7",
NULL,"Keypad-8","Keypad-9",NULL,NULL,NULL,"F5","F6","F7","F3",
"F8","F9",NULL,"F11",NULL,"F13",NULL,"Scrolllock",NULL,"F10",
NULL,"F12",NULL,"Pause","Insert","Home","Pageup","Delete","F4","End",
"F2","Pagedown","F1",
};

SystemKeyboard::SystemKeyboard()
{
	reset();
}

bool SystemKeyboard::handleEvent(const SystemEvent &ev)
{
	int keycode;

	if (ev.type != sysevKey) return false;

	switch(ev.key.keycode) {
	case KEY_CONTROL: 
		mCtrl = ev.key.pressed ? KEYCODE_CTRL : 0;
		keycode = KEY_UNDEF;
		break;
	case KEY_ALT:
		mLAlt = ev.key.pressed ? KEYCODE_LALT : 0;
		keycode = KEY_UNDEF;
		break;
	case KEY_ALTGR:
		mRAlt = ev.key.pressed ? KEYCODE_RALT : 0;
		keycode = KEY_UNDEF;
		break;
	case KEY_SHIFT:
		mShift = ev.key.pressed ? KEYCODE_SHIFT : 0;
		keycode = KEY_UNDEF;
		break;
	default:
		keycode = ev.key.keycode;
		break;
	}

//	ht_printf("KEYS: %3d%c -> %c%c%c%c + %3d\n", ev.key.keycode, ev.key.pressed? 'd': 'u', mCtrl? 'C': '.', mRAlt? 'A': '.', mLAlt? 'A': '.', mShift? 'S': '.', keycode);
	keycode = keycode | mCtrl | mRAlt | mLAlt | mShift;

	if (keycode == keyConfig.key_toggle_mouse_grab) {
		if (ev.key.pressed) gDisplay->setMouseGrab(!gDisplay->isMouseGrabbed());
		return true;
	} else if (keycode == keyConfig.key_toggle_full_screen) {
		if (ev.key.pressed) gDisplay->setFullscreenMode(!gDisplay->mFullscreen);
		/*
		 *	Changing fullscreen/windowed mode confuses
		 *	some well-known OS (key release events are dropped).
		 */
		reset();
		return true;
	} else if (keycode == keyConfig.key_compose_dialog) {
		if (ev.key.pressed) gDisplay->composeKeyDialog();
		return true;
	} else {
		return SystemDevice::handleEvent(ev);
	}
}

void SystemKeyboard::reset()
{
	mCtrl = false;
	mLAlt = false;
	mRAlt = false;
	mShift = false;
}

bool SystemKeyboard::convertKeycodeToString(String &result, int keycode)
{
	int first = 1;
	if (((keycode & KEY_MASK) != KEY_UNDEF) && (!key_names[keycode & KEY_MASK])) return false;
	if (keycode & ~(KEY_MASK|KEYCODE_CTRL|KEYCODE_LALT|KEYCODE_RALT|KEYCODE_SHIFT)) return false;
	result = "";
	if (keycode & KEYCODE_CTRL) {
		if(!first)
			result += "+";
		first = 0;
		result += key_names[KEY_CONTROL];
	}
	if (keycode & KEYCODE_LALT) {
		if(!first)
			result += "+";
		first = 0;
		result += key_names[KEY_ALT];
	}
	if (keycode & KEYCODE_RALT) {
		if(!first)
			result += "+";
		first = 0;
		result += key_names[KEY_ALTGR];
	}
	if (keycode & KEYCODE_SHIFT) {
		if(!first)
			result += "+";
		first = 0;
		result += key_names[KEY_SHIFT];
	}
	if((keycode & KEY_MASK) != KEY_UNDEF) {
		if(!first)
			result += "+";
		first = 0;
		result += key_names[keycode & KEY_MASK];
	}
	return true;
}

bool SystemKeyboard::convertStringToKeycode(int &keycode, const String &s)
{
	if (s == "none") {
		keycode = KEY_NONE;
		return true;
	}
	String k = s;
	bool cont = true;
	keycode = KEY_UNDEF;
	while (cont) {
		String key, rem;
		cont = k.leftSplit('+', key, rem);
		k = rem;

		int found = -1;
		for (uint i=0; i < (sizeof key_names / sizeof key_names[0]); i++) {
			if (key_names[i] && key == (String)key_names[i]) found = i;
		}
		switch (found) {
		case -1:
			ht_printf("%y not found\n", &key);
			return false;
		case KEY_CONTROL:
			if (keycode & KEYCODE_CTRL) return false;
			keycode |= KEYCODE_CTRL;
			break;
		case KEY_ALT:
			if (keycode & KEYCODE_LALT) return false;
			keycode |= KEYCODE_LALT;
			break;
		case KEY_ALTGR:
			if (keycode & KEYCODE_RALT) return false;
			keycode |= KEYCODE_RALT;
			break;
		case KEY_SHIFT:
			if (keycode & KEYCODE_SHIFT) return false;
			keycode |= KEYCODE_SHIFT;
			break;
		default:
			if ((keycode & KEY_MASK) != KEY_UNDEF) return false;
			keycode = (keycode & (~KEY_MASK)) | found;
			break;
		}
	}
	return true;
}

/* FIXME: need proper keymaps */
static char lchrs[0x7f] = {
'a','s','d','f','h','g','z','x','c','v',
0,'b','q','w','e','r','y','t','1','2',
'3','4','6','5','=','9','7','-','8','0',']',
'o','u','[','i','p',13,'l','j','\'','k',
';','\\',',','/','n','m','.',9,32,
'`',8,0,27,0,0,0,0,0,0,
0,0,0,0,0,'.',0,'*',0,'+',
0,0,0,0,0,'/',13,0,'-',0,
0,0,'0','1','2','3','4','5','6','7',
0,'8','9',0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,
0,0,0,
};

static char uchrs[0x7f] = {
'A','S','D','F','H','G','Z','X','C','V',
0,'B','Q','W','E','R','Y','T','!','@',
'#','$','^','%','+','(','&','_','*',')','}',
'O','U','{','I','P',13,'L','J','"','K',
':','|','<','?','N','M','>',9,32,
'~',8,0,27,0,0,0,0,0,0,
0,0,0,0,0,'.',0,'*',0,'+',
0,0,0,0,0,'/',13,0,'-',0,
0,0,'0','1','2','3','4','5','6','7',
0,'8','9',0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,
0,0,0,
};

bool SystemKeyboard::adbKeyToAscii(char &chr, int adbcode)
{
	adbcode &= 0x7f;

	if (mShift)
		chr = uchrs[adbcode];
	else
		chr = lchrs[adbcode];

	return chr != 0;
}

bool SystemKeyboard::setKeyConfig(KeyboardCharacteristics keycon)
{
	keyConfig=keycon;
	return true;
}

KeyboardCharacteristics &SystemKeyboard::getKeyConfig()
{
	return keyConfig;
}
