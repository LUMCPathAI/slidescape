/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2024  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#pragma once
#include "common.h"
#define WIN32_GRAPHICAL_APP_IMPL
#include "graphical_app.h"
#include "win32_graphical_app.h"


// Sources:
// https://stackoverflow.com/questions/291424/canonical-way-to-parse-the-command-line-into-arguments-in-plain-c-windows-api
// https://github.com/futurist/CommandLineToArgvA

/*************************************************************************
 * CommandLineToArgvA            [SHELL32.@]
 *
 * MODIFIED FROM https://www.winehq.org/ project
 * We must interpret the quotes in the command line to rebuild the argv
 * array correctly:
 * - arguments are separated by spaces or tabs
 * - quotes serve as optional argument delimiters
 *   '"a b"'   -> 'a b'
 * - escaped quotes must be converted back to '"'
 *   '\"'      -> '"'
 * - consecutive backslashes preceding a quote see their number halved with
 *   the remainder escaping the quote:
 *   2n   backslashes + quote -> n backslashes + quote as an argument delimiter
 *   2n+1 backslashes + quote -> n backslashes + literal quote
 * - backslashes that are not followed by a quote are copied literally:
 *   'a\b'     -> 'a\b'
 *   'a\\b'    -> 'a\\b'
 * - in quoted strings, consecutive quotes see their number divided by three
 *   with the remainder modulo 3 deciding whether to close the string or not.
 *   Note that the opening quote must be counted in the consecutive quotes,
 *   that's the (1+) below:
 *   (1+) 3n   quotes -> n quotes
 *   (1+) 3n+1 quotes -> n quotes plus closes the quoted string
 *   (1+) 3n+2 quotes -> n+1 quotes plus closes the quoted string
 * - in unquoted strings, the first quote opens the quoted string and the
 *   remaining consecutive quotes follow the above rule.
 */

LPSTR* WINAPI CommandLineToArgvA(LPSTR lpCmdline, int* numargs)
{
	DWORD argc;
	LPSTR  *argv;
	LPSTR s;
	LPSTR d;
	LPSTR cmdline;
	int qcount,bcount;

	if(!numargs || *lpCmdline==0)
	{
		SetLastError(ERROR_INVALID_PARAMETER);
		return NULL;
	}

	/* --- First count the arguments */
	argc=1;
	s=lpCmdline;
	/* The first argument, the executable path, follows special rules */
	if (*s=='"')
	{
		/* The executable path ends at the next quote, no matter what */
		s++;
		while (*s)
			if (*s++=='"')
				break;
	}
	else
	{
		/* The executable path ends at the next space, no matter what */
		while (*s && *s!=' ' && *s!='\t')
			s++;
	}
	/* skip to the first argument, if any */
	while (*s==' ' || *s=='\t')
		s++;
	if (*s)
		argc++;

	/* Analyze the remaining arguments */
	qcount=bcount=0;
	while (*s)
	{
		if ((*s==' ' || *s=='\t') && qcount==0)
		{
			/* skip to the next argument and count it if any */
			while (*s==' ' || *s=='\t')
				s++;
			if (*s)
				argc++;
			bcount=0;
		}
		else if (*s=='\\')
		{
			/* '\', count them */
			bcount++;
			s++;
		}
		else if (*s=='"')
		{
			/* '"' */
			if ((bcount & 1)==0)
				qcount++; /* unescaped '"' */
			s++;
			bcount=0;
			/* consecutive quotes, see comment in copying code below */
			while (*s=='"')
			{
				qcount++;
				s++;
			}
			qcount=qcount % 3;
			if (qcount==2)
				qcount=0;
		}
		else
		{
			/* a regular character */
			bcount=0;
			s++;
		}
	}

	/* Allocate in a single lump, the string array, and the strings that go
	 * with it. This way the caller can make a single LocalFree() call to free
	 * both, as per MSDN.
	 */
	argv=(LPSTR*)LocalAlloc(LMEM_FIXED, (argc+1)*sizeof(LPSTR)+(strlen(lpCmdline)+1)*sizeof(char));
	if (!argv)
		return NULL;
	cmdline=(LPSTR)(argv+argc+1);
	strcpy(cmdline, lpCmdline);

	/* --- Then split and copy the arguments */
	argv[0]=d=cmdline;
	argc=1;
	/* The first argument, the executable path, follows special rules */
	if (*d=='"')
	{
		/* The executable path ends at the next quote, no matter what */
		s=d+1;
		while (*s)
		{
			if (*s=='"')
			{
				s++;
				break;
			}
			*d++=*s++;
		}
	}
	else
	{
		/* The executable path ends at the next space, no matter what */
		while (*d && *d!=' ' && *d!='\t')
			d++;
		s=d;
		if (*s)
			s++;
	}
	/* close the executable path */
	*d++=0;
	/* skip to the first argument and initialize it if any */
	while (*s==' ' || *s=='\t')
		s++;
	if (!*s)
	{
		/* There are no parameters so we are all done */
		argv[argc]=NULL;
		*numargs=argc;
		return argv;
	}

	/* Split and copy the remaining arguments */
	argv[argc++]=d;
	qcount=bcount=0;
	while (*s)
	{
		if ((*s==' ' || *s=='\t') && qcount==0)
		{
			/* close the argument */
			*d++=0;
			bcount=0;

			/* skip to the next one and initialize it if any */
			do {
				s++;
			} while (*s==' ' || *s=='\t');
			if (*s)
				argv[argc++]=d;
		}
		else if (*s=='\\')
		{
			*d++=*s++;
			bcount++;
		}
		else if (*s=='"')
		{
			if ((bcount & 1)==0)
			{
				/* Preceded by an even number of '\', this is half that
				 * number of '\', plus a quote which we erase.
				 */
				d-=bcount/2;
				qcount++;
			}
			else
			{
				/* Preceded by an odd number of '\', this is half that
				 * number of '\' followed by a '"'
				 */
				d=d-bcount/2-1;
				*d++='"';
			}
			s++;
			bcount=0;
			/* Now count the number of consecutive quotes. Note that qcount
			 * already takes into account the opening quote if any, as well as
			 * the quote that lead us here.
			 */
			while (*s=='"')
			{
				if (++qcount==3)
				{
					*d++='"';
					qcount=0;
				}
				s++;
			}
			if (qcount==2)
				qcount=0;
		}
		else
		{
			/* a regular character */
			*d++=*s++;
			bcount=0;
		}
	}
	*d='\0';
	argv[argc]=NULL;
	*numargs=argc;

	return argv;
}

u8* platform_alloc(size_t size) {
	u8* result = (u8*) VirtualAlloc(0, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	if (!result) {
		console_print("Error: memory allocation failed!\n");
		fatal_error();
	}
	return result;
}

void message_box(window_handle_t window, const char* message) {
	MessageBoxA(window, message, APP_TITLE, MB_ICONERROR);
}

// Window related procecures

void set_window_title(window_handle_t window, const char* title) {
	size_t len = strlen(title) + 1;
	wchar_t* wide_title = win32_string_widen(title, len, (wchar_t*) alloca(2 * len));
	SetWindowTextW(window, wide_title);
}

void reset_window_title(window_handle_t window) {
	SetWindowTextA(window, APP_TITLE);
}

POINT stored_mouse_pos;

void mouse_hide() {
	if (!cursor_hidden && !gui_want_capture_mouse) {
		GetCursorPos(&stored_mouse_pos);
		ShowCursor(0);
		cursor_hidden = true;
	}
}

void mouse_show() {
	if (cursor_hidden) {
		// TODO: mouse move asymptotically to the window corners?
//		SetCursorPos(stored_mouse_pos.x, stored_mouse_pos.y);
		ShowCursor(1);
		cursor_hidden = false;
	}
}

void update_cursor() {
	if (!gui_user_can_resize_at_window_edge) {
		SetCursor(global_current_cursor);
	}
}

void set_cursor_default() {
	global_current_cursor = global_cursor_arrow;
	update_cursor();
}

void set_cursor_crosshair() {
	global_current_cursor = global_cursor_crosshair;
	update_cursor();
}

const char* get_default_save_directory() {
	return "";
};

