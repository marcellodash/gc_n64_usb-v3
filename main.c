/*	gc_n64_usb : Gamecube or N64 controller to USB adapter firmware
	Copyright (C) 2007-2015  Raphael Assenat <raph@raphnet.net>

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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/delay.h>

#include "main.h"
#include "util.h"
#include "usart1.h"
#include "usb.h"
#include "gamepads.h"
#include "bootloader.h"

#include "gcn64_protocol.h"
#include "n64.h"
#include "gamecube.h"
#include "usbpad.h"
#include "eeprom.h"
#include "hiddata.h"
#include "usbstrings.h"
#include "intervaltimer.h"
#include "requests.h"

#define GCN64_USB_PID	0x001D
#define N64_USB_PID		0x0020
#define GC_USB_PID		0x0021

/* Those .c files are included rather than linked for we
 * want the sizeof() operator to work on the arrays */
#include "reportdesc.c"
#include "dataHidReport.c"

#define MAX_READ_ERRORS	30

struct cfg0 {
	struct usb_configuration_descriptor configdesc;
	struct usb_interface_descriptor interface;
	struct usb_hid_descriptor hid;
	struct usb_endpoint_descriptor ep1_in;

	struct usb_interface_descriptor interface_admin;
	struct usb_hid_descriptor hid_data;
	struct usb_endpoint_descriptor ep2_in;
};

static const struct cfg0 cfg0 PROGMEM = {
	.configdesc = {
		.bLength = sizeof(struct usb_configuration_descriptor),
		.bDescriptorType = CONFIGURATION_DESCRIPTOR,
		.wTotalLength = sizeof(cfg0), // includes all descriptors returned together
		.bNumInterfaces = 2,
		.bConfigurationValue = 1,
		.bmAttributes = CFG_DESC_ATTR_RESERVED, // set Self-powred and remote-wakeup here if needed.
		.bMaxPower = 25, // for 50mA
	},

	// Main interface, HID
	.interface = {
		.bLength = sizeof(struct usb_interface_descriptor),
		.bDescriptorType = INTERFACE_DESCRIPTOR,
		.bInterfaceNumber = 0,
		.bAlternateSetting = 0,
		.bNumEndpoints = 1,
		.bInterfaceClass = USB_DEVICE_CLASS_HID,
		.bInterfaceSubClass = HID_SUBCLASS_NONE,
		.bInterfaceProtocol = HID_PROTOCOL_NONE,
	},
	.hid = {
		.bLength = sizeof(struct usb_hid_descriptor),
		.bDescriptorType = HID_DESCRIPTOR,
		.bcdHid = 0x0101,
		.bCountryCode = HID_COUNTRY_NOT_SUPPORTED,
		.bNumDescriptors = 1, // Only a report descriptor
		.bClassDescriptorType = REPORT_DESCRIPTOR,
		.wClassDescriptorLength = sizeof(gcn64_usbHidReportDescriptor),
	},
	.ep1_in = {
		.bLength = sizeof(struct usb_endpoint_descriptor),
		.bDescriptorType = ENDPOINT_DESCRIPTOR,
		.bEndpointAddress = USB_RQT_DEVICE_TO_HOST | 1, // 0x81
		.bmAttributes = TRANSFER_TYPE_INT,
		.wMaxPacketsize = 32,
		.bInterval = LS_FS_INTERVAL_MS(1),
	},

	// Second HID interface for config and update
	.interface_admin = {
		.bLength = sizeof(struct usb_interface_descriptor),
		.bDescriptorType = INTERFACE_DESCRIPTOR,
		.bInterfaceNumber = 1,
		.bAlternateSetting = 0,
		.bNumEndpoints = 1,
		.bInterfaceClass = USB_DEVICE_CLASS_HID,
		.bInterfaceSubClass = HID_SUBCLASS_NONE,
		.bInterfaceProtocol = HID_PROTOCOL_NONE,
	},
	.hid_data = {
		.bLength = sizeof(struct usb_hid_descriptor),
		.bDescriptorType = HID_DESCRIPTOR,
		.bcdHid = 0x0101,
		.bCountryCode = HID_COUNTRY_NOT_SUPPORTED,
		.bNumDescriptors = 1, // Only a report descriptor
		.bClassDescriptorType = REPORT_DESCRIPTOR,
		.wClassDescriptorLength = sizeof(dataHidReport),
	},
	.ep2_in = {
		.bLength = sizeof(struct usb_endpoint_descriptor),
		.bDescriptorType = ENDPOINT_DESCRIPTOR,
		.bEndpointAddress = USB_RQT_DEVICE_TO_HOST | 2, // 0x82
		.bmAttributes = TRANSFER_TYPE_INT,
		.wMaxPacketsize = 64,
		.bInterval = LS_FS_INTERVAL_MS(1),
	},

};

struct usb_device_descriptor device_descriptor = {
	.bLength = sizeof(struct usb_device_descriptor),
	.bDescriptorType = DEVICE_DESCRIPTOR,
	.bcdUSB = 0x0101,
	.bDeviceClass = 0, // set at interface
	.bDeviceSubClass = 0, // set at interface
	.bDeviceProtocol = 0,
	.bMaxPacketSize = 64,
	.idVendor = 0x289B,
	.idProduct = GCN64_USB_PID,
	.bcdDevice = 0x0300, // 1.0.0
	.bNumConfigurations = 1,
	.iManufacturer = 1,
	.iProduct = 2,
	.iSerialNumber = 3,
};

/** **/

static struct usb_parameters usb_params = {
	.flags = USB_PARAM_FLAG_CONFDESC_PROGMEM |
					USB_PARAM_FLAG_REPORTDESC_PROGMEM,
	.devdesc = (PGM_VOID_P)&device_descriptor,
	.configdesc = (PGM_VOID_P)&cfg0,
	.configdesc_ttllen = sizeof(cfg0),
	.num_strings = NUM_USB_STRINGS,
	.strings = g_usb_strings,

	.n_hid_interfaces = 2,
	.hid_params = {
		[0] = {
			.reportdesc = gcn64_usbHidReportDescriptor,
			.reportdesc_len = sizeof(gcn64_usbHidReportDescriptor),
			.getReport = usbpad_hid_get_report,
			.setReport = usbpad_hid_set_report,
		},
		[1] = {
			.reportdesc = dataHidReport,
			.reportdesc_len = sizeof(dataHidReport),
			.getReport = hiddata_get_report,
			.setReport = hiddata_set_report,
		},
	},
};

void hwinit(void)
{
	/* PORTB
	 *
	 * 7: NC    Output low
	 * 6: NC	Output low
	 * 5: NC	Output low
	 * 4: NC	Output low
	 * 3: MISO	Output low
	 * 2: MOSI	Output low
	 * 1: SCK	Output low
	 * 0: NC	Output low
	 */
	PORTB = 0x00;
	DDRB = 0xFF;

	/* PORTC
	 *
	 * 7: NC	Output low
	 * 6: NC	Output low
	 * 5: NC	Output low
	 * 4: NC	Output low
	 * 3: (no such pin)
	 * 2: NC	Output low
	 * 1: RESET	(N/A: Reset input per fuses) (left floating)
	 * 0: XTAL2	(N/A: Crystal oscillator) (left floating)
	 */
	DDRC = 0xfc;
	PORTC = 0x00;

	/* PORTD
	 *
	 * 7: HWB		Input (external pull-up)
	 * 6: NC		Output low
	 * 5: NC		Output low
	 * 4: NC		Output low
	 * 3: IO3_MCU	Input
	 * 2: IO2_MCU	Input
	 * 1: IO1_MCU	Input
	 * 0: IO0_MCU	Input
	 */
	PORTD = 0x00;
	DDRD = 0x70;

	// System clock. External crystal is 16 Mhz and we want
	// to run at max. speed.
	CLKPR = 0x80;
	CLKPR = 0x0; // Division factor of 1
	PRR0 = 0;
	PRR1 = 0;
}


#define NUM_PAD_TYPES	2

unsigned char current_pad_type = CONTROLLER_IS_ABSENT;

Gamepad *detectPad(void)
{
	current_pad_type = gcn64_detectController(GCN64_CHANNEL_0);

	switch (current_pad_type)
	{
		case CONTROLLER_IS_ABSENT:
		case CONTROLLER_IS_UNKNOWN:
			return NULL;

		case CONTROLLER_IS_N64:
			printf("Detected N64 controller\n");
			return n64GetGamepad();
			break;

		case CONTROLLER_IS_GC:
			printf("Detected GC controller\n");
			return gamecubeGetGamepad();
	}

	return NULL;
}

/* Called after eeprom content is loaded. */
void eeprom_app_ready(void)
{
	static wchar_t serial_from_eeprom[SERIAL_NUM_LEN+1];
	int i;

	for (i=0; i<SERIAL_NUM_LEN; i++) {
		serial_from_eeprom[i] = g_eeprom_data.cfg.serial[i];
	}
	serial_from_eeprom[i] = 0;
	g_usb_strings[USB_STRING_SERIAL_IDX] = serial_from_eeprom;
}

char g_polling_suspended = 0;

void pollDelay(void)
{
	int i;
	for (i=0; i<g_eeprom_data.cfg.poll_interval[0]; i++) {
		_delay_ms(1);
	}
}

#define STATE_WAIT_POLLTIME			0
#define STATE_POLL_PAD				1
#define STATE_WAIT_INTERRUPT_READY	2
#define STATE_TRANSMIT				3

int main(void)
{
	Gamepad *pad = NULL;
	gamepad_data pad_data;
	unsigned char gamepad_vibrate = 0;
	unsigned char state = STATE_WAIT_POLLTIME;
	int error_count=0;

	hwinit();
	usart1_init();
	eeprom_init();
	intervaltimer_init();

	usbpad_init();

	switch (g_eeprom_data.cfg.mode)
	{
		default:
		case CFG_MODE_STANDARD:
			break;

		case CFG_MODE_N64_ONLY:
			usbstrings_changeProductString(L"N64 to USB v"VERSIONSTR_SHORT);
			device_descriptor.idProduct = N64_USB_PID;
			break;

		case CFG_MODE_GC_ONLY:
			usbstrings_changeProductString(L"Gamecube to USB v"VERSIONSTR_SHORT);
			device_descriptor.idProduct = GC_USB_PID;
			break;
	}

	sei();
	usb_init(&usb_params);

	while (1)
	{
		static char last_v = 0;

		usb_doTasks();
		hiddata_doTask();

		switch(state)
		{
			case STATE_WAIT_POLLTIME:
				if (!g_polling_suspended) {
					intervaltimer_set(g_eeprom_data.cfg.poll_interval[0]);
					if (intervaltimer_get()) {
						state = STATE_POLL_PAD;
					}
				}
				break;

			case STATE_POLL_PAD:
				/* Try to auto-detect controller if none*/
				if (!pad) {
					pad = detectPad();
					if (pad && (pad->hotplug)) {
						// For gamecube, this make sure the next
						// analog values we read become the center
						// reference.
						pad->hotplug();
					}
				}
				if (pad) {
					if (pad->update()) {
						error_count++;
						if (error_count > MAX_READ_ERRORS) {
							pad = NULL;
							state = STATE_WAIT_POLLTIME;
							error_count = 0;
							break;
						}
					} else {
						error_count=0;
					}

					if (pad->changed()) {

						pad->getReport(&pad_data);
						usbpad_update(&pad_data);
						state = STATE_WAIT_INTERRUPT_READY;
						break;
					}
				} else {
					/* Just make sure the gamepad state holds valid data
					 * to appear inactive (no buttons and axes in neutral) */
					usbpad_update(NULL);
				}
				state = STATE_WAIT_POLLTIME;
				break;

			case STATE_WAIT_INTERRUPT_READY:
				if (usb_interruptReady()) {
					state = STATE_TRANSMIT;
				}
				break;

			case STATE_TRANSMIT:
				usb_interruptSend(usbpad_getReportBuffer(), usbpad_getReportSize());
				state = STATE_WAIT_POLLTIME;
				break;
		}

		gamepad_vibrate = usbpad_mustVibrate();
		if (last_v != gamepad_vibrate) {
			if (pad && pad->setVibration) {
				pad->setVibration(gamepad_vibrate);
			}
			last_v = gamepad_vibrate;
		}
	}

	return 0;
}
