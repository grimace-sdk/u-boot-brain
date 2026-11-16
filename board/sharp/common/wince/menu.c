// SPDX-License-Identifier: GPL-2.0+

#include <common.h>
#include <command.h>
#include <console.h>
#include <env.h>
#include <fs.h>
#include <mapmem.h>
#include <time.h>
#include <video_font.h>
#include <linux/delay.h>
#include "brain.h"
#include "menu_model.h"
#include "menu_tux.h"

#define CANVAS_BUFFER		0x44200000
#define BACKGROUND_BUFFER	0x44300000
#define MENU_FILE		"menu.txt"
#define MENU_FILE_BYTES		4096
#define PATH_LENGTH		64
#define POLL_MS			10
#define FIRST_KEY_MS		200
#define ESCAPE_WAIT_MS		50
#define TILE_ICON_REFERENCE	88
#define HIGHLIGHT_WIDTH		4
#define STATUS_LENGTH		64

#define COLOUR_BLACK		0x0000
#define COLOUR_WHITE		0xffff
#define COLOUR_TILE		0x0000
#define COLOUR_TERMINAL_TEXT	0x07e0
#define COLOUR_WINDOWS_RED	0xf2a2
#define COLOUR_WINDOWS_GREEN	0x7dc0
#define COLOUR_WINDOWS_BLUE	0x051e
#define COLOUR_WINDOWS_YELLOW	0xfde0
#define COLOUR_STOCK_LIGHT	0xbdf7
#define COLOUR_STOCK_DARK	0x7bef

struct menu_layout {
	int tile_size;
	int tile_radius;
	int tile_top;
	int tile_spacing;
	int label_top;
	int label_scale;
	int status_top;
	int progress_width;
	int progress_height;
	int progress_top;
};

static u16 *canvas = (u16 *)CANVAS_BUFFER;
static u16 *background = (u16 *)BACKGROUND_BUFFER;
static struct brain_screen screen;
static struct menu_layout layout;
static struct menu_model shown_model;
static bool screen_ready;
static bool menu_shown;
static bool linux_found;
static char menu_file[MENU_FILE_BYTES];

static void set_layout(void)
{
	bool large = screen.height >= 480;

	layout.tile_size = large ? 88 : 64;
	layout.tile_radius = large ? 16 : 12;
	layout.tile_spacing = large ? 180 : 112;
	layout.label_scale = large ? 2 : 1;
	layout.tile_top = large ? screen.height * 2 / 3 : screen.height * 3 / 5;
	layout.label_top = layout.tile_top + layout.tile_size + (large ? 8 : 6);
	layout.status_top = screen.height - (large ? 22 : 18);
	layout.progress_width = large ? 304 : 200;
	layout.progress_height = large ? 16 : 12;
	layout.progress_top = screen.height - (large ? 24 : 20);
}

static void put_pixel(int x, int y, u16 colour)
{
	if (x >= 0 && x < screen.width && y >= 0 && y < screen.height)
		canvas[y * screen.width + x] = colour;
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
							put_pixel(left + column * scale + scale_x,
								  top + row * scale + scale_y, colour);
	}
}

static void draw_centred_text(const char *text, int centre, int top, int scale, u16 colour)
{
	draw_text(text, centre - (int)strlen(text) * VIDEO_FONT_WIDTH * scale / 2, top, scale, colour);
}

static int icon_length(int reference)
{
	return reference * layout.tile_size / TILE_ICON_REFERENCE;
}

static void draw_windows_icon(int left, int top, bool stock)
{
	const int pane = icon_length(30);
	const int gap = icon_length(4);
	const int origin_x = left + (layout.tile_size - 2 * pane - gap) / 2;
	const int origin_y = top + (layout.tile_size - 2 * pane - gap) / 2;

	fill_rectangle(origin_x, origin_y, pane, pane, stock ? COLOUR_STOCK_LIGHT : COLOUR_WINDOWS_RED);
	fill_rectangle(origin_x + pane + gap, origin_y, pane, pane, stock ? COLOUR_STOCK_DARK : COLOUR_WINDOWS_GREEN);
	fill_rectangle(origin_x, origin_y + pane + gap, pane, pane, stock ? COLOUR_STOCK_DARK : COLOUR_WINDOWS_BLUE);
	fill_rectangle(origin_x + pane + gap, origin_y + pane + gap, pane, pane,
		       stock ? COLOUR_STOCK_LIGHT : COLOUR_WINDOWS_YELLOW);
}

static void draw_tux_icon(int left, int top)
{
	const int size = icon_length(TUX_WIDTH);
	const int origin_x = left + (layout.tile_size - size) / 2;
	const int origin_y = top + (layout.tile_size - size) / 2;
	int x, y;

	for (y = 0; y < size; y++)
		for (x = 0; x < size; x++)
			put_pixel(origin_x + x, origin_y + y,
				  tux_pixels[(y * TUX_HEIGHT / size) * TUX_WIDTH + x * TUX_WIDTH / size]);
}

static void draw_terminal_icon(int left, int top)
{
	const int scale = layout.label_scale + 1;

	draw_text(">_", left + layout.tile_size / 2 - VIDEO_FONT_WIDTH * scale,
		  top + layout.tile_size / 2 - VIDEO_FONT_HEIGHT * scale / 2, scale, COLOUR_TERMINAL_TEXT);
}

static void draw_icon(enum menu_action action, int left, int top)
{
	if (action == MENU_WINDOWS_CE || action == MENU_WINDOWS_CE_STOCK)
		draw_windows_icon(left, top, action == MENU_WINDOWS_CE_STOCK);
	else if (action == MENU_LINUX)
		draw_tux_icon(left, top);
	else
		draw_terminal_icon(left, top);
}

static void draw_menu_body(const struct menu_model *model)
{
	int first_centre = (int)screen.width / 2 - ((int)model->count - 1) * layout.tile_spacing / 2;
	unsigned int index;

	memcpy(canvas, background, screen.width * screen.height * sizeof(*canvas));
	for (index = 0; index < model->count; index++) {
		int centre = first_centre + index * layout.tile_spacing;
		int left = centre - layout.tile_size / 2;

		if (index == model->selected)
			fill_rounded_rectangle(left - HIGHLIGHT_WIDTH, layout.tile_top - HIGHLIGHT_WIDTH,
					       layout.tile_size + 2 * HIGHLIGHT_WIDTH,
					       layout.tile_radius + HIGHLIGHT_WIDTH, COLOUR_WHITE);
		fill_rounded_rectangle(left, layout.tile_top, layout.tile_size, layout.tile_radius, COLOUR_TILE);
		draw_icon(model->entries[index], left, layout.tile_top);
		draw_centred_text(menu_action_label(model->entries[index]), centre, layout.label_top,
				  layout.label_scale, COLOUR_BLACK);
	}
	shown_model = *model;
	menu_shown = true;
}

static void draw_menu(const struct menu_model *model, const char *status)
{
	if (!screen_ready)
		return;
	draw_menu_body(model);
	if (*status)
		draw_centred_text(status, screen.width / 2, layout.status_top, 1, COLOUR_BLACK);
	brain_screen_push(canvas, 0, 0, screen.width, screen.height);
}

int brain_progress_bar_open(struct brain_progress_bar *bar)
{
	if (!menu_shown)
		return -ENODEV;
	draw_menu_body(&shown_model);
	if (brain_screen_push(canvas, 0, 0, screen.width, screen.height))
		return -EIO;
	bar->width = layout.progress_width;
	bar->height = layout.progress_height;
	bar->left = (screen.width - bar->width) / 2;
	bar->top = layout.progress_top;
	bar->interval_ms = screen.update_interval_ms;
	return 0;
}

int brain_progress_bar_push(const u16 *pixels, const struct brain_progress_bar *bar)
{
	unsigned int row;

	for (row = 0; row < bar->height; row++)
		memcpy(&canvas[(bar->top + row) * screen.width + bar->left], &pixels[row * bar->width],
		       bar->width * sizeof(*pixels));
	return brain_screen_push(canvas, bar->left, bar->top, bar->width, bar->height);
}

__weak int brain_screen_open(struct brain_screen *opened, u16 *splash)
{
	return -ENODEV;
}

__weak int brain_screen_push(const u16 *pixels, unsigned int left, unsigned int top, unsigned int width,
			     unsigned int height)
{
	return -ENODEV;
}

__weak int brain_keys_poll(void)
{
	return BRAIN_KEY_NONE;
}

__weak bool brain_keys_held(void)
{
	return false;
}

__weak void brain_display_hold(bool hold)
{
}

static bool wait_for_character(void)
{
	unsigned long started = get_timer(0);

	while (!tstc() && get_timer(started) < ESCAPE_WAIT_MS)
		;
	return tstc();
}

static int read_console_key(void)
{
	int character;

	if (!tstc())
		return BRAIN_KEY_NONE;
	character = getchar();
	if (character == '\r' || character == '\n' || character == ' ')
		return BRAIN_KEY_SELECT;
	if (character != 0x1b)
		return BRAIN_KEY_NONE;
	if (!wait_for_character())
		return BRAIN_KEY_BACK;
	if (getchar() != '[' || !wait_for_character())
		return BRAIN_KEY_NONE;
	character = getchar();
	if (character == 'D' || character == 'A')
		return BRAIN_KEY_PREVIOUS;
	if (character == 'C' || character == 'B')
		return BRAIN_KEY_NEXT;
	return BRAIN_KEY_NONE;
}

static int read_key(void)
{
	int key = read_console_key();

	return key != BRAIN_KEY_NONE ? key : brain_keys_poll();
}

static int read_card_file(const char *path, loff_t *length)
{
	if (fs_set_blk_dev("mmc", CARD_DEVICE, FS_TYPE_FAT))
		return -ENODEV;
	return fs_read(path, map_to_sysmem(menu_file), 0, sizeof(menu_file) - 1, length);
}

static void load_menu_config(struct menu_config *config)
{
	char path[PATH_LENGTH];
	loff_t length = 0;
	int result = -ENOENT;

	if (*CONFIG_SHARP_WINCE_PROFILE) {
		snprintf(path, sizeof(path), CARD_DIRECTORY "/%s/" MENU_FILE, CONFIG_SHARP_WINCE_PROFILE);
		result = read_card_file(path, &length);
	}
	if (result) {
		strcpy(path, CARD_DIRECTORY "/" MENU_FILE);
		result = read_card_file(path, &length);
	}
	if (result) {
		printf("brainmenu: no " MENU_FILE ", Windows CE entries only\n");
		return;
	}
	menu_config_parse(config, menu_file, length);
	printf("brainmenu: %s, %u lines ignored\n", path, config->ignored_lines);
}

static bool card_file_exists(const char *partition, const char *path)
{
	if (fs_set_blk_dev("mmc", partition, FS_TYPE_ANY))
		return false;
	return fs_exists(path);
}

static bool linux_present(const struct menu_config *config)
{
	char partition[MENU_PARTITION_LENGTH];
	const char *path = menu_linux_probe_path(config);
	bool kernel = menu_linux_target(config) == MENU_LINUX_KERNEL;
	bool present;

	if (!config->linux_enabled)
		return false;
	if (path) {
		present = card_file_exists(config->linux_partition, path);
		if (present && kernel && config->linux_dtb[0])
			present = card_file_exists(config->linux_partition, config->linux_dtb);
		if (present && kernel && config->linux_initrd[0])
			present = card_file_exists(config->linux_partition, config->linux_initrd);
		printf("brainmenu: Linux %s on mmc %s %s\n", path, config->linux_partition,
		       present ? "found" : "missing");
		return present;
	}
	snprintf(partition, sizeof(partition), "%s:%s", env_get("sddev") ?: "1", env_get("sdpart") ?: "1");
	path = env_get("image") ?: "zImage";
	present = card_file_exists(partition, path);
	printf("brainmenu: Brainux %s on mmc %s %s\n", path, partition, present ? "found" : "missing");
	return present;
}

static int run_action(const struct menu_config *config, enum menu_action action)
{
	char command[MENU_COMMAND_LENGTH];

	switch (action) {
	case MENU_WINDOWS_CE:
		return run_command("bootsharpce", 0);
	case MENU_WINDOWS_CE_STOCK:
		return run_command("bootsharpce stock", 0);
	case MENU_LINUX:
		if (menu_linux_command(config, command, sizeof(command)))
			return -ENOSPC;
		if (menu_linux_target(config) == MENU_LINUX_KERNEL && config->linux_cmdline[0])
			env_set("bootargs", config->linux_cmdline);
		printf("brainmenu: %s\n", command);
		return run_command(command, 0);
	default:
		return 0;
	}
}

static void reveal_console(struct menu_model *model, const struct menu_config *config)
{
	menu_model_build(model, config, linux_found, true, menu_model_selected(model));
}

static void choose(struct menu_model *model, const struct menu_config *config, bool counting, const char *message)
{
	unsigned long started = get_timer(0);
	int seconds_left = config->timeout;
	char status[STATUS_LENGTH];
	bool redraw = true;
	int key;

	for (;;) {
		key = read_key();
		if (key != BRAIN_KEY_NONE) {
			printf("brainmenu: key %d\n", key);
			counting = false;
			message = "";
			redraw = true;
		}
		if (key == BRAIN_KEY_SELECT)
			return;
		if (key == BRAIN_KEY_PREVIOUS)
			menu_model_move(model, -1);
		else if (key == BRAIN_KEY_NEXT)
			menu_model_move(model, 1);
		else if (key == BRAIN_KEY_BACK)
			reveal_console(model, config);
		if (counting && !config->timeout && get_timer(started) >= FIRST_KEY_MS)
			return;
		if (counting && config->timeout) {
			int remaining = config->timeout - (int)(get_timer(started) / 1000);

			if (remaining <= 0)
				return;
			redraw |= remaining != seconds_left;
			seconds_left = remaining;
		}
		if (redraw) {
			if (*message)
				snprintf(status, sizeof(status), "%s", message);
			else if (counting && config->timeout)
				snprintf(status, sizeof(status), "Starting %s in %d",
					 menu_action_label(menu_model_selected(model)), seconds_left);
			else
				status[0] = 0;
			draw_menu(model, status);
			redraw = false;
		}
		udelay(POLL_MS * 1000);
	}
}

static int do_brainmenu(struct cmd_tbl *command, int flag, int argc, char *const argv[])
{
	int timeout = argc > 1 ? (int)simple_strtol(argv[1], NULL, 10) : MENU_DEFAULT_TIMEOUT;
	struct menu_config config;
	struct menu_model model;
	enum menu_action action;
	char message[STATUS_LENGTH] = "";
	bool held = brain_keys_held();

	menu_config_defaults(&config, timeout < 0 ? 0 : timeout);
	load_menu_config(&config);
	linux_found = linux_present(&config);
	menu_model_build(&model, &config, linux_found, held, config.default_action);
	if (held)
		printf("brainmenu: key held at start, countdown off\n");
	brain_display_hold(true);
	screen_ready = !brain_screen_open(&screen, background);
	if (screen_ready)
		set_layout();
	else
		printf("brainmenu: no screen\n");
	for (;;) {
		choose(&model, &config, !held && !*message, message);
		action = menu_model_selected(&model);
		printf("brainmenu: %s\n", menu_action_label(action));
		if (action == MENU_CONSOLE)
			break;
		run_action(&config, action);
		snprintf(message, sizeof(message), "%s did not start", menu_action_label(action));
		printf("brainmenu: %s\n", message);
		menu_model_build(&model, &config, linux_found, true, action);
	}
	brain_display_hold(false);
	return CMD_RET_SUCCESS;
}

U_BOOT_CMD(brainmenu, 2, 0, do_brainmenu,
	   "graphical boot menu over the EBOOT splash",
	   "[timeout]\n"
	   "    - entries, default and timeout from " CARD_DIRECTORY "/[PROFILE/]" MENU_FILE "\n"
	   "    - timeout (default 10 s) applies when " MENU_FILE " sets none");
