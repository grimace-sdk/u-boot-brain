// SPDX-License-Identifier: GPL-2.0+

#include <common.h>
#include <fs.h>
#include <malloc.h>
#include <mapmem.h>
#include <memalign.h>
#include <u-boot/crc.h>
#include "brain.h"

#define NKGEN_FILE		CARD_DIRECTORY "/nkgen.bin"
#define NKGEN_PAK_FILE		CARD_DIRECTORY "/nkgen.pak"
#define NO_NKGEN_MARKER		CARD_DIRECTORY "/no-nkgen"
#define BOOT_STATUS_FILE	CARD_DIRECTORY "/boot-status.txt"
#define CACHE_DIRECTORY		CARD_DIRECTORY "/nk-%08x"
#define CACHE_STAMP_NAME	"cache.txt"
#define GENERATOR_LOG_NAME	"nkgen.log"
#define BUNDLE_NAME		"boot.nkp"

#define NKGEN_HEADER_MAGIC	"NKGENBIN"
#define NKGEN_HEADER_VERSION	1
#define NKGEN_ABI_VERSION	2
#define NKGEN_HEADER_SIZE	72
#define NKGEN_VERSION_OFFSET	40
#define NKGEN_VERSION_LENGTH	32
#define NKGEN_CALL_MAGIC	0x4347484e
#define NKGEN_FLAG_SKIP		2
#define PAK_MAGIC		"NKGENPAK"
#define PAK_HEADER_SIZE		80
#define PAK_CHECKSUM_START	20

#define NKGEN_ADDRESS		CONFIG_SHARP_WINCE_NKGEN_ADDRESS
#define NKGEN_PAK_ADDRESS	CONFIG_SHARP_WINCE_NKGEN_PAK_ADDRESS
#define NKGEN_ARENA_ADDRESS	CONFIG_SHARP_WINCE_NKGEN_ARENA_ADDRESS
#define NKGEN_ARENA_SIZE	CONFIG_SHARP_WINCE_NKGEN_ARENA_SIZE
#define NKGEN_REGION		(NKGEN_PAK_ADDRESS - NKGEN_ADDRESS)
#define NKGEN_PAK_REGION	(NKGEN_ARENA_ADDRESS - NKGEN_PAK_ADDRESS)

#define SECTOR_SIZE		512
#define IMAGE_BASE		0x80200000
#define SIGNATURE_OFFSET	0x40
#define TOC_POINTER_OFFSET	0x44
#define ROM_HEADER_SIZE		84
#define MODULE_COUNT_OFFSET	16
#define FILE_COUNT_OFFSET	48
#define MODULE_ENTRY_SIZE	32
#define FILE_ENTRY_SIZE		28
#define IDENTITY_FIRST_SECTORS	32
#define IDENTITY_LIMIT		0x10000

#define PATH_LENGTH		128
#define LOG_LIMIT		0x10000
#define STEPS_LIMIT		0x1000
#define STAMP_LIMIT		0x4000
#define STATUS_LIMIT		0x2000
#define GENERATOR_UNITS		1000

struct nkgen_output {
	const char *path;
	const u8 *data;
	u32 length;
	u32 crc32;
};

struct nkgen_step {
	const char *name;
	s32 result;
	const char *detail;
};

struct nkgen_call {
	u32 magic;
	u32 abi_version;
	u32 size;
	const u8 *image;
	u32 image_length;
	const u8 *pak;
	u32 pak_length;
	u8 *arena;
	u32 arena_length;
	u32 flags;
	void (*progress)(void *context, const char *stage, u32 done, u32 total);
	void (*log)(void *context, const char *line);
	void *context;
	s32 result;
	u32 output_count;
	const struct nkgen_output *outputs;
	u32 step_count;
	const struct nkgen_step *steps;
	u32 known;
	const char *profile;
	const char *device;
	const char *release;
	u32 identity_crc32;
	u8 image_sha256[32];
	char message[160];
};

struct generator_stage {
	const char *name;
	u32 start;
	u32 share;
};

static const struct generator_stage generator_stages[] = {
	{ "Identifying image", 0, 600 },
	{ "Extracting modules", 600, 100 },
	{ "Merging registry", 700, 100 },
	{ "Building patches", 800, 200 },
};

struct boot_status {
	bool identified;
	u32 identity;
	u32 image_length;
	char folder[PATH_LENGTH];
	const char *image;
	char profile[32];
	char device[32];
	char sha256[65];
	const char *bundle;
	char generator[NKGEN_VERSION_LENGTH + 1];
	char release[64];
	char error[160];
	char *steps;
	size_t steps_length;
};

struct cache_stamp {
	char generator[NKGEN_VERSION_LENGTH + 1];
	u32 pak_crc;
	bool has_bundle_line;
	u32 bundle_length;
	u32 bundle_crc;
};

static struct boot_status status;
static struct nkgen_call call;
static struct brain_ram_file *generated_files;
static char generator_version[NKGEN_VERSION_LENGTH + 1];
static u32 pak_crc;
static u32 pak_length;
static char *generator_log;
static size_t generator_log_length;
static const struct generator_stage *current_stage;
static unsigned long generated_ms;

static u32 le32(const u8 *bytes)
{
	return bytes[0] | bytes[1] << 8 | bytes[2] << 16 | (u32)bytes[3] << 24;
}

static void copy_text(char *destination, size_t size, const char *source)
{
	snprintf(destination, size, "%s", source ? source : "");
}

static void append_bytes(char **buffer, size_t *length, size_t limit, const char *bytes, size_t count)
{
	if (!*buffer)
		*buffer = malloc(limit);
	if (!*buffer || *length + count + 1 > limit)
		return;
	memcpy(*buffer + *length, bytes, count);
	*length += count;
	(*buffer)[*length] = 0;
}

static void append_text(char **buffer, size_t *length, size_t limit, const char *text)
{
	size_t text_length = strlen(text);

	if (*length + text_length + 2 > limit)
		return;
	append_bytes(buffer, length, limit, text, text_length);
	append_bytes(buffer, length, limit, "\n", 1);
}

static void log_line(const char *line)
{
	printf("nkgen: %s\n", line);
	append_text(&generator_log, &generator_log_length, LOG_LIMIT, line);
}

static int card_select(void)
{
	return fs_set_blk_dev("mmc", CARD_DEVICE, FS_TYPE_FAT);
}

static bool card_exists(const char *path)
{
	return !card_select() && fs_exists(path);
}

static int card_read(const char *path, void *buffer, loff_t length, loff_t *read)
{
	*read = 0;
	if (card_select())
		return -ENODEV;
	return fs_read(path, map_to_sysmem(buffer), 0, length, read);
}

static int card_size(const char *path, loff_t *size)
{
	*size = 0;
	if (card_select())
		return -ENODEV;
	return fs_size(path, size);
}

static void card_make_directories(const char *path)
{
	char directory[PATH_LENGTH];
	const char *slash;

	for (slash = strchr(path, '/'); slash; slash = strchr(slash + 1, '/')) {
		snprintf(directory, sizeof(directory), "%.*s", (int)(slash - path), path);
		if (!card_select() && !fs_exists(directory) && !card_select())
			fs_mkdir(directory);
	}
}

static int card_write(const char *path, const void *data, u32 length)
{
	loff_t written = 0;
	int result;

	card_make_directories(path);
	result = card_select();
	if (!result)
		result = fs_write(path, map_to_sysmem(data), 0, length, &written);
	if (!result && written != length)
		result = -EIO;
	return result;
}

static void card_unlink(const char *path)
{
	if (!card_select())
		fs_unlink(path);
}

static void folder_path(char *path, size_t size, const char *name)
{
	snprintf(path, size, "%s/%s", status.folder, name);
}

static bool in_folder(const char *path)
{
	size_t length = strlen(status.folder);

	return !strncmp(path, status.folder, length) && path[length] == '/';
}

int brain_image_identity(brain_sector_reader read, void *context, u32 image_length, u32 *identity)
{
	u32 image_sectors = DIV_ROUND_UP(image_length, SECTOR_SIZE);
	u32 header_offset, first_sector, within, range, sectors, first_read;
	u8 *buffer;
	int result = -EINVAL;

	buffer = memalign(ARCH_DMA_MINALIGN, IDENTITY_LIMIT + 2 * SECTOR_SIZE);
	if (!buffer)
		return -ENOMEM;
	if (read(context, 0, 1, buffer)) {
		result = -EIO;
		goto done;
	}
	if (memcmp(buffer + SIGNATURE_OFFSET, "ECEC", 4))
		goto done;
	header_offset = le32(buffer + TOC_POINTER_OFFSET) - IMAGE_BASE;
	if (le32(buffer + TOC_POINTER_OFFSET) < IMAGE_BASE || header_offset > image_length ||
	    image_length - header_offset < ROM_HEADER_SIZE)
		goto done;
	first_sector = header_offset / SECTOR_SIZE;
	within = header_offset % SECTOR_SIZE;
	first_read = min((u32)IDENTITY_FIRST_SECTORS, image_sectors - first_sector);
	if (read(context, first_sector, first_read, buffer)) {
		result = -EIO;
		goto done;
	}
	range = le32(buffer + within + MODULE_COUNT_OFFSET);
	if (range > IDENTITY_LIMIT / MODULE_ENTRY_SIZE)
		goto done;
	range = ROM_HEADER_SIZE + range * MODULE_ENTRY_SIZE;
	if (le32(buffer + within + FILE_COUNT_OFFSET) > IDENTITY_LIMIT / FILE_ENTRY_SIZE)
		goto done;
	range += le32(buffer + within + FILE_COUNT_OFFSET) * FILE_ENTRY_SIZE;
	if (range > IDENTITY_LIMIT || range > image_length - header_offset)
		goto done;
	sectors = DIV_ROUND_UP(within + range, SECTOR_SIZE);
	if (sectors > first_read &&
	    read(context, first_sector + first_read, sectors - first_read, buffer + first_read * SECTOR_SIZE)) {
		result = -EIO;
		goto done;
	}
	*identity = crc32(0, buffer + within, range);
	result = 0;
done:
	free(buffer);
	return result;
}

static void steps_reset(void)
{
	status.steps_length = 0;
	if (status.steps)
		status.steps[0] = 0;
}

void brain_boot_status_begin(u32 image_length, const u32 *identity)
{
	char *steps = status.steps;

	memset(&status, 0, sizeof(status));
	status.steps = steps;
	status.image_length = image_length;
	status.image = "";
	status.bundle = "none";
	steps_reset();
	if (identity) {
		status.identified = true;
		status.identity = *identity;
		snprintf(status.folder, sizeof(status.folder), CACHE_DIRECTORY, *identity);
	}
}

static void set_error(const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(status.error, sizeof(status.error), format, arguments);
	va_end(arguments);
	log_line(status.error);
}

static bool generation_allowed(void)
{
	if (card_exists(NO_NKGEN_MARKER)) {
		printf("nkgen: " NO_NKGEN_MARKER " present, not generating\n");
		return false;
	}
	return true;
}

static int read_generator_identity(char *version, u32 *crc)
{
	ALLOC_CACHE_ALIGN_BUFFER(u8, header, NKGEN_HEADER_SIZE);
	loff_t read;

	if (card_read(NKGEN_FILE, header, NKGEN_HEADER_SIZE, &read) || read != NKGEN_HEADER_SIZE)
		return -ENOENT;
	snprintf(version, NKGEN_VERSION_LENGTH + 1, "%.*s", NKGEN_VERSION_LENGTH, header + NKGEN_VERSION_OFFSET);
	if (card_read(NKGEN_PAK_FILE, header, PAK_CHECKSUM_START, &read) || read != PAK_CHECKSUM_START)
		return -ENOENT;
	*crc = le32(header + 16);
	return 0;
}

static void read_stamp_line(struct cache_stamp *stamp, const char *line, size_t length)
{
	char bundle_path[PATH_LENGTH];
	char text[256];
	char *value;

	snprintf(text, sizeof(text), "%.*s", (int)length, line);
	value = strchr(text, ' ');
	if (!value)
		return;
	*value++ = 0;
	folder_path(bundle_path, sizeof(bundle_path), BUNDLE_NAME);
	if (!strcmp(text, "generator")) {
		copy_text(stamp->generator, sizeof(stamp->generator), value);
		copy_text(status.generator, sizeof(status.generator), value);
	} else if (!strcmp(text, "pak")) {
		stamp->pak_crc = simple_strtoul(value, NULL, 16);
	} else if (!strcmp(text, "release")) {
		copy_text(status.release, sizeof(status.release), value);
	} else if (!strcmp(text, "image-sha256")) {
		copy_text(status.sha256, sizeof(status.sha256), value);
	} else if (!strcmp(text, "known")) {
		status.image = strcmp(value, "yes") ? "unknown" : "known";
	} else if (!strcmp(text, "profile")) {
		copy_text(status.profile, sizeof(status.profile), value);
	} else if (!strcmp(text, "device")) {
		copy_text(status.device, sizeof(status.device), value);
	} else if (!strcmp(text, "step")) {
		snprintf(text, sizeof(text), "step %.*s", (int)(length - 5), line + 5);
		append_text(&status.steps, &status.steps_length, STEPS_LIMIT, text);
	} else if (!strcmp(text, "file") && !strncmp(value, bundle_path, strlen(bundle_path)) &&
		   value[strlen(bundle_path)] == ' ') {
		char *end;

		stamp->bundle_length = simple_strtoul(value + strlen(bundle_path) + 1, &end, 16);
		while (*end == ' ')
			end++;
		stamp->bundle_crc = simple_strtoul(end, NULL, 16);
		stamp->has_bundle_line = true;
	}
}

static int read_cache_stamp(struct cache_stamp *stamp)
{
	char path[PATH_LENGTH];
	char *text;
	const char *line, *end;
	loff_t size = 0, read = 0;

	memset(stamp, 0, sizeof(*stamp));
	steps_reset();
	folder_path(path, sizeof(path), CACHE_STAMP_NAME);
	if (card_size(path, &size))
		return -ENOENT;
	if (size > STAMP_LIMIT)
		return -EFBIG;
	text = malloc_cache_aligned(STAMP_LIMIT);
	if (!text)
		return -ENOMEM;
	if (card_read(path, text, size, &read) || read != size) {
		free(text);
		return -EIO;
	}
	for (line = text; line < text + size; line = end + 1) {
		end = memchr(line, '\n', text + size - line);
		if (!end)
			end = text + size;
		read_stamp_line(stamp, line, end - line);
	}
	free(text);
	return 0;
}

static bool bundle_matches_stamp(const struct cache_stamp *stamp)
{
	size_t length;
	const char *text = brain_nk_bundle_text(&length);
	u32 crc = text ? crc32(0, (const u8 *)text, length) : 0;

	if (stamp->has_bundle_line && text && stamp->bundle_length == length && crc == stamp->bundle_crc)
		return true;
	printf("nkgen: %s/" BUNDLE_NAME " is %zx bytes, crc32 %08x; " CACHE_STAMP_NAME " %s %x bytes, crc32 %08x\n",
	       status.folder, text ? length : 0, crc, stamp->has_bundle_line ? "says" : "has no line, so",
	       stamp->bundle_length, stamp->bundle_crc);
	return false;
}

static bool cache_outdated(const struct cache_stamp *stamp)
{
	char version[NKGEN_VERSION_LENGTH + 1];
	u32 crc;

	if (read_generator_identity(version, &crc))
		return false;
	if (strcmp(version, stamp->generator) || crc != stamp->pak_crc) {
		printf("nkgen: cache made by %s with pak %08x, card has %s with pak %08x\n", stamp->generator,
		       stamp->pak_crc, version, crc);
		return true;
	}
	return false;
}

static int load_generator_file(const char *path, ulong address, u32 limit, loff_t *size)
{
	loff_t read = 0;

	if (card_size(path, size))
		return -ENOENT;
	if (*size > limit) {
		set_error("%s is too large", path);
		return -EFBIG;
	}
	if (card_read(path, map_sysmem(address, *size), *size, &read) || read != *size) {
		set_error("%s is unreadable", path);
		return -EIO;
	}
	return 0;
}

static int check_generator(const u8 *header, loff_t size)
{
	u32 file_length = le32(header + 20);
	u32 bss_length = le32(header + 24);
	u32 entry_offset = le32(header + 28);

	if (size < NKGEN_HEADER_SIZE || memcmp(header, NKGEN_HEADER_MAGIC, 8))
		set_error("%s has no NKGENBIN header", NKGEN_FILE);
	else if (le32(header + 8) != NKGEN_HEADER_VERSION || le32(header + 12) != NKGEN_ABI_VERSION)
		set_error("%s has a different header or ABI version from this U-Boot", NKGEN_FILE);
	else if (le32(header + 16) != NKGEN_ADDRESS)
		set_error("%s is linked for a different address", NKGEN_FILE);
	else if (file_length != size || (u64)file_length + bss_length > NKGEN_REGION)
		set_error("%s length or BSS does not fit", NKGEN_FILE);
	else if (entry_offset < NKGEN_HEADER_SIZE || entry_offset >= file_length || entry_offset % 4)
		set_error("%s entry is outside its code", NKGEN_FILE);
	else if (crc32(0, header + NKGEN_HEADER_SIZE, file_length - NKGEN_HEADER_SIZE) != le32(header + 32))
		set_error("%s CRC-32 does not match", NKGEN_FILE);
	else
		return 0;
	return -EINVAL;
}

static int check_pak(const u8 *pak, loff_t size, u32 format)
{
	if (size < PAK_HEADER_SIZE || memcmp(pak, PAK_MAGIC, 8))
		set_error("%s has no NKGENPAK header", NKGEN_PAK_FILE);
	else if (le32(pak + 8) != format)
		set_error("%s format differs from the generator's", NKGEN_PAK_FILE);
	else if (le32(pak + 12) != size)
		set_error("%s length differs from its header", NKGEN_PAK_FILE);
	else if (crc32(0, pak + PAK_CHECKSUM_START, size - PAK_CHECKSUM_START) != le32(pak + 16))
		set_error("%s CRC-32 does not match", NKGEN_PAK_FILE);
	else
		return 0;
	return -EINVAL;
}

static int prepare_generator(void)
{
	const u8 *header = map_sysmem(NKGEN_ADDRESS, NKGEN_REGION);
	const u8 *pak = map_sysmem(NKGEN_PAK_ADDRESS, NKGEN_PAK_REGION);
	loff_t size = 0, pak_size = 0;
	int result;

	result = load_generator_file(NKGEN_FILE, NKGEN_ADDRESS, NKGEN_REGION, &size);
	if (!result)
		result = check_generator(header, size);
	if (!result)
		result = load_generator_file(NKGEN_PAK_FILE, NKGEN_PAK_ADDRESS, NKGEN_PAK_REGION, &pak_size);
	if (!result)
		result = check_pak(pak, pak_size, le32(header + 36));
	if (result) {
		if (result == -ENOENT)
			printf("nkgen: no generator on the card\n");
		return result;
	}
	snprintf(generator_version, sizeof(generator_version), "%.*s", NKGEN_VERSION_LENGTH,
		 (const char *)header + NKGEN_VERSION_OFFSET);
	pak_crc = le32(pak + 16);
	pak_length = pak_size;
	printf("nkgen: %s, %lld bytes, pak %08x, %lld bytes\n", generator_version, size, pak_crc, pak_size);
	return 0;
}

enum brain_bundle_choice brain_bundle_choose(u32 image_length, bool regenerate)
{
	struct cache_stamp stamp;
	bool found, stamped = false, matches = false, generate = true;

	if (!status.identified) {
		brain_bundle_fallback(image_length);
		return BRAIN_BUNDLE_READY;
	}
	found = !brain_nk_bundle_load_from(status.folder);
	if (found) {
		stamped = !read_cache_stamp(&stamp);
		matches = stamped && bundle_matches_stamp(&stamp);
		generate = regenerate || (stamped && (!matches || cache_outdated(&stamp)));
	}
	if (generate && generation_allowed() && !prepare_generator())
		return BRAIN_BUNDLE_GENERATE;
	if (found) {
		status.bundle = stamped ? "cached" : "existing";
		return BRAIN_BUNDLE_READY;
	}
	brain_bundle_fallback(image_length);
	return BRAIN_BUNDLE_READY;
}

void brain_bundle_fallback(u32 image_length)
{
	struct cache_stamp stamp;

	if (status.identified && !brain_nk_bundle_load_from(status.folder)) {
		status.bundle = read_cache_stamp(&stamp) ? "existing" : "cached";
		return;
	}
	if (!brain_nk_bundle_load(image_length)) {
		status.bundle = "existing";
		return;
	}
	status.bundle = status.error[0] ? "failed" : "none";
}

static void on_progress(void *context, const char *stage, u32 done, u32 total)
{
	const struct generator_stage *found = NULL;
	unsigned int index;

	for (index = 0; index < ARRAY_SIZE(generator_stages); index++)
		if (!strcmp(generator_stages[index].name, stage))
			found = &generator_stages[index];
	if (found && found != current_stage) {
		current_stage = found;
		brain_progress_stage(stage);
	}
	if (current_stage && total)
		brain_progress_update(current_stage->start + (u64)current_stage->share * min(done, total) / total,
				      GENERATOR_UNITS);
}

static void on_log(void *context, const char *line)
{
	log_line(line);
}

static void hex_digest(char *text, const u8 *digest)
{
	unsigned int index;
	bool computed = false;

	text[0] = 0;
	for (index = 0; index < 32; index++)
		computed |= digest[index] != 0;
	for (index = 0; computed && index < 32; index++)
		snprintf(text + index * 2, 3, "%02x", digest[index]);
}

static void keep_steps(void)
{
	char line[256];
	u32 index;

	steps_reset();
	for (index = 0; index < call.step_count; index++) {
		const struct nkgen_step *step = &call.steps[index];

		snprintf(line, sizeof(line), "step %s=%s%s%s", step->name, step->result ? "failed" : "ok",
			 step->detail[0] ? ": " : "", step->detail);
		append_text(&status.steps, &status.steps_length, STEPS_LIMIT, line);
		log_line(line);
	}
}

static bool inside(const void *pointer, u32 length, ulong address, u32 region)
{
	ulong start = map_to_sysmem(pointer);

	return start >= address && start - address <= region && length <= region - (start - address);
}

static bool valid_output_path(const char *path)
{
	size_t length = strnlen(path, PATH_LENGTH);

	return length < PATH_LENGTH && length > strlen(CARD_DIRECTORY "/") &&
	       !strncmp(path, CARD_DIRECTORY "/", strlen(CARD_DIRECTORY "/")) && !strstr(path, "..");
}

static bool inside_outputs(const void *pointer, u32 length)
{
	return inside(pointer, length, NKGEN_ARENA_ADDRESS, NKGEN_ARENA_SIZE) ||
	       inside(pointer, length, NKGEN_PAK_ADDRESS, pak_length);
}

static int check_outputs(const struct nkgen_output **bundle)
{
	char bundle_path[PATH_LENGTH];
	u32 index;

	*bundle = NULL;
	folder_path(bundle_path, sizeof(bundle_path), BUNDLE_NAME);
	if (!inside(call.outputs, call.output_count * sizeof(*call.outputs), NKGEN_ARENA_ADDRESS, NKGEN_ARENA_SIZE)) {
		set_error("generator output list is outside the arena");
		return -EINVAL;
	}
	for (index = 0; index < call.output_count; index++) {
		const struct nkgen_output *output = &call.outputs[index];

		if (!inside_outputs(output->path, PATH_LENGTH) || !valid_output_path(output->path)) {
			set_error("generator output %u is not a path under " CARD_DIRECTORY "/", index);
			return -EINVAL;
		}
		if (!inside_outputs(output->data, output->length)) {
			set_error("generator output %s is outside the arena and pak", output->path);
			return -EINVAL;
		}
		if (!strcmp(output->path, bundle_path))
			*bundle = output;
	}
	if (!*bundle) {
		set_error("generator made no %s", bundle_path);
		return -EINVAL;
	}
	return 0;
}

int brain_nkgen_generate(const u8 *image, u32 image_length)
{
	const u8 *header = map_sysmem(NKGEN_ADDRESS, NKGEN_REGION);
	const struct nkgen_output *bundle;
	char line[96];
	unsigned long started = get_timer(0);
	u32 index;
	int result;

	generator_log_length = 0;
	current_stage = NULL;
	memset(map_sysmem(NKGEN_ADDRESS + le32(header + 20), le32(header + 24)), 0, le32(header + 24));
	memset(&call, 0, sizeof(call));
	call.magic = NKGEN_CALL_MAGIC;
	call.abi_version = NKGEN_ABI_VERSION;
	call.size = sizeof(call);
	call.image = image;
	call.image_length = image_length;
	call.pak = map_sysmem(NKGEN_PAK_ADDRESS, pak_length);
	call.pak_length = pak_length;
	call.arena = map_sysmem(NKGEN_ARENA_ADDRESS, NKGEN_ARENA_SIZE);
	call.arena_length = NKGEN_ARENA_SIZE;
	call.flags = NKGEN_FLAG_SKIP;
	call.progress = on_progress;
	call.log = on_log;
	copy_text(status.generator, sizeof(status.generator), generator_version);
	snprintf(line, sizeof(line), "generator %s, pak %08x, image %x bytes, identity %08x", generator_version,
		 pak_crc, image_length, status.identity);
	log_line(line);
	result = brain_nkgen_enter(NKGEN_ADDRESS, le32(header + 20) + le32(header + 24),
				   NKGEN_ADDRESS + le32(header + 28), &call);
	generated_ms = get_timer(started);
	snprintf(line, sizeof(line), "result %d in %lu ms, %u outputs", result, generated_ms, call.output_count);
	log_line(line);
	keep_steps();
	copy_text(status.release, sizeof(status.release), call.release);
	if (result || call.result) {
		set_error("%s", call.message[0] ? call.message : "generator failed");
	} else if (call.identity_crc32 != status.identity) {
		set_error("generator identity %08x differs from U-Boot's %08x", call.identity_crc32, status.identity);
	} else if (!check_outputs(&bundle)) {
		status.image = call.known ? "known" : "unknown";
		copy_text(status.profile, sizeof(status.profile), call.profile);
		copy_text(status.device, sizeof(status.device), call.device);
		hex_digest(status.sha256, call.image_sha256);
		free(generated_files);
		generated_files = calloc(call.output_count, sizeof(*generated_files));
		if (!generated_files) {
			set_error("no memory for the output table");
		} else {
			for (index = 0; index < call.output_count; index++) {
				generated_files[index].path = call.outputs[index].path;
				generated_files[index].data = call.outputs[index].data;
				generated_files[index].length = call.outputs[index].length;
			}
			brain_nk_bundle_use_ram((const char *)bundle->data, bundle->length, status.folder,
						generated_files, call.output_count);
			status.bundle = "generated";
			return 0;
		}
	}
	folder_path(line, sizeof(line), GENERATOR_LOG_NAME);
	if (generator_log && card_write(line, generator_log, generator_log_length))
		printf("nkgen: writing %s failed\n", line);
	return -EINVAL;
}

static void append_stamp(char **stamp, size_t *length, const char *format, ...)
{
	char line[PATH_LENGTH + 64];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	append_text(stamp, length, STAMP_LIMIT, line);
}

static int write_stamp(void)
{
	char path[PATH_LENGTH];
	char *stamp = NULL;
	size_t length = 0;
	u32 index;
	int result;

	append_stamp(&stamp, &length, "generator %s", generator_version);
	append_stamp(&stamp, &length, "pak %08x", pak_crc);
	append_stamp(&stamp, &length, "release %s", status.release);
	append_stamp(&stamp, &length, "image-sha256 %s", status.sha256);
	append_stamp(&stamp, &length, "image-length %x", status.image_length);
	append_stamp(&stamp, &length, "known %s", call.known ? "yes" : "no");
	append_stamp(&stamp, &length, "profile %s", status.profile);
	append_stamp(&stamp, &length, "device %s", status.device);
	for (index = 0; index < call.output_count; index++)
		append_stamp(&stamp, &length, "file %s %x %08x", call.outputs[index].path, call.outputs[index].length,
			     call.outputs[index].crc32);
	if (status.steps)
		append_bytes(&stamp, &length, STAMP_LIMIT, status.steps, status.steps_length);
	if (!stamp)
		return -ENOMEM;
	folder_path(path, sizeof(path), CACHE_STAMP_NAME);
	result = card_write(path, stamp, length);
	free(stamp);
	return result;
}

int brain_nkgen_save(void)
{
	char path[PATH_LENGTH];
	char line[PATH_LENGTH + 64];
	unsigned long started = get_timer(0);
	u64 total = 0, done = 0;
	u32 index, written;
	int result = 0;

	brain_progress_stage("Saving to card");
	folder_path(path, sizeof(path), CACHE_STAMP_NAME);
	card_unlink(path);
	for (index = 0; index < call.output_count; index++)
		total += call.outputs[index].length;
	for (written = 0; written < call.output_count && !result; written++) {
		const struct nkgen_output *output = &call.outputs[written];

		brain_progress_update(done, total);
		result = card_write(output->path, output->data, output->length);
		done += output->length;
		snprintf(line, sizeof(line), "wrote %s, %u bytes, result %d", output->path, output->length, result);
		log_line(line);
	}
	brain_progress_update(total, total);
	if (result) {
		for (index = 0; index < written; index++)
			if (index == written - 1 || in_folder(call.outputs[index].path))
				card_unlink(call.outputs[index].path);
		set_error("writing %s failed (%d)", call.outputs[written - 1].path, result);
	}
	snprintf(line, sizeof(line), "generated in %lu ms, saved in %lu ms", generated_ms, get_timer(started));
	log_line(line);
	folder_path(path, sizeof(path), GENERATOR_LOG_NAME);
	if (generator_log)
		card_write(path, generator_log, generator_log_length);
	if (!result)
		result = write_stamp();
	if (result && !status.error[0])
		set_error("writing " CACHE_STAMP_NAME " failed");
	return result;
}

static void append_status(char **text, size_t *length, const char *key, const char *value)
{
	char line[256];

	snprintf(line, sizeof(line), "%s=%s", key, value);
	append_text(text, length, STATUS_LIMIT, line);
}

static size_t build_status(char **text)
{
	const struct brain_nk_section *sections;
	unsigned int count, index;
	char value[160];
	size_t length = 0;

	append_status(text, &length, "image", status.image);
	append_status(text, &length, "profile", status.profile);
	append_status(text, &length, "device", status.device);
	snprintf(value, sizeof(value), "%08x", status.identity);
	append_status(text, &length, "image-crc", status.identified ? value : "");
	snprintf(value, sizeof(value), "%x", status.image_length);
	append_status(text, &length, "image-length", value);
	append_status(text, &length, "image-sha256", status.sha256);
	append_status(text, &length, "bundle", status.bundle);
	append_status(text, &length, "folder", brain_nk_bundle_directory());
	append_status(text, &length, "generator", status.generator);
	append_status(text, &length, "release", status.release);
	append_status(text, &length, "uboot", CONFIG_SHARP_WINCE_MODEL);
	append_status(text, &length, "error", status.error);
	if (status.steps)
		append_bytes(text, &length, STATUS_LIMIT, status.steps, status.steps_length);
	count = brain_nk_bundle_sections(&sections);
	for (index = 0; index < count; index++) {
		char line[256];

		if (sections[index].problem)
			snprintf(line, sizeof(line), "section %.*s=skipped: %s at line %u",
				 (int)sections[index].name_length, sections[index].name, sections[index].problem,
				 sections[index].line);
		else
			snprintf(line, sizeof(line), "section %.*s=applied", (int)sections[index].name_length,
				 sections[index].name);
		append_text(text, &length, STATUS_LIMIT, line);
	}
	return length;
}

int brain_boot_status_save(void)
{
	char *text = NULL;
	char *previous;
	size_t length = build_status(&text);
	loff_t read = 0;
	int result = 0;

	if (!text)
		return -ENOMEM;
	previous = malloc_cache_aligned(STATUS_LIMIT);
	if (previous && !card_read(BOOT_STATUS_FILE, previous, STATUS_LIMIT, &read) && read == length &&
	    !memcmp(previous, text, length)) {
		printf("boot status: unchanged\n");
	} else {
		result = card_write(BOOT_STATUS_FILE, text, length);
		printf("boot status: bundle %s, %s, written, result %d\n", status.bundle,
		       status.error[0] ? status.error : "no error", result);
	}
	free(previous);
	free(text);
	return result;
}
