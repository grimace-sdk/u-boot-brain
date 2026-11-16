// SPDX-License-Identifier: GPL-2.0+

#include <common.h>
#include <cpu_func.h>
#include <asm/cache.h>
#include <asm/io.h>
#include <asm/arch/imx-regs.h>
#include <asm/arch/sys_proto.h>
#include <asm/mach-imx/regs-lcdif.h>
#include "../common/wince/brain.h"

#define PROGRESS_LEFT		140
#define PROGRESS_TOP		300
#define PROGRESS_WIDTH		200
#define PROGRESS_HEIGHT		12
#define PROGRESS_PIXELS		(PROGRESS_WIDTH * PROGRESS_HEIGHT)
#define PANEL_COLUMNS		320
#define PANEL_PAGES		480
#define PANEL_COLUMN_ADDRESS	0x2a
#define PANEL_PAGE_ADDRESS	0x2b
#define PANEL_MEMORY_WRITE	0x2c
#define PANEL_ADDRESS_ORDER	0x36
#define ADDRESS_ORDER_EXCHANGED	0x28
#define LCDIF_TIMEOUT_US	20000
#define SAVED_CTRL_FIELDS	(LCDIF_CTRL_SHIFT_NUM_BITS_MASK | LCDIF_CTRL_INPUT_DATA_SWIZZLE_MASK | \
				 LCDIF_CTRL_DATA_SHIFT_DIR | LCDIF_CTRL_LCDIF_MASTER | LCDIF_CTRL_DATA_SELECT)

static u16 progress_pixels[PROGRESS_PIXELS] __aligned(ARCH_DMA_MINALIGN);

struct lcdif_state {
	u32 ctrl;
	u32 ctrl1;
	u32 transfer_count;
	u32 current_buffer;
};

static bool lcdif_idle(struct mxs_lcdif_regs *lcdif)
{
	return !mxs_wait_mask_clr(&lcdif->hw_lcdif_ctrl_reg, LCDIF_CTRL_RUN, LCDIF_TIMEOUT_US);
}

static bool panel_write(struct mxs_lcdif_regs *lcdif, u32 value, bool is_data)
{
	if (!lcdif_idle(lcdif))
		return false;
	writel(LCDIF_CTRL_SHIFT_NUM_BITS_MASK | LCDIF_CTRL_INPUT_DATA_SWIZZLE_MASK | LCDIF_CTRL_LCDIF_MASTER,
	       &lcdif->hw_lcdif_ctrl_clr);
	writel(LCDIF_CTRL_DATA_SHIFT_DIR, &lcdif->hw_lcdif_ctrl_set);
	clrsetbits_le32(&lcdif->hw_lcdif_ctrl1, LCDIF_CTRL1_BYTE_PACKING_FORMAT_MASK,
			0x3 << LCDIF_CTRL1_BYTE_PACKING_FORMAT_OFFSET);
	writel(0x00010001, &lcdif->hw_lcdif_transfer_count);
	writel(LCDIF_CTRL_DATA_SELECT, is_data ? &lcdif->hw_lcdif_ctrl_set : &lcdif->hw_lcdif_ctrl_clr);
	writel(LCDIF_CTRL_RUN, &lcdif->hw_lcdif_ctrl_set);
	writel(value, &lcdif->hw_lcdif_data);
	return lcdif_idle(lcdif);
}

static bool panel_range(struct mxs_lcdif_regs *lcdif, u32 command, unsigned int first, unsigned int last)
{
	return panel_write(lcdif, command, false) && panel_write(lcdif, first >> 8, true) &&
	       panel_write(lcdif, first & 0xff, true) && panel_write(lcdif, last >> 8, true) &&
	       panel_write(lcdif, last & 0xff, true);
}

static bool panel_window(struct mxs_lcdif_regs *lcdif, unsigned int first_column, unsigned int columns,
			 unsigned int first_page, unsigned int pages)
{
	return panel_range(lcdif, PANEL_COLUMN_ADDRESS, first_column, first_column + columns - 1) &&
	       panel_range(lcdif, PANEL_PAGE_ADDRESS, first_page, first_page + pages - 1);
}

static bool push_pixels(struct mxs_lcdif_regs *lcdif, const struct brain_progress_bar *bar)
{
	ulong start = (ulong)progress_pixels;

	flush_dcache_range(start, start + ALIGN(sizeof(progress_pixels), ARCH_DMA_MINALIGN));
	if (!panel_write(lcdif, PANEL_ADDRESS_ORDER, false) || !panel_write(lcdif, ADDRESS_ORDER_EXCHANGED, true) ||
	    !panel_window(lcdif, bar->top, bar->height, bar->left, bar->width) ||
	    !panel_write(lcdif, PANEL_MEMORY_WRITE, false))
		return false;
	clrsetbits_le32(&lcdif->hw_lcdif_ctrl1, LCDIF_CTRL1_BYTE_PACKING_FORMAT_MASK,
			0xf << LCDIF_CTRL1_BYTE_PACKING_FORMAT_OFFSET);
	writel(start, &lcdif->hw_lcdif_cur_buf);
	writel((bar->height << LCDIF_TRANSFER_COUNT_V_COUNT_OFFSET) |
	       (bar->width << LCDIF_TRANSFER_COUNT_H_COUNT_OFFSET), &lcdif->hw_lcdif_transfer_count);
	writel(LCDIF_CTRL_LCDIF_MASTER | LCDIF_CTRL_DATA_SELECT, &lcdif->hw_lcdif_ctrl_set);
	writel(LCDIF_CTRL_RUN, &lcdif->hw_lcdif_ctrl_set);
	return lcdif_idle(lcdif) && panel_window(lcdif, 0, PANEL_COLUMNS, 0, PANEL_PAGES);
}

static void save_lcdif(struct mxs_lcdif_regs *lcdif, struct lcdif_state *state)
{
	state->ctrl = readl(&lcdif->hw_lcdif_ctrl_reg);
	state->ctrl1 = readl(&lcdif->hw_lcdif_ctrl1);
	state->transfer_count = readl(&lcdif->hw_lcdif_transfer_count);
	state->current_buffer = readl(&lcdif->hw_lcdif_cur_buf);
}

static void restore_lcdif(struct mxs_lcdif_regs *lcdif, const struct lcdif_state *state)
{
	writel(SAVED_CTRL_FIELDS, &lcdif->hw_lcdif_ctrl_clr);
	writel(state->ctrl & SAVED_CTRL_FIELDS, &lcdif->hw_lcdif_ctrl_set);
	clrsetbits_le32(&lcdif->hw_lcdif_ctrl1, LCDIF_CTRL1_BYTE_PACKING_FORMAT_MASK,
			state->ctrl1 & LCDIF_CTRL1_BYTE_PACKING_FORMAT_MASK);
	writel(state->transfer_count, &lcdif->hw_lcdif_transfer_count);
	writel(state->current_buffer, &lcdif->hw_lcdif_cur_buf);
}

int brain_progress_bar_open(struct brain_progress_bar *bar)
{
	struct mxs_lcdif_regs *lcdif = (struct mxs_lcdif_regs *)MXS_LCDIF_BASE;
	u32 ctrl = readl(&lcdif->hw_lcdif_ctrl_reg);

	if ((ctrl & (LCDIF_CTRL_DOTCLK_MODE | LCDIF_CTRL_DVI_MODE)) || !(ctrl & LCDIF_CTRL_LCDIF_MASTER)) {
		printf("progress: lcdif ctrl %08x is not EBOOT's system mode, no bar\n", ctrl);
		return -ENODEV;
	}
	bar->left = PROGRESS_LEFT;
	bar->top = PROGRESS_TOP;
	bar->width = PROGRESS_WIDTH;
	bar->height = PROGRESS_HEIGHT;
	return 0;
}

int brain_progress_bar_push(const u16 *pixels, const struct brain_progress_bar *bar)
{
	struct mxs_lcdif_regs *lcdif = (struct mxs_lcdif_regs *)MXS_LCDIF_BASE;
	struct lcdif_state state;
	bool pushed;

	if (bar->width * bar->height > PROGRESS_PIXELS)
		return -EINVAL;
	memcpy(progress_pixels, pixels, bar->width * bar->height * sizeof(*pixels));
	save_lcdif(lcdif, &state);
	pushed = push_pixels(lcdif, bar);
	restore_lcdif(lcdif, &state);
	if (!pushed) {
		printf("progress: lcdif timed out, no bar\n");
		return -ETIMEDOUT;
	}
	return 0;
}
