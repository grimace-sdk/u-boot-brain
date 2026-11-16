// SPDX-License-Identifier: GPL-2.0+

#include <common.h>
#include <blk.h>
#include <command.h>
#include <cpu_func.h>
#include <console.h>
#include <env.h>
#include <irq_func.h>
#include <mmc.h>
#include <memalign.h>
#include <fs.h>
#include <mapmem.h>
#include <asm/io.h>
#include <asm/arch/clock.h>
#include <asm/arch/sys_proto.h>
#include <asm/arch/imx-regs.h>
#include <asm/unaligned.h>
#include "brain.h"

#define SHARP_CE_MMC_DEVICE		0
#define SHARP_CE_PARTITION_TYPE		0x10
#define SHARP_CE_PARTITION_INDEX	2
#define SECTOR_SIZE			512
#define SLOT_SECTORS			(32 * 1024 * 1024 / SECTOR_SIZE)
#define TRAILER_OFFSET_SECTORS		(30 * 1024 * 1024 / SECTOR_SIZE - 1)
#define TRAILER_MAGIC			"06732754"
#define TRAILER_MAGIC_LENGTH		8
#define SHARP_CE_LOAD_ADDRESS		0x40200000
#define SHARP_CE_SIGNATURE_OFFSET	0x40
#define BSP_ARGS_INTERNAL_BOOT		CONFIG_SHARP_WINCE_BSP_ARGS
#define EBOOT_SCRATCH_START		0x400C1000
#define EBOOT_SCRATCH_END		0x400E0000
#define PROGRESS_CHUNK_SECTORS		6144
#define READ_BYTES_PER_US		17
#define CLEAR_BYTES_PER_US		400
#define MAXIMUM_SKIP_SPANS		64
#define GENERATE_READ_UNITS		35
#define GENERATE_RUN_UNITS		40
#define GENERATE_SAVE_UNITS		15
#define GENERATE_PATCH_UNITS		10
#ifdef CONFIG_SHARP_WINCE_DEBUG_LOG
#define CE_DEBUG_LOG_ADDRESS		0x40180000
#define CE_DEBUG_LOG_MAGIC		0x474c4543
#define CE_DEBUG_LOG_BYTES		0x10000
#define CE_DEBUG_LOG_HEADER_BYTES	16
#define CE_DEBUG_LOG_STAGING		0x45800000
#define CE_DEBUG_LOG_FILE		CARD_DIRECTORY "/ce-debug.log"
#define CE_DEBUG_LOG_PREVIOUS_FILE	CARD_DIRECTORY "/ce-debug-prev.log"

struct ce_debug_log {
	u32 magic;
	u32 written;
	u32 capacity;
	u32 reserved;
	u8 text[];
};
#endif
#define DUMP_MARKER			CARD_DIRECTORY "/dump-nk"
#define DUMP_NAME_LENGTH		64

#define CLKCTRL_BASE		0x80040000
#define CLKCTRL_CPU		(CLKCTRL_BASE + 0x050)
#define CLKCTRL_SSP0		(CLKCTRL_BASE + 0x090)
#define CLKCTRL_SSP1		(CLKCTRL_BASE + 0x0a0)
#define CLKCTRL_GPMI		(CLKCTRL_BASE + 0x0d0)
#define CLKCTRL_FRAC0		(CLKCTRL_BASE + 0x1b0)
#define CLKCTRL_CLKSEQ		(CLKCTRL_BASE + 0x1d0)
#define REGISTER_SET		0x4
#define REGISTER_CLEAR		0x8
#define CLOCK_BUSY_BITS		0x30000000
#define APBH_CTRL0		0x80004000

static void wait_clock_idle(u32 address)
{
	unsigned int attempts;

	for (attempts = 0; attempts < 100000 && (readl(address) & CLOCK_BUSY_BITS); attempts++)
		;
}

static void restore_clock_register(u32 address)
{
	u32 eboot_value;

	if (brain_eboot_register(address, &eboot_value))
		return;
	writel(eboot_value, address);
	wait_clock_idle(address);
}

static void restore_eboot_clocks(void)
{
	u32 eboot_sequence, eboot_fractions;

	if (!brain_eboot_register(CLKCTRL_CLKSEQ, &eboot_sequence)) {
		writel(eboot_sequence, CLKCTRL_CLKSEQ + REGISTER_SET);
		writel(~eboot_sequence, CLKCTRL_CLKSEQ + REGISTER_CLEAR);
	}
	restore_clock_register(CLKCTRL_SSP0);
	restore_clock_register(CLKCTRL_SSP1);
	restore_clock_register(CLKCTRL_GPMI);
	restore_clock_register(CLKCTRL_CPU);
	if (!brain_eboot_register(CLKCTRL_FRAC0, &eboot_fractions)) {
		writeb((eboot_fractions >> 16) & 0xff, CLKCTRL_FRAC0 + 2);
		writeb((eboot_fractions >> 24) & 0xff, CLKCTRL_FRAC0 + 3);
	}
	restore_clock_register(APBH_CTRL0);
}

struct sharp_ce_image {
	lbaint_t start_sector;
	u32 length;
	char slot;
};

static int read_trailer(struct blk_desc *device, lbaint_t sector, u64 partition_bytes, u32 *length)
{
	ALLOC_CACHE_ALIGN_BUFFER(u8, buffer, SECTOR_SIZE);
	u8 *trailer = buffer + SECTOR_SIZE - 12;

	if (blk_dread(device, sector, 1, buffer) != 1)
		return -EIO;
	if (memcmp(trailer, TRAILER_MAGIC, TRAILER_MAGIC_LENGTH))
		return -ENOENT;
	*length = get_unaligned_le32(trailer + TRAILER_MAGIC_LENGTH);
	if (!*length || *length > partition_bytes)
		return -EINVAL;
	return 0;
}

static int find_sharp_ce_image(struct blk_desc *device, struct sharp_ce_image *image)
{
	ALLOC_CACHE_ALIGN_BUFFER(u8, master_boot_record, SECTOR_SIZE);
	u8 *entry;
	lbaint_t partition_start;
	u64 partition_bytes;

	if (blk_dread(device, 0, 1, master_boot_record) != 1)
		return -EIO;
	if (master_boot_record[510] != 0x55 || master_boot_record[511] != 0xaa)
		return -ENOENT;
	entry = master_boot_record + 0x1be + SHARP_CE_PARTITION_INDEX * 16;
	if (entry[4] != SHARP_CE_PARTITION_TYPE)
		return -ENOENT;
	partition_start = get_unaligned_le32(entry + 8);
	partition_bytes = (u64)get_unaligned_le32(entry + 12) * SECTOR_SIZE;

	image->start_sector = partition_start;
	image->slot = 'A';
	if (!read_trailer(device, partition_start + TRAILER_OFFSET_SECTORS, partition_bytes, &image->length))
		return 0;
	if (!IS_ENABLED(CONFIG_SHARP_WINCE_SLOT_B))
		return -ENOENT;
	image->start_sector = partition_start + SLOT_SECTORS;
	image->slot = 'B';
	if (!read_trailer(device, partition_start + SLOT_SECTORS + TRAILER_OFFSET_SECTORS, partition_bytes, &image->length))
		return 0;
	return -ENOENT;
}

static unsigned long count_nonzero_words(const u32 *words, unsigned long count)
{
	unsigned long nonzero = 0;

	while (count--)
		if (*words++)
			nonzero++;
	return nonzero;
}

struct slot_reader {
	struct blk_desc *device;
	lbaint_t start_sector;
};

static int read_slot_sectors(void *context, u32 first_sector, u32 sector_count, void *buffer)
{
	struct slot_reader *reader = context;

	if (blk_dread(reader->device, reader->start_sector + first_sector, sector_count, buffer) != sector_count)
		return -EIO;
	return 0;
}

static int read_identity(struct blk_desc *device, const struct sharp_ce_image *image, u32 *identity)
{
	struct slot_reader reader = { device, image->start_sector };
	unsigned long started = get_timer(0);
	int result = brain_image_identity(read_slot_sectors, &reader, image->length, identity);

	printf("Windows CE: identity %08x, result %d, %lu ms\n", result ? 0 : *identity, result, get_timer(started));
	return result;
}

static int read_sectors(struct blk_desc *device, const struct sharp_ce_image *image, lbaint_t sectors)
{
	lbaint_t done, chunk;

	for (done = 0; done < sectors; done += chunk) {
		chunk = min((lbaint_t)PROGRESS_CHUNK_SECTORS, sectors - done);
		brain_progress_update(done, sectors);
		if (blk_dread(device, image->start_sector + done, chunk,
			      (void *)(SHARP_CE_LOAD_ADDRESS + done * SECTOR_SIZE)) != chunk)
			return -EIO;
	}
	brain_progress_update(sectors, sectors);
	return 0;
}

static int read_image(struct blk_desc *device, const struct sharp_ce_image *image, lbaint_t sectors)
{
	brain_progress_begin(sectors);
	if (read_sectors(device, image, sectors))
		return -EIO;
	brain_progress_end();
	return 0;
}

static int dump_image(struct blk_desc *device, const struct sharp_ce_image *image)
{
	char name[DUMP_NAME_LENGTH];
	loff_t written;
	int result;

	if (read_image(device, image, DIV_ROUND_UP(image->length, SECTOR_SIZE)))
		return -EIO;
	snprintf(name, sizeof(name), CARD_DIRECTORY "/nk-slot-%c.bin", image->slot);
	if (!fs_set_blk_dev("mmc", CARD_DEVICE, FS_TYPE_FAT))
		fs_mkdir(CARD_DIRECTORY);
	if (fs_set_blk_dev("mmc", CARD_DEVICE, FS_TYPE_FAT))
		return -ENODEV;
	result = fs_write(name, SHARP_CE_LOAD_ADDRESS, 0, image->length, &written);
	if (result || written != image->length)
		return result ? result : -EIO;
	printf("Windows CE: slot %c written to %s\n", image->slot, name);
	return 0;
}

static bool dump_requested(void)
{
	if (fs_set_blk_dev("mmc", CARD_DEVICE, FS_TYPE_FAT))
		return false;
	return fs_exists(DUMP_MARKER);
}

static void dump_on_request(struct blk_desc *device, const struct sharp_ce_image *image)
{
	if (!dump_requested())
		return;
	if (dump_image(device, image)) {
		printf("bootsharpce: writing the image to the card failed\n");
		return;
	}
	if (!fs_set_blk_dev("mmc", CARD_DEVICE, FS_TYPE_FAT))
		fs_unlink(DUMP_MARKER);
}

#ifdef CONFIG_SHARP_WINCE_DEBUG_LOG
static void keep_previous_ce_debug_log(u8 *staging)
{
	loff_t size = 0, written;

	if (fs_set_blk_dev("mmc", CARD_DEVICE, FS_TYPE_FAT) ||
	    fs_read(CE_DEBUG_LOG_FILE, map_to_sysmem(staging), 0, 0, &size) || !size)
		return;
	if (!fs_set_blk_dev("mmc", CARD_DEVICE, FS_TYPE_FAT))
		fs_write(CE_DEBUG_LOG_PREVIOUS_FILE, map_to_sysmem(staging), 0, size, &written);
}

static void save_ce_debug_log(void)
{
	struct ce_debug_log *log = map_sysmem(CE_DEBUG_LOG_ADDRESS, CE_DEBUG_LOG_BYTES);
	u8 *staging = map_sysmem(CE_DEBUG_LOG_STAGING, CE_DEBUG_LOG_BYTES);
	u32 capacity = CE_DEBUG_LOG_BYTES - CE_DEBUG_LOG_HEADER_BYTES;
	u32 length, oldest;
	loff_t written;
	int result;

	if (log->magic != CE_DEBUG_LOG_MAGIC || log->capacity != capacity || !log->written)
		return;
	keep_previous_ce_debug_log(staging);
	if (log->written <= capacity) {
		length = log->written;
		memcpy(staging, log->text, length);
	} else {
		length = capacity;
		oldest = log->written % capacity;
		memcpy(staging, log->text + oldest, capacity - oldest);
		memcpy(staging + capacity - oldest, log->text, oldest);
	}
	result = fs_set_blk_dev("mmc", CARD_DEVICE, FS_TYPE_FAT);
	if (!result)
		result = fs_write(CE_DEBUG_LOG_FILE, map_to_sysmem(staging), 0, length, &written);
	printf("CE debug log: %u bytes written by CE, %u saved, result %d\n", log->written, length, result);
	log->written = 0;
}
#else
static void save_ce_debug_log(void)
{
}
#endif

static u32 load_cost(lbaint_t read_sectors, lbaint_t cleared_sectors)
{
	return (u64)read_sectors * SECTOR_SIZE / READ_BYTES_PER_US +
	       (u64)cleared_sectors * SECTOR_SIZE / CLEAR_BYTES_PER_US;
}

static lbaint_t skipped_sectors(const struct nk_skip_span *spans, unsigned int span_count)
{
	lbaint_t sectors = 0;
	unsigned int index;

	for (index = 0; index < span_count; index++)
		sectors += (spans[index].end - spans[index].start) / SECTOR_SIZE;
	return sectors;
}

static int load_with_skips(struct blk_desc *device, const struct sharp_ce_image *image, struct mmc *mmc, bool logging)
{
	lbaint_t sectors = DIV_ROUND_UP(image->length, SECTOR_SIZE);
	lbaint_t done, chunk, skipped, planned_skip;
	struct nk_skip_span spans[MAXIMUM_SKIP_SPANS];
	unsigned int span_count, span_index;
	u32 read_cost, patch_cost;
	unsigned long started, clearing_started, clearing_time, retained_words;
	void *span_address;

	started = get_timer(0);
	span_count = brain_nk_bundle_skip_spans(image->length, spans, MAXIMUM_SKIP_SPANS);
	planned_skip = skipped_sectors(spans, span_count);
	read_cost = load_cost(sectors - planned_skip, planned_skip);
	patch_cost = brain_nk_bundle_cost();
	brain_progress_begin(read_cost + patch_cost);
	brain_progress_step(read_cost);
	span_index = 0;
	skipped = 0;
	clearing_time = 0;
	retained_words = 0;
	for (done = 0; done < sectors; done += chunk) {
		while (span_index < span_count && spans[span_index].end / SECTOR_SIZE <= done)
			span_index++;
		if (span_index < span_count && spans[span_index].start / SECTOR_SIZE <= done) {
			chunk = spans[span_index].end / SECTOR_SIZE - done;
			span_address = (void *)(SHARP_CE_LOAD_ADDRESS + done * SECTOR_SIZE);
			if (logging)
				retained_words += count_nonzero_words(span_address, chunk * SECTOR_SIZE / 4);
			clearing_started = get_timer(0);
			memset(span_address, 0, chunk * SECTOR_SIZE);
			clearing_time += get_timer(clearing_started);
			skipped += chunk;
			brain_progress_update(load_cost(done + chunk - skipped, skipped), read_cost);
			continue;
		}
		chunk = min((lbaint_t)PROGRESS_CHUNK_SECTORS, sectors - done);
		if (span_index < span_count)
			chunk = min(chunk, (lbaint_t)(spans[span_index].start / SECTOR_SIZE) - done);
		brain_progress_update(load_cost(done - skipped, skipped), read_cost);
		if (blk_dread(device, image->start_sector + done, chunk,
			      (void *)(SHARP_CE_LOAD_ADDRESS + done * SECTOR_SIZE)) != chunk)
			return -EIO;
	}
	brain_progress_update(read_cost, read_cost);
	printf("Windows CE: read in %lu ms at %u Hz, %u spans (%lu KB) skipped and cleared in %lu ms\n",
	       get_timer(started), mmc->clock, span_count, (unsigned long)(skipped / 2), clearing_time);
	if (logging)
		printf("Windows CE: %lu KB of the skipped spans held data before clearing\n", retained_words / 256);
	brain_progress_step(patch_cost);
	return 0;
}

static void clear_skip_spans(u32 image_length)
{
	struct nk_skip_span spans[MAXIMUM_SKIP_SPANS];
	unsigned int span_count = brain_nk_bundle_skip_spans(image_length, spans, MAXIMUM_SKIP_SPANS);
	unsigned int index;

	for (index = 0; index < span_count; index++)
		memset((void *)(SHARP_CE_LOAD_ADDRESS + spans[index].start), 0, spans[index].end - spans[index].start);
	printf("Windows CE: %u spans cleared after generation\n", span_count);
}

static int load_and_generate(struct blk_desc *device, const struct sharp_ce_image *image)
{
	unsigned long started = get_timer(0);

	brain_progress_begin(GENERATE_READ_UNITS + GENERATE_RUN_UNITS + GENERATE_SAVE_UNITS + GENERATE_PATCH_UNITS);
	brain_progress_stage("First start: reading Windows CE");
	brain_progress_step(GENERATE_READ_UNITS);
	if (read_sectors(device, image, DIV_ROUND_UP(image->length, SECTOR_SIZE)))
		return -EIO;
	printf("Windows CE: whole image read in %lu ms for the generator\n", get_timer(started));
	brain_progress_step(GENERATE_RUN_UNITS);
	if (brain_nkgen_generate((const u8 *)SHARP_CE_LOAD_ADDRESS, image->length)) {
		brain_bundle_fallback(image->length);
		brain_progress_step(GENERATE_SAVE_UNITS);
	} else {
		brain_progress_step(GENERATE_SAVE_UNITS);
		brain_nkgen_save();
	}
	clear_skip_spans(image->length);
	brain_progress_stage("Patching");
	brain_progress_step(GENERATE_PATCH_UNITS);
	return 0;
}

int brain_nkgen_enter(ulong start, ulong length, ulong entry, struct nkgen_call *call)
{
	int (*generate)(struct nkgen_call *call) = (int (*)(struct nkgen_call *))entry;

	flush_dcache_range(start, start + ALIGN(length, ARCH_DMA_MINALIGN));
	invalidate_icache_all();
	return generate(call);
}

static int do_bootsharpce(struct cmd_tbl *command, int flag, int argc, char *const argv[])
{
	struct sharp_ce_image image;
	struct blk_desc *device;
	struct mmc *mmc;
	enum brain_bundle_choice choice = BRAIN_BUNDLE_READY;
	unsigned long started;
	bool logging, identified;
	u32 identity;
	int result;
	const char *action = argc > 1 ? argv[1] : "";
	void (*kernel_entry)(void) = (void (*)(void))SHARP_CE_LOAD_ADDRESS;

	mmc = find_mmc_device(SHARP_CE_MMC_DEVICE);
	if (!mmc || mmc_init(mmc)) {
		printf("bootsharpce: internal MMC unavailable\n");
		return CMD_RET_FAILURE;
	}
	device = mmc_get_blk_desc(mmc);
	if (find_sharp_ce_image(device, &image)) {
		printf("bootsharpce: no valid Windows CE image on the internal MMC\n");
		return CMD_RET_FAILURE;
	}
	printf("Windows CE: slot %c, sector 0x" LBAF ", %u bytes\n", image.slot, image.start_sector, image.length);
	identified = !read_identity(device, &image, &identity);
	if (!strcmp(action, "info"))
		return CMD_RET_SUCCESS;
	if (!strcmp(action, "dump"))
		return dump_image(device, &image) ? CMD_RET_FAILURE : CMD_RET_SUCCESS;
	if (strcmp(action, "prepare")) {
		dump_on_request(device, &image);
		save_ce_debug_log();
	}
	logging = strcmp(action, "prepare") && brain_boot_log_requested();

	brain_boot_status_begin(image.length, identified ? &identity : NULL);
	brain_nk_bundle_clear();
	if (strcmp(action, "stock"))
		choice = brain_bundle_choose(image.length, !strcmp(action, "generate"));
	if (choice == BRAIN_BUNDLE_GENERATE)
		result = load_and_generate(device, &image);
	else
		result = load_with_skips(device, &image, mmc, logging);
	if (result) {
		printf("bootsharpce: read failed\n");
		return CMD_RET_FAILURE;
	}
	if (memcmp((void *)(SHARP_CE_LOAD_ADDRESS + SHARP_CE_SIGNATURE_OFFSET), "ECEC", 4)) {
		printf("bootsharpce: image has no ECEC signature\n");
		return CMD_RET_FAILURE;
	}

	started = get_timer(0);
	brain_nk_bundle_apply((u8 *)SHARP_CE_LOAD_ADDRESS, image.length);
	brain_progress_end();
	printf("nk patches: %lu ms\n", get_timer(started));
	if (!strcmp(action, "prepare")) {
		printf("Windows CE prepared in RAM; not started\n");
		return CMD_RET_SUCCESS;
	}
	brain_boot_status_save();
	writel(1, BSP_ARGS_INTERNAL_BOOT);
	memset((void *)EBOOT_SCRATCH_START, 0, EBOOT_SCRATCH_END - EBOOT_SCRATCH_START);
	brain_report_hardware_changes();
	brain_display_report();
	printf("Starting Windows CE...\n");
	if (logging)
		brain_save_boot_log();
	restore_eboot_clocks();
	disable_interrupts();
	cleanup_before_linux();
	kernel_entry();
	return CMD_RET_FAILURE;
}

U_BOOT_CMD(bootsharpce, 2, 0, do_bootsharpce,
	   "boot Sharp's Windows CE from the internal MMC (read only)",
	   "[info|dump|stock|generate]\n"
	   "    - load the slot EBOOT would boot, patch it from " CARD_DIRECTORY "/nk-<identity>/boot.nkp (made by\n"
	   "      " CARD_DIRECTORY "/nkgen.bin when missing), nk-<length>/boot.nkp or boot.nkp, and start it\n"
	   "    - 'stock' starts it unpatched; 'generate' remakes nk-<identity> first\n"
	   "    - 'info' only reports the slot and identity\n"
	   "    - 'dump' writes it to " CARD_DIRECTORY "/nk-slot-<slot>.bin on the card\n"
	   "    - with " DUMP_MARKER " on the card, the slot is written there once before booting");

static void set_boot_command(void)
{
	if (!env_get("linux_boot"))
		env_set("linux_boot", env_get("bootcmd"));
	env_set("bootcmd", CONFIG_SHARP_WINCE_BOOTCMD);
}

__weak void brain_display_report(void)
{
}

__weak void brain_board_late_init(void)
{
}

int board_late_init(void)
{
	console_record_reset_enable();
	brain_board_late_init();
	set_boot_command();
	return 0;
}
