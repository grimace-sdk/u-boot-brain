// SPDX-License-Identifier: GPL-2.0+

#include <common.h>
#include <cpu_func.h>
#include <asm/cache.h>
#include <asm/io.h>
#include <asm/arch/imx-regs.h>
#include <asm/arch/sys_proto.h>
#include <asm/mach-imx/regs-lcdif.h>
#include "brain.h"

#define PANEL_COLUMNS		480
#define PANEL_PAGES		CONFIG_SHARP_PANEL_PAGES
#define EBOOT_SPLASH		0x40100000
#define PANEL_MEMORY_WRITE	0x2c
#define PANEL_COLUMN_ADDRESS	0x2a
#define PANEL_PAGE_ADDRESS	0x2b
#define LCDIF_TIMEOUT		0x100000
#define FULL_FRAME_INTERVAL_MS	200

static unsigned int push_count;
static unsigned int lcdif_timeouts;

static void wait_lcdif_idle(struct mxs_lcdif_regs *lcdif)
{
	if (mxs_wait_mask_clr(&lcdif->hw_lcdif_ctrl_reg, LCDIF_CTRL_RUN, LCDIF_TIMEOUT))
		lcdif_timeouts++;
}

void brain_display_report(void)
{
	printf("display: %u frames pushed, %u LCDIF timeouts\n", push_count, lcdif_timeouts);
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

static void panel_range(struct mxs_lcdif_regs *lcdif, u32 command, unsigned int first, unsigned int last)
{
	panel_write(lcdif, command, false);
	panel_write(lcdif, first >> 8, true);
	panel_write(lcdif, first & 0xff, true);
	panel_write(lcdif, last >> 8, true);
	panel_write(lcdif, last & 0xff, true);
}

static void panel_window(struct mxs_lcdif_regs *lcdif, unsigned int first_column, unsigned int columns,
			 unsigned int first_page, unsigned int pages)
{
	panel_range(lcdif, PANEL_COLUMN_ADDRESS, first_column, first_column + columns - 1);
	panel_range(lcdif, PANEL_PAGE_ADDRESS, first_page, first_page + pages - 1);
}

void brain_portrait_push(unsigned int rows, unsigned int columns)
{
	struct mxs_lcdif_regs *lcdif = (struct mxs_lcdif_regs *)MXS_LCDIF_BASE;

	flush_dcache_range(BRAIN_PORTRAIT_BUFFER,
			   BRAIN_PORTRAIT_BUFFER + ALIGN(rows * columns * 2, ARCH_DMA_MINALIGN));
	panel_write(lcdif, PANEL_MEMORY_WRITE, false);
	wait_lcdif_idle(lcdif);
	writel(BRAIN_PORTRAIT_BUFFER, &lcdif->hw_lcdif_cur_buf);
	writel((rows << LCDIF_TRANSFER_COUNT_V_COUNT_OFFSET) | (columns << LCDIF_TRANSFER_COUNT_H_COUNT_OFFSET),
	       &lcdif->hw_lcdif_transfer_count);
	writel(LCDIF_CTRL_LCDIF_MASTER | LCDIF_CTRL_DATA_SELECT, &lcdif->hw_lcdif_ctrl_set);
	writel(LCDIF_CTRL_RUN, &lcdif->hw_lcdif_ctrl_set);
	wait_lcdif_idle(lcdif);
	push_count++;
}

int brain_screen_open(struct brain_screen *screen, u16 *background)
{
	struct mxs_lcdif_regs *lcdif = (struct mxs_lcdif_regs *)MXS_LCDIF_BASE;
	const u16 *splash = (const u16 *)EBOOT_SPLASH;
	u32 ctrl = readl(&lcdif->hw_lcdif_ctrl_reg);
	unsigned int x, y;

	if ((ctrl & (LCDIF_CTRL_DOTCLK_MODE | LCDIF_CTRL_DVI_MODE)) || !(ctrl & LCDIF_CTRL_LCDIF_MASTER)) {
		printf("display: lcdif ctrl %08x is not EBOOT's system mode\n", ctrl);
		return -ENODEV;
	}
	screen->width = PANEL_PAGES;
	screen->height = PANEL_COLUMNS;
	screen->update_interval_ms = IS_ENABLED(CONFIG_SHARP_PANEL_FULL_FRAMES) ? FULL_FRAME_INTERVAL_MS : 0;
	for (y = 0; y < PANEL_COLUMNS; y++)
		for (x = 0; x < PANEL_PAGES; x++)
			*background++ = splash[x * PANEL_COLUMNS + (PANEL_COLUMNS - 1 - y)];
	return 0;
}

int brain_screen_push(const u16 *canvas, unsigned int left, unsigned int top, unsigned int width,
		      unsigned int height)
{
	struct mxs_lcdif_regs *lcdif = (struct mxs_lcdif_regs *)MXS_LCDIF_BASE;
	u16 *portrait = (u16 *)BRAIN_PORTRAIT_BUFFER;
	bool full_screen;
	unsigned int first_column, row, column;

	if (IS_ENABLED(CONFIG_SHARP_PANEL_FULL_FRAMES)) {
		left = 0;
		top = 0;
		width = PANEL_PAGES;
		height = PANEL_COLUMNS;
	}
	full_screen = !left && !top && width == PANEL_PAGES && height == PANEL_COLUMNS;
	first_column = PANEL_COLUMNS - top - height;
	for (row = left; row < left + width; row++)
		for (column = first_column; column < first_column + height; column++)
			*portrait++ = canvas[(PANEL_COLUMNS - 1 - column) * PANEL_PAGES + row];
	if (full_screen) {
		brain_portrait_push(width, height);
		return 0;
	}
	panel_window(lcdif, first_column, height, left, width);
	brain_portrait_push(width, height);
	panel_window(lcdif, 0, PANEL_COLUMNS, 0, PANEL_PAGES);
	return 0;
}
