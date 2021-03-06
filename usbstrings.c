/*	gc_n64_usb : Gamecube or N64 controller to USB firmware
	Copyright (C) 2007-2016  Raphael Assenat <raph@raphnet.net>

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include <stdlib.h> // for wchar_t
#include "usbstrings.h"

static wchar_t product_string[PRODUCT_STRING_MAXCHARS]; // = L"GC/N64 to USB v"VERSIONSTR_SHORT;

const wchar_t *g_usb_strings[] = {
	[0] = L"raphnet technologies", 	// 1 : Vendor
	[1] = product_string,	// 2: Product
	[2] = L"123456",				// 3 : Serial
};

void usbstrings_changeProductString_P(const char *str)
{
	const char *s = str;
	wchar_t *d = product_string;
	uint8_t c;
	int n = 0;

	do {
		/* Make sure target is always NUL terminated. */
		n++;
		if (n == PRODUCT_STRING_MAXCHARS) {
			*d = 0;
			break;
		}

		c = pgm_read_byte(s);

		*d = c;
		s++; d++;
	} while (c);
}
