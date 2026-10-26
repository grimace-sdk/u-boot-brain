// SPDX-License-Identifier: GPL-2.0+

#include <common.h>
#include <cpu_func.h>
#include <env.h>
#include <stdio_dev.h>
#include <time.h>
#include <video_fb.h>
#include <asm/cache.h>
#include <asm/io.h>
#include <asm/arch/imx-regs.h>
#include <asm/arch/sys_proto.h>
#include <asm/mach-imx/regs-lcdif.h>
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
#define PORTRAIT_BUFFER		0x44100000
#define EBOOT_SPLASH		0x40100000
#define PANEL_MEMORY_WRITE	0x2c
#define PANEL_COLUMN_ADDRESS	0x2a
#define PANEL_PAGE_ADDRESS	0x2b
#define LCDIF_TIMEOUT		0x100000
#define REFRESH_INTERVAL_MS	40

static GraphicDevice landscape_display;
static struct stdio_dev *console_device;
static bool display_dirty;
static unsigned long last_refresh;
static unsigned int refresh_count;
static bool display_held = true;
static unsigned int lcdif_timeouts;

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

static void wait_lcdif_idle(struct mxs_lcdif_regs *lcdif)
{
	if (mxs_wait_mask_clr(&lcdif->hw_lcdif_ctrl_reg, LCDIF_CTRL_RUN, LCDIF_TIMEOUT))
		lcdif_timeouts++;
}

void brain_display_report(void)
{
	printf("display: %u frames pushed, %u LCDIF timeouts\n", refresh_count, lcdif_timeouts);
}

static void panel_write(struct mxs_lcdif_regs *lcdif, u32 value, bool is_data)
{
	wait_lcdif_idle(lcdif);
	writel(LCDIF_CTRL_SHIFT_NUM_BITS_MASK | LCDIF_CTRL_INPUT_DATA_SWIZZLE_MASK, &lcdif->hw_lcdif_ctrl_clr);
	writel(LCDIF_CTRL_DATA_SHIFT_DIR, &lcdif->hw_lcdif_ctrl_set);
	clrsetbits_le32(&lcdif->hw_lcdif_ctrl1, LCDIF_CTRL1_BYTE_PACKING_FORMAT_MASK,
			0x3 << LCDIF_CTRL1_BYTE_PACKING_FORMAT_OFFSET);
	writel(LCDIF_CTRL_LCDIF_MASTER, &lcdif->hw_lcdif_ctrl_clr);
	writel(0x00010001, &lcdif->hw_lcdif_transfer_count);
	writel(LCDIF_CTRL_DATA_SELECT, is_data ? &lcdif->hw_lcdif_ctrl_set : &lcdif->hw_lcdif_ctrl_clr);
	writel(LCDIF_CTRL_RUN, &lcdif->hw_lcdif_ctrl_set);
	writel(value, &lcdif->hw_lcdif_data);
	wait_lcdif_idle(lcdif);
	clrsetbits_le32(&lcdif->hw_lcdif_ctrl1, LCDIF_CTRL1_BYTE_PACKING_FORMAT_MASK,
			0xf << LCDIF_CTRL1_BYTE_PACKING_FORMAT_OFFSET);
	writel(LCDIF_CTRL_LCDIF_MASTER, &lcdif->hw_lcdif_ctrl_set);
}

static void panel_command(struct mxs_lcdif_regs *lcdif, u32 command)
{
	panel_write(lcdif, command, false);
}

void brain_display_hold(bool hold)
{
	display_held = hold;
	display_dirty = true;
}

static void push_portrait_buffer(unsigned int rows, unsigned int columns)
{
	struct mxs_lcdif_regs *lcdif = (struct mxs_lcdif_regs *)MXS_LCDIF_BASE;

	flush_dcache_range(PORTRAIT_BUFFER, PORTRAIT_BUFFER + ALIGN(rows * columns * 2, ARCH_DMA_MINALIGN));
	panel_command(lcdif, PANEL_MEMORY_WRITE);
	wait_lcdif_idle(lcdif);
	writel(PORTRAIT_BUFFER, &lcdif->hw_lcdif_cur_buf);
	writel((rows << LCDIF_TRANSFER_COUNT_V_COUNT_OFFSET) | (columns << LCDIF_TRANSFER_COUNT_H_COUNT_OFFSET),
	       &lcdif->hw_lcdif_transfer_count);
	writel(LCDIF_CTRL_LCDIF_MASTER | LCDIF_CTRL_DATA_SELECT, &lcdif->hw_lcdif_ctrl_set);
	writel(LCDIF_CTRL_RUN, &lcdif->hw_lcdif_ctrl_set);
	wait_lcdif_idle(lcdif);
	refresh_count++;
}

void brain_display_show(const u16 *canvas)
{
	u16 *portrait = (u16 *)PORTRAIT_BUFFER;
	unsigned int portrait_x, portrait_y;

	for (portrait_y = 0; portrait_y < PORTRAIT_HEIGHT; portrait_y++)
		for (portrait_x = 0; portrait_x < PORTRAIT_WIDTH; portrait_x++)
			*portrait++ = canvas[(PORTRAIT_WIDTH - 1 - portrait_x) * LANDSCAPE_WIDTH + portrait_y];
	push_portrait_buffer(PORTRAIT_HEIGHT, PORTRAIT_WIDTH);
}

static void panel_window(struct mxs_lcdif_regs *lcdif, unsigned int first_column, unsigned int last_column,
			 unsigned int first_row, unsigned int last_row)
{
	panel_command(lcdif, PANEL_COLUMN_ADDRESS);
	panel_write(lcdif, first_column >> 8, true);
	panel_write(lcdif, first_column & 0xff, true);
	panel_write(lcdif, last_column >> 8, true);
	panel_write(lcdif, last_column & 0xff, true);
	panel_command(lcdif, PANEL_PAGE_ADDRESS);
	panel_write(lcdif, first_row >> 8, true);
	panel_write(lcdif, first_row & 0xff, true);
	panel_write(lcdif, last_row >> 8, true);
	panel_write(lcdif, last_row & 0xff, true);
}

void brain_display_show_region(const u16 *canvas, unsigned int left, unsigned int top,
			       unsigned int width, unsigned int height)
{
	struct mxs_lcdif_regs *lcdif = (struct mxs_lcdif_regs *)MXS_LCDIF_BASE;
	unsigned int first_column = PORTRAIT_WIDTH - top - height;
	unsigned int columns = height;
	unsigned int first_row = left;
	unsigned int rows = width;
	u16 *portrait = (u16 *)PORTRAIT_BUFFER;
	unsigned int column, row;

	for (row = first_row; row < first_row + rows; row++)
		for (column = first_column; column < first_column + columns; column++)
			*portrait++ = canvas[(PORTRAIT_WIDTH - 1 - column) * LANDSCAPE_WIDTH + row];
	panel_window(lcdif, first_column, first_column + columns - 1, first_row, first_row + rows - 1);
	push_portrait_buffer(rows, columns);
	panel_window(lcdif, 0, PORTRAIT_WIDTH - 1, 0, PORTRAIT_HEIGHT - 1);
}

static void rotate_to_portrait(void)
{
	const u16 *console = (const u16 *)LANDSCAPE_BUFFER;
	const u16 *splash = (const u16 *)EBOOT_SPLASH;
	u16 *portrait = (u16 *)PORTRAIT_BUFFER;
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
	push_portrait_buffer(PORTRAIT_HEIGHT, PORTRAIT_WIDTH);
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
