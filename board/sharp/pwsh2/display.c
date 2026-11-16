// SPDX-License-Identifier: GPL-2.0+

#include <common.h>
#include <env.h>
#include <stdio_dev.h>
#include <time.h>
#include <video_fb.h>
#include "../common/wince/brain.h"

#define LANDSCAPE_WIDTH		800
#define LANDSCAPE_HEIGHT	480
#define PORTRAIT_WIDTH		LANDSCAPE_HEIGHT
#define PORTRAIT_HEIGHT		LANDSCAPE_WIDTH
#define CONSOLE_SCALE		2
#define CONSOLE_WIDTH		(LANDSCAPE_WIDTH / CONSOLE_SCALE)
#define CONSOLE_HEIGHT		(LANDSCAPE_HEIGHT / CONSOLE_SCALE)
#define CONSOLE_BYTES		(CONSOLE_WIDTH * CONSOLE_HEIGHT * 2)
#define TEXT_COLOUR		0xffff
#define DIM_SPLASH(pixel)	(((pixel) >> 1) & 0x7bef)
#define LANDSCAPE_BUFFER	0x44000000
#define EBOOT_SPLASH		0x40100000
#define REFRESH_INTERVAL_MS	40

static GraphicDevice landscape_display;
static struct stdio_dev *console_device;
static bool display_dirty;
static unsigned long last_refresh;
static bool display_held = true;

void *video_hw_init(void)
{
	memset((void *)LANDSCAPE_BUFFER, 0, CONSOLE_BYTES);
	landscape_display.frameAdrs = LANDSCAPE_BUFFER;
	landscape_display.memSize = CONSOLE_BYTES;
	landscape_display.winSizeX = CONSOLE_WIDTH;
	landscape_display.winSizeY = CONSOLE_HEIGHT;
	landscape_display.plnSizeX = CONSOLE_WIDTH;
	landscape_display.plnSizeY = CONSOLE_HEIGHT;
	landscape_display.gdfBytesPP = 2;
	landscape_display.gdfIndex = GDF_16BIT_565RGB;
	strcpy(landscape_display.modeIdent, "PW-SH2 panel as EBOOT left it");
	display_dirty = true;
	return &landscape_display;
}

void brain_display_hold(bool hold)
{
	display_held = hold;
	display_dirty = true;
}

static void rotate_to_portrait(void)
{
	const u16 *console = (const u16 *)LANDSCAPE_BUFFER;
	const u16 *splash = (const u16 *)EBOOT_SPLASH;
	u16 *portrait = (u16 *)BRAIN_PORTRAIT_BUFFER;
	unsigned int portrait_x, portrait_y, console_x, console_y;

	for (portrait_y = 0; portrait_y < PORTRAIT_HEIGHT; portrait_y++) {
		console_x = portrait_y / CONSOLE_SCALE;
		for (portrait_x = 0; portrait_x < PORTRAIT_WIDTH; portrait_x++, splash++) {
			console_y = (PORTRAIT_WIDTH - 1 - portrait_x) / CONSOLE_SCALE;
			*portrait++ = console[console_y * CONSOLE_WIDTH + console_x] ? TEXT_COLOUR : DIM_SPLASH(*splash);
		}
	}
}

void brain_display_refresh(void)
{
	if (display_held || !display_dirty || get_timer(last_refresh) < REFRESH_INTERVAL_MS)
		return;
	display_dirty = false;
	last_refresh = get_timer(0);
	rotate_to_portrait();
	brain_portrait_push(PORTRAIT_HEIGHT, PORTRAIT_WIDTH);
}

static void display_putc(struct stdio_dev *device, const char character)
{
	console_device->putc(console_device, character);
	display_dirty = true;
}

static void display_puts(struct stdio_dev *device, const char *text)
{
	console_device->puts(console_device, text);
	display_dirty = true;
}

int brain_display_init(void)
{
	struct stdio_dev device;

	console_device = stdio_get_by_name("vga");
	if (!console_device)
		return -ENODEV;
	memset(&device, 0, sizeof(device));
	strcpy(device.name, "brainlcd");
	device.flags = DEV_FLAGS_OUTPUT;
	device.putc = display_putc;
	device.puts = display_puts;
	if (stdio_register(&device))
		return -EIO;
	env_set("stdout", "serial,brainlcd");
	env_set("stderr", "serial,brainlcd");
	return 0;
}
