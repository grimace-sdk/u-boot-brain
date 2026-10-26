// SPDX-License-Identifier: GPL-2.0+

#include <common.h>
#include <command.h>
#include <console.h>
#include <fs.h>
#include <malloc.h>
#include <mapmem.h>
#include "brain.h"

#define BOOT_LOG_FILE		CARD_DIRECTORY "/uboot.log"
#define BOOT_LOG_MARKER		CARD_DIRECTORY "/log"
#define BOOT_LOG_LIMIT		(64 * 1024)
#define BOOT_LOG_LINE		256

static char *boot_log;
static int boot_log_length;

static void collect_console_record(void)
{
	char line[BOOT_LOG_LINE];
	int length;

	if (!boot_log)
		boot_log = malloc(BOOT_LOG_LIMIT);
	if (!boot_log)
		return;
	while (console_record_avail() > 0) {
		length = console_record_readline(line, sizeof(line));
		if (length < 0)
			break;
		if (boot_log_length + length + 1 > BOOT_LOG_LIMIT)
			break;
		memcpy(boot_log + boot_log_length, line, length);
		boot_log_length += length;
		boot_log[boot_log_length++] = '\n';
	}
}

bool brain_boot_log_requested(void)
{
	if (fs_set_blk_dev("mmc", CARD_DEVICE, FS_TYPE_FAT))
		return false;
	return fs_exists(BOOT_LOG_MARKER);
}

int brain_save_boot_log(void)
{
	loff_t written;

	collect_console_record();
	if (!boot_log || !boot_log_length)
		return -ENOENT;
	if (fs_set_blk_dev("mmc", CARD_DEVICE, FS_TYPE_FAT))
		return -ENODEV;
	return fs_write(BOOT_LOG_FILE, map_to_sysmem(boot_log), 0, boot_log_length, &written);
}

static int do_savelog(struct cmd_tbl *command, int flag, int argc, char *const argv[])
{
	int result = brain_save_boot_log();

	if (result)
		printf("savelog: failed (%d)\n", result);
	return result ? CMD_RET_FAILURE : CMD_RET_SUCCESS;
}

U_BOOT_CMD(savelog, 1, 0, do_savelog,
	   "save recorded console output to " BOOT_LOG_FILE " on the SD card",
	   "");
