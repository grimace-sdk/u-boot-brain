// SPDX-License-Identifier: GPL-2.0+

#include <common.h>
#include <command.h>
#include <time.h>
#include <asm/io.h>
#include <asm/arch/imx-regs.h>
#include <asm/arch/sys_proto.h>
#include <asm/mach-imx/regs-lcdif.h>

#define PINCTRL_BASE		0x80018000
#define PINCTRL_MUXSEL(bank, pin)	(PINCTRL_BASE + 0x100 + (bank) * 0x20 + ((pin) / 16) * 0x10)
#define PINCTRL_DIN(bank)	(PINCTRL_BASE + 0x900 + (bank) * 0x10)
#define PINCTRL_DOE(bank)	(PINCTRL_BASE + 0xb00 + (bank) * 0x10)
#define REGISTER_SET		0x4
#define REGISTER_CLEAR		0x8
#define MUX_GPIO		3
#define TE_BANK			4
#define TE_PIN			16
#define DEFAULT_SAMPLE_MS	200
#define LCDIF_TIMEOUT		0x100000
#define PANEL_TEAR_OFF		0x34
#define PANEL_TEAR_ON		0x35
#define PANEL_TEAR_VBLANK_ONLY	0x00
#define PIO_CTRL_FIELDS		(LCDIF_CTRL_SHIFT_NUM_BITS_MASK | LCDIF_CTRL_INPUT_DATA_SWIZZLE_MASK | \
				 LCDIF_CTRL_DATA_SHIFT_DIR | LCDIF_CTRL_LCDIF_MASTER | LCDIF_CTRL_DATA_SELECT)

static bool lcdif_idle(struct mxs_lcdif_regs *lcdif)
{
	return !mxs_wait_mask_clr(&lcdif->hw_lcdif_ctrl_reg, LCDIF_CTRL_RUN, LCDIF_TIMEOUT);
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

static bool panel_sequence(const u8 *bytes, unsigned int count)
{
	struct mxs_lcdif_regs *lcdif = (struct mxs_lcdif_regs *)MXS_LCDIF_BASE;
	u32 ctrl = readl(&lcdif->hw_lcdif_ctrl_reg);
	u32 ctrl1 = readl(&lcdif->hw_lcdif_ctrl1);
	u32 transfer_count = readl(&lcdif->hw_lcdif_transfer_count);
	bool written = true;
	unsigned int index;

	printf("teprobe: lcdif ctrl %08x ctrl1 %08x transfer %08x\n", ctrl, ctrl1, transfer_count);
	if (ctrl & (LCDIF_CTRL_DOTCLK_MODE | LCDIF_CTRL_DVI_MODE)) {
		printf("teprobe: lcdif is not in system mode, not sending panel commands\n");
		return false;
	}
	for (index = 0; index < count && written; index++)
		written = panel_write(lcdif, bytes[index], index > 0);
	writel(PIO_CTRL_FIELDS, &lcdif->hw_lcdif_ctrl_clr);
	writel(ctrl & PIO_CTRL_FIELDS, &lcdif->hw_lcdif_ctrl_set);
	clrsetbits_le32(&lcdif->hw_lcdif_ctrl1, LCDIF_CTRL1_BYTE_PACKING_FORMAT_MASK,
			ctrl1 & LCDIF_CTRL1_BYTE_PACKING_FORMAT_MASK);
	writel(transfer_count, &lcdif->hw_lcdif_transfer_count);
	if (!written)
		printf("teprobe: lcdif timed out\n");
	return written;
}

static void sample_pin(unsigned long sample_us)
{
	u32 bit = 1 << TE_PIN;
	unsigned long started, now, first_rise = 0, last_rise = 0;
	unsigned int rises = 0, falls = 0, high_samples = 0, samples = 0;
	bool level, previous;

	previous = readl(PINCTRL_DIN(TE_BANK)) & bit;
	started = timer_get_us();
	do {
		now = timer_get_us();
		level = readl(PINCTRL_DIN(TE_BANK)) & bit;
		samples++;
		if (level)
			high_samples++;
		if (level && !previous) {
			if (!rises)
				first_rise = now;
			last_rise = now;
			rises++;
		} else if (!level && previous) {
			falls++;
		}
		previous = level;
	} while (now - started < sample_us);
	printf("teprobe: %lu ms, %u samples, %u high, %u rising, %u falling edges\n", sample_us / 1000, samples,
	       high_samples, rises, falls);
	if (rises > 1)
		printf("teprobe: mean period %lu us\n", (last_rise - first_rise) / (rises - 1));
}

static int do_teprobe(struct cmd_tbl *command, int flag, int argc, char *const argv[])
{
	static const u8 tear_on[] = { PANEL_TEAR_ON, PANEL_TEAR_VBLANK_ONLY };
	static const u8 tear_off[] = { PANEL_TEAR_OFF };
	u32 mux_address = PINCTRL_MUXSEL(TE_BANK, TE_PIN);
	unsigned int mux_shift = (TE_PIN % 16) * 2;
	u32 mux = readl(mux_address);
	u32 output_enable = readl(PINCTRL_DOE(TE_BANK));
	u32 original_function = (mux >> mux_shift) & 3;
	unsigned long sample_us = (argc > 1 ? simple_strtoul(argv[1], NULL, 10) : DEFAULT_SAMPLE_MS) * 1000;

	printf("teprobe: pin %u_%u muxsel %08x function %u, doe %08x (output %s)\n", TE_BANK, TE_PIN, mux,
	       original_function, output_enable, output_enable & (1 << TE_PIN) ? "enabled" : "disabled");
	if (output_enable & (1 << TE_PIN)) {
		printf("teprobe: pin is an output, not sampling\n");
		return CMD_RET_FAILURE;
	}
	if (original_function != MUX_GPIO)
		writel(MUX_GPIO << mux_shift, mux_address + REGISTER_SET);

	printf("teprobe: as EBOOT left the panel\n");
	sample_pin(sample_us);
	if (panel_sequence(tear_on, ARRAY_SIZE(tear_on))) {
		printf("teprobe: after tearing effect on (35 00)\n");
		sample_pin(sample_us);
		panel_sequence(tear_off, ARRAY_SIZE(tear_off));
		printf("teprobe: after tearing effect off (34)\n");
		sample_pin(sample_us);
	}

	if (original_function != MUX_GPIO) {
		writel(3 << mux_shift, mux_address + REGISTER_CLEAR);
		writel(original_function << mux_shift, mux_address + REGISTER_SET);
	}
	printf("teprobe: muxsel restored to %08x\n", readl(mux_address));
	return CMD_RET_SUCCESS;
}

U_BOOT_CMD(teprobe, 2, 0, do_teprobe,
	   "sample the panel TE pin (GPIO 4_16), then with tearing effect on, then off",
	   "[milliseconds]");
