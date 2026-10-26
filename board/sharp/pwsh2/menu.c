// SPDX-License-Identifier: GPL-2.0+

#include <common.h>
#include <command.h>
#include <console.h>
#include <env.h>
#include <time.h>
#include <video_font.h>
#include <linux/delay.h>
#include "../common/wince/brain.h"
#include "menu_tux.h"

#define CANVAS_WIDTH		800
#define CANVAS_HEIGHT		480
#define CANVAS_BUFFER		0x44200000
#define EBOOT_SPLASH		0x40100000
#define SPLASH_WIDTH		480

#define TILE_SIZE		88
#define TILE_RADIUS		16
#define TILE_TOP		320
#define TILE_SPACING		180
#define FIRST_TILE_CENTRE	220
#define HIGHLIGHT_WIDTH		4
#define LABEL_TOP		(TILE_TOP + TILE_SIZE + 8)
#define LABEL_SCALE		2
#define COUNTDOWN_TOP		458
#define PROGRESS_LEFT		250
#define PROGRESS_WIDTH		300
#define PROGRESS_TOP		458
#define PROGRESS_HEIGHT		12

#define COLOUR_BLACK		0x0000
#define COLOUR_WHITE		0xffff
#define COLOUR_TILE		0x0000
#define COLOUR_TERMINAL_TEXT	0x07e0
#define COLOUR_WINDOWS_RED	0xf2a2
#define COLOUR_WINDOWS_GREEN	0x7dc0
#define COLOUR_WINDOWS_BLUE	0x051e
#define COLOUR_WINDOWS_YELLOW	0xfde0

#define KEY_NONE		0
#define KEY_LEFT		1
#define KEY_RIGHT		2
#define KEY_SELECT		3

struct menu_entry {
	const char *label;
	const char *command;
};

static const struct menu_entry menu_entries[] = {
	{ "Windows CE", "bootsharpce" },
	{ "Linux", "run linux_boot" },
	{ "U-Boot", NULL },
};

static u16 *canvas = (u16 *)CANVAS_BUFFER;
static int menu_selected;
static bool menu_shown;

static void put_pixel(int x, int y, u16 colour)
{
	if (x >= 0 && x < CANVAS_WIDTH && y >= 0 && y < CANVAS_HEIGHT)
		canvas[y * CANVAS_WIDTH + x] = colour;
}

static void draw_splash(void)
{
	const u16 *splash = (const u16 *)EBOOT_SPLASH;
	int x, y;

	for (y = 0; y < CANVAS_HEIGHT; y++)
		for (x = 0; x < CANVAS_WIDTH; x++)
			canvas[y * CANVAS_WIDTH + x] = splash[x * SPLASH_WIDTH + (SPLASH_WIDTH - 1 - y)];
}

static bool inside_rounded_rectangle(int x, int y, int left, int top, int size, int radius)
{
	int corner_x, corner_y, distance_x, distance_y;

	if (x < left || y < top || x >= left + size || y >= top + size)
		return false;
	corner_x = x < left + radius ? left + radius : (x >= left + size - radius ? left + size - radius - 1 : x);
	corner_y = y < top + radius ? top + radius : (y >= top + size - radius ? top + size - radius - 1 : y);
	distance_x = x - corner_x;
	distance_y = y - corner_y;
	return distance_x * distance_x + distance_y * distance_y <= radius * radius;
}

static void fill_rounded_rectangle(int left, int top, int size, int radius, u16 colour)
{
	int x, y;

	for (y = top; y < top + size; y++)
		for (x = left; x < left + size; x++)
			if (inside_rounded_rectangle(x, y, left, top, size, radius))
				put_pixel(x, y, colour);
}

static void fill_rectangle(int left, int top, int width, int height, u16 colour)
{
	int x, y;

	for (y = top; y < top + height; y++)
		for (x = left; x < left + width; x++)
			put_pixel(x, y, colour);
}

static void draw_text(const char *text, int left, int top, int scale, u16 colour)
{
	int column, row, scale_x, scale_y;

	for (; *text; text++, left += VIDEO_FONT_WIDTH * scale) {
		const unsigned char *glyph = &video_fontdata[(unsigned char)*text * VIDEO_FONT_HEIGHT];

		for (row = 0; row < VIDEO_FONT_HEIGHT; row++)
			for (column = 0; column < VIDEO_FONT_WIDTH; column++)
				if (glyph[row] & (0x80 >> column))
					for (scale_y = 0; scale_y < scale; scale_y++)
						for (scale_x = 0; scale_x < scale; scale_x++)
							put_pixel(left + column * scale + scale_x, top + row * scale + scale_y, colour);
	}
}

static void draw_centred_text(const char *text, int centre, int top, int scale, u16 colour)
{
	draw_text(text, centre - (int)strlen(text) * VIDEO_FONT_WIDTH * scale / 2, top, scale, colour);
}

static void draw_windows_icon(int left, int top)
{
	const int pane = 30;
	const int gap = 4;
	const int origin_x = left + (TILE_SIZE - 2 * pane - gap) / 2;
	const int origin_y = top + (TILE_SIZE - 2 * pane - gap) / 2;

	fill_rectangle(origin_x, origin_y, pane, pane, COLOUR_WINDOWS_RED);
	fill_rectangle(origin_x + pane + gap, origin_y, pane, pane, COLOUR_WINDOWS_GREEN);
	fill_rectangle(origin_x, origin_y + pane + gap, pane, pane, COLOUR_WINDOWS_BLUE);
	fill_rectangle(origin_x + pane + gap, origin_y + pane + gap, pane, pane, COLOUR_WINDOWS_YELLOW);
}

static void draw_tux_icon(int left, int top)
{
	const int origin_x = left + (TILE_SIZE - TUX_WIDTH) / 2;
	const int origin_y = top + (TILE_SIZE - TUX_HEIGHT) / 2;
	int x, y;

	for (y = 0; y < TUX_HEIGHT; y++)
		for (x = 0; x < TUX_WIDTH; x++)
			put_pixel(origin_x + x, origin_y + y, tux_pixels[y * TUX_WIDTH + x]);
}

static void draw_terminal_icon(int left, int top)
{
	draw_text(">_", left + TILE_SIZE / 2 - VIDEO_FONT_WIDTH * 3, top + TILE_SIZE / 2 - VIDEO_FONT_HEIGHT * 3 / 2,
		  3, COLOUR_TERMINAL_TEXT);
}

static void draw_menu_body(int selected)
{
	int index;

	draw_splash();
	for (index = 0; index < ARRAY_SIZE(menu_entries); index++) {
		int centre = FIRST_TILE_CENTRE + index * TILE_SPACING;
		int left = centre - TILE_SIZE / 2;

		if (index == selected)
			fill_rounded_rectangle(left - HIGHLIGHT_WIDTH, TILE_TOP - HIGHLIGHT_WIDTH,
					       TILE_SIZE + 2 * HIGHLIGHT_WIDTH, TILE_RADIUS + HIGHLIGHT_WIDTH, COLOUR_WHITE);
		fill_rounded_rectangle(left, TILE_TOP, TILE_SIZE, TILE_RADIUS, COLOUR_TILE);
		if (index == 0)
			draw_windows_icon(left, TILE_TOP);
		else if (index == 1)
			draw_tux_icon(left, TILE_TOP);
		else
			draw_terminal_icon(left, TILE_TOP);
		draw_centred_text(menu_entries[index].label, centre, LABEL_TOP, LABEL_SCALE, COLOUR_BLACK);
	}
	menu_selected = selected;
	menu_shown = true;
}

static void draw_menu(int selected, int seconds_left)
{
	char countdown[48];

	draw_menu_body(selected);
	if (seconds_left >= 0) {
		snprintf(countdown, sizeof(countdown), "Starting %s in %d", menu_entries[selected].label, seconds_left);
		draw_centred_text(countdown, CANVAS_WIDTH / 2, COUNTDOWN_TOP, 1, COLOUR_BLACK);
	}
	brain_display_show(canvas);
}

void brain_menu_progress(unsigned int done, unsigned int total)
{
	int filled;

	if (!menu_shown || !total)
		return;
	if (!done) {
		draw_menu_body(menu_selected);
		brain_display_show(canvas);
	}
	filled = (int)((u64)PROGRESS_WIDTH * done / total);
	fill_rectangle(PROGRESS_LEFT - 2, PROGRESS_TOP - 2, PROGRESS_WIDTH + 4, PROGRESS_HEIGHT + 4, COLOUR_BLACK);
	fill_rectangle(PROGRESS_LEFT, PROGRESS_TOP, PROGRESS_WIDTH, PROGRESS_HEIGHT, COLOUR_WHITE);
	fill_rectangle(PROGRESS_LEFT, PROGRESS_TOP, filled, PROGRESS_HEIGHT, COLOUR_WINDOWS_BLUE);
	brain_display_show_region(canvas, PROGRESS_LEFT - 2, PROGRESS_TOP - 2, PROGRESS_WIDTH + 4, PROGRESS_HEIGHT + 4);
}

static int read_key(void)
{
	unsigned long started;
	int character;

	if (!tstc())
		return KEY_NONE;
	character = getchar();
	if (character == '\r' || character == '\n' || character == ' ')
		return KEY_SELECT;
	if (character != 0x1b)
		return KEY_NONE;
	started = get_timer(0);
	while (!tstc() && get_timer(started) < 50)
		;
	if (!tstc() || getchar() != '[')
		return KEY_NONE;
	started = get_timer(0);
	while (!tstc() && get_timer(started) < 50)
		;
	if (!tstc())
		return KEY_NONE;
	character = getchar();
	if (character == 'D' || character == 'A')
		return KEY_LEFT;
	if (character == 'C' || character == 'B')
		return KEY_RIGHT;
	return KEY_NONE;
}

static int do_brainmenu(struct cmd_tbl *command, int flag, int argc, char *const argv[])
{
	int timeout = argc > 1 ? (int)simple_strtol(argv[1], NULL, 10) : 10;
	int selected = 0;
	int seconds_left = timeout;
	int shown_seconds = -2;
	int key;
	unsigned long started = get_timer(0);
	bool counting = timeout > 0;

	brain_display_hold(true);
	draw_menu(selected, counting ? seconds_left : -1);
	for (;;) {
		key = read_key();
		if (key != KEY_NONE)
			counting = false;
		if (key == KEY_LEFT)
			selected = (selected + ARRAY_SIZE(menu_entries) - 1) % ARRAY_SIZE(menu_entries);
		else if (key == KEY_RIGHT)
			selected = (selected + 1) % ARRAY_SIZE(menu_entries);
		if (counting) {
			seconds_left = timeout - (int)(get_timer(started) / 1000);
			if (seconds_left <= 0)
				key = KEY_SELECT;
		}
		if (key == KEY_SELECT)
			break;
		if (key != KEY_NONE || (counting && seconds_left != shown_seconds) || (!counting && shown_seconds != -1)) {
			shown_seconds = counting ? seconds_left : -1;
			draw_menu(selected, shown_seconds);
		}
		udelay(10000);
	}
	printf("brainmenu: %s\n", menu_entries[selected].label);
	if (!menu_entries[selected].command) {
		brain_display_hold(false);
		return CMD_RET_SUCCESS;
	}
	if (run_command(menu_entries[selected].command, 0)) {
		brain_display_hold(false);
		return CMD_RET_FAILURE;
	}
	return CMD_RET_SUCCESS;
}

U_BOOT_CMD(brainmenu, 2, 0, do_brainmenu,
	   "graphical boot menu over the EBOOT splash",
	   "[timeout]\n"
	   "    - choose Windows CE, Linux or the U-Boot console; boots Windows CE after timeout seconds");
