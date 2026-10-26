// SPDX-License-Identifier: GPL-2.0+

#include <common.h>
#include <command.h>
#include <asm/io.h>
#include "brain.h"

#define SNAPSHOT_ADDRESS	0x46000000
#define SNAPSHOT_MAGIC		0x48575354

struct register_range {
	const char *name;
	u32 base;
	u32 first;
	u32 last;
};

static const struct register_range snapshot_ranges[] = {
	{ "clkctrl", 0x80040000, 0x000, 0x160 },
	{ "clkctrl", 0x80040000, 0x1b0, 0x1d0 },
	{ "pinctrl.mux", 0x80018000, 0x100, 0x1d0 },
	{ "pinctrl.drive", 0x80018000, 0x300, 0x430 },
	{ "pinctrl.pull", 0x80018000, 0x600, 0x660 },
	{ "pinctrl.dout", 0x80018000, 0x700, 0x740 },
	{ "pinctrl.doe", 0x80018000, 0xb00, 0xb40 },
	{ "power", 0x80044000, 0x000, 0x0e0 },
	{ "apbh", 0x80004000, 0x000, 0x030 },
	{ "ssp0", 0x80010000, 0x000, 0x000 },
	{ "ssp0", 0x80010000, 0x070, 0x080 },
	{ "ssp1", 0x80012000, 0x000, 0x000 },
	{ "ssp1", 0x80012000, 0x070, 0x080 },
	{ "i2c1", 0x8005a000, 0x000, 0x040 },
	{ "pwm", 0x80064000, 0x000, 0x0a0 },
};

static u32 *snapshot_words(void)
{
	return (u32 *)(SNAPSHOT_ADDRESS + 4);
}

void brain_snapshot_eboot_hardware(void)
{
	u32 *words = snapshot_words();
	unsigned int range_index;
	u32 offset;

	for (range_index = 0; range_index < ARRAY_SIZE(snapshot_ranges); range_index++) {
		const struct register_range *range = &snapshot_ranges[range_index];

		for (offset = range->first; offset <= range->last; offset += 0x10)
			*words++ = readl(range->base + offset);
	}
	writel(SNAPSHOT_MAGIC, SNAPSHOT_ADDRESS);
}

int brain_eboot_register(u32 address, u32 *value)
{
	u32 *words = snapshot_words();
	unsigned int range_index;
	u32 offset;

	if (readl(SNAPSHOT_ADDRESS) != SNAPSHOT_MAGIC)
		return -ENOENT;
	for (range_index = 0; range_index < ARRAY_SIZE(snapshot_ranges); range_index++) {
		const struct register_range *range = &snapshot_ranges[range_index];

		for (offset = range->first; offset <= range->last; offset += 0x10, words++) {
			if (range->base + offset == address) {
				*value = *words;
				return 0;
			}
		}
	}
	return -ENOENT;
}

void brain_report_hardware_changes(void)
{
	u32 *words = snapshot_words();
	unsigned int range_index, changes = 0;
	u32 offset, now;

	if (readl(SNAPSHOT_ADDRESS) != SNAPSHOT_MAGIC) {
		printf("hardware: no EBOOT snapshot\n");
		return;
	}
	for (range_index = 0; range_index < ARRAY_SIZE(snapshot_ranges); range_index++) {
		const struct register_range *range = &snapshot_ranges[range_index];

		for (offset = range->first; offset <= range->last; offset += 0x10, words++) {
			now = readl(range->base + offset);
			if (now != *words) {
				printf("hardware: %s %08x eboot %08x now %08x\n",
				       range->name, range->base + offset, *words, now);
				changes++;
			}
		}
	}
	printf("hardware: %u registers differ from EBOOT\n", changes);
}

static int do_hwdiff(struct cmd_tbl *command, int flag, int argc, char *const argv[])
{
	brain_report_hardware_changes();
	return CMD_RET_SUCCESS;
}

U_BOOT_CMD(hwdiff, 1, 0, do_hwdiff,
	   "list registers that differ from the state EBOOT left",
	   "");
