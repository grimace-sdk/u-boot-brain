// SPDX-License-Identifier: GPL-2.0+

#include <common.h>
#include <fs.h>
#include <hexdump.h>
#include <mapmem.h>
#include <u-boot/crc.h>
#include "brain.h"

#define PATCH_FILE_NAME		"boot.nkp"
#define IMAGE_DIRECTORY		CARD_DIRECTORY "/nk-%08x"
#define PATCH_BUFFER		0x45000000
#define PATCH_BUFFER_LIMIT	0x00800000
#define MAXIMUM_NAME_LENGTH	64
#define MAXIMUM_EXTENSION	0x10000
#define MAXIMUM_PATH_LENGTH	96
#define MAXIMUM_LOADS		4

enum patch_pass { PASS_VERIFY, PASS_APPLY };

static u32 zeroed_end;
static char bundle_directory[MAXIMUM_PATH_LENGTH];

struct loaded_region {
	u8 *start;
	size_t length;
};

struct patch_result {
	unsigned int records;
	unsigned int bytes;
	struct loaded_region loads[MAXIMUM_LOADS];
	unsigned int load_count;
	const char *problem;
	unsigned int problem_line;
};

struct token {
	const char *start;
	size_t length;
};

static bool next_token(const char **cursor, const char *line_end, struct token *token)
{
	const char *position = *cursor;

	while (position < line_end && (*position == ' ' || *position == '\t' || *position == '\r'))
		position++;
	if (position >= line_end) {
		*cursor = position;
		return false;
	}
	token->start = position;
	while (position < line_end && *position != ' ' && *position != '\t' && *position != '\r')
		position++;
	token->length = position - token->start;
	*cursor = position;
	return true;
}

static bool token_is(const struct token *token, const char *word)
{
	return token->length == strlen(word) && !strncmp(token->start, word, token->length);
}

static int fail(struct patch_result *result, const char *problem, unsigned int line_number)
{
	result->problem = problem;
	result->problem_line = line_number;
	return -EINVAL;
}

static bool region_is_zero(const u8 *start, size_t length)
{
	size_t index;

	for (index = 0; index < length; index++)
		if (start[index])
			return false;
	return true;
}

static int load_card_file(struct token *path_token, u8 *destination, u32 length, u32 expected_crc,
			  struct patch_result *result, unsigned int line_number)
{
	char path[MAXIMUM_PATH_LENGTH];
	loff_t size = 0, read = 0;

	if (strlen(bundle_directory) + path_token->length + 2 > sizeof(path))
		return fail(result, "load path too long", line_number);
	snprintf(path, sizeof(path), "%s/%.*s", bundle_directory, (int)path_token->length, path_token->start);
	if (result->load_count >= MAXIMUM_LOADS)
		return fail(result, "too many loads", line_number);
	if (!region_is_zero(destination, length))
		return fail(result, "load target is not cleared", line_number);
	if (fs_set_blk_dev("mmc", CARD_DEVICE, FS_TYPE_FAT) || fs_size(path, &size) || size != length)
		return fail(result, "load file missing or wrong size", line_number);
	if (fs_set_blk_dev("mmc", CARD_DEVICE, FS_TYPE_FAT) ||
	    fs_read(path, map_to_sysmem(destination), 0, length, &read) || read != length) {
		memset(destination, 0, length);
		return fail(result, "load file unreadable", line_number);
	}
	result->loads[result->load_count].start = destination;
	result->loads[result->load_count].length = length;
	result->load_count++;
	if (crc32(0, destination, length) != expected_crc)
		return fail(result, "load file checksum differs", line_number);
	return 0;
}

static void clear_loads(struct patch_result *result)
{
	unsigned int index;

	for (index = 0; index < result->load_count; index++)
		memset(result->loads[index].start, 0, result->loads[index].length);
	result->load_count = 0;
}

static int run_patch_pass(const char *text, size_t text_length, u8 *image, u32 image_length,
			  enum patch_pass pass, struct patch_result *result)
{
	u32 patch_limit = image_length;
	const char *line = text;
	const char *text_end = text + text_length;
	unsigned int line_number = 0;

	while (line < text_end) {
		const char *line_end = memchr(line, '\n', text_end - line);
		const char *cursor = line;
		struct token first, expected, replacement;
		size_t byte_count, index;
		unsigned long offset;

		if (!line_end)
			line_end = text_end;
		line_number++;
		if (next_token(&cursor, line_end, &first)) {
			if (token_is(&first, "image")) {
				struct token length_token, extension_token;

				if (!next_token(&cursor, line_end, &length_token) ||
				    simple_strtoul(length_token.start, NULL, 16) != image_length)
					return fail(result, "built for a different image", line_number);
				if (next_token(&cursor, line_end, &extension_token)) {
					patch_limit = simple_strtoul(extension_token.start, NULL, 16);
					if (patch_limit < image_length || patch_limit - image_length > MAXIMUM_EXTENSION)
						return fail(result, "extension out of range", line_number);
					if (patch_limit > zeroed_end) {
						memset(image + zeroed_end, 0, patch_limit - zeroed_end);
						zeroed_end = patch_limit;
					}
				}
			} else if (token_is(&first, "load")) {
				struct token offset_token, length_token, crc_token, path_token;
				u32 length;

				if (!next_token(&cursor, line_end, &offset_token) || !next_token(&cursor, line_end, &length_token) ||
				    !next_token(&cursor, line_end, &crc_token) || !next_token(&cursor, line_end, &path_token))
					return fail(result, "malformed load", line_number);
				offset = simple_strtoul(offset_token.start, NULL, 16);
				length = simple_strtoul(length_token.start, NULL, 16);
				if (offset > patch_limit || length > patch_limit - offset)
					return fail(result, "load outside the image", line_number);
				if (pass == PASS_VERIFY &&
				    load_card_file(&path_token, image + offset, length,
						   simple_strtoul(crc_token.start, NULL, 16), result, line_number))
					return -EINVAL;
				if (pass == PASS_APPLY) {
					result->records++;
					result->bytes += length;
				}
			} else {
				if (!next_token(&cursor, line_end, &expected) || !next_token(&cursor, line_end, &replacement) ||
				    expected.length != replacement.length || expected.length % 2)
					return fail(result, "malformed record", line_number);
				offset = simple_strtoul(first.start, NULL, 16);
				byte_count = expected.length / 2;
				if (offset > patch_limit || byte_count > patch_limit - offset)
					return fail(result, "record outside the image", line_number);
				for (index = 0; index < byte_count; index++) {
					u8 old_byte, new_byte;

					if (hex2bin(&old_byte, expected.start + index * 2, 1) ||
					    hex2bin(&new_byte, replacement.start + index * 2, 1))
						return fail(result, "bad hex", line_number);
					if (pass == PASS_VERIFY && image[offset + index] != old_byte)
						return fail(result, "expected bytes differ", line_number);
					if (pass == PASS_APPLY)
						image[offset + index] = new_byte;
				}
				if (pass == PASS_APPLY) {
					result->records++;
					result->bytes += byte_count;
				}
			}
		}
		line = line_end + 1;
	}
	return 0;
}

static const char *bundle_text;
static size_t bundle_length;

static const char *line_end_of(const char *line)
{
	const char *end = memchr(line, '\n', bundle_text + bundle_length - line);

	return end ? end : bundle_text + bundle_length;
}

static int read_bundle(const char *directory, loff_t *size)
{
	char path[MAXIMUM_PATH_LENGTH];
	int result = fs_set_blk_dev("mmc", CARD_DEVICE, FS_TYPE_FAT);

	strlcpy(bundle_directory, directory, sizeof(bundle_directory));
	snprintf(path, sizeof(path), "%s/" PATCH_FILE_NAME, directory);
	if (!result)
		result = fs_read(path, PATCH_BUFFER, 0, 0, size);
	return result;
}

int brain_nk_bundle_load(u32 image_length)
{
	unsigned long started = get_timer(0);
	char directory[MAXIMUM_PATH_LENGTH];
	loff_t size = 0;
	int result;

	bundle_text = NULL;
	bundle_length = 0;
	snprintf(directory, sizeof(directory), IMAGE_DIRECTORY, image_length);
	result = read_bundle(directory, &size);
	if (result)
		result = read_bundle(CARD_DIRECTORY, &size);
	printf("nk bundle: %s/" PATCH_FILE_NAME ", read result %d, %lld bytes, %lu ms\n", bundle_directory, result, size,
	       get_timer(started));
	if (result || size > PATCH_BUFFER_LIMIT)
		return -ENOENT;
	bundle_text = map_sysmem(PATCH_BUFFER, PATCH_BUFFER_LIMIT);
	bundle_length = size;
	return 0;
}

unsigned int brain_nk_bundle_skip_spans(u32 image_length, struct nk_skip_span *spans, unsigned int maximum)
{
	const char *line = bundle_text;
	unsigned int count = 0;
	u32 previous_end = 0;

	if (!bundle_text)
		return 0;
	while (line < bundle_text + bundle_length) {
		const char *end = line_end_of(line);
		const char *cursor = line;
		struct token first, start_token, end_token, length_token;

		if (next_token(&cursor, end, &first)) {
			if (token_is(&first, "patch"))
				break;
			if (token_is(&first, "skip")) {
				u32 start, finish;

				if (!next_token(&cursor, end, &start_token) || !next_token(&cursor, end, &end_token) ||
				    !next_token(&cursor, end, &length_token) ||
				    simple_strtoul(length_token.start, NULL, 16) != image_length) {
					printf("nk bundle: skip list is for a different image, reading everything\n");
					return 0;
				}
				start = simple_strtoul(start_token.start, NULL, 16);
				finish = simple_strtoul(end_token.start, NULL, 16);
				if (start % 512 || finish % 512 || finish <= start || start < previous_end ||
				    finish > image_length || count >= maximum) {
					printf("nk bundle: invalid skip span %x-%x, reading everything\n", start, finish);
					return 0;
				}
				spans[count].start = start;
				spans[count].end = finish;
				previous_end = finish;
				count++;
			}
		}
		line = end + 1;
	}
	return count;
}

void brain_nk_bundle_apply(u8 *image, u32 image_length, unsigned int progress_start)
{
	const char *line = bundle_text;
	const char *section_name = NULL, *section_start = NULL;
	size_t section_name_length = 0;
	unsigned int sections = 0, section_index = 0, applied = 0;
	unsigned long started = get_timer(0);

	zeroed_end = image_length;
	if (!bundle_text)
		return;
	for (line = bundle_text; line < bundle_text + bundle_length; line = line_end_of(line) + 1)
		if (!strncmp(line, "patch ", 6))
			sections++;
	for (line = bundle_text; ; line = line_end_of(line) + 1) {
		bool at_end = line >= bundle_text + bundle_length;

		if (at_end || !strncmp(line, "patch ", 6)) {
			if (section_start) {
				struct patch_result result = { 0 };
				size_t length = (at_end ? bundle_text + bundle_length : line) - section_start;

				if (run_patch_pass(section_start, length, image, image_length, PASS_VERIFY, &result)) {
					clear_loads(&result);
					printf("nk patch %.*s: skipped, %s at line %u\n", (int)section_name_length, section_name,
					       result.problem, result.problem_line);
				} else {
					run_patch_pass(section_start, length, image, image_length, PASS_APPLY, &result);
					applied++;
					printf("nk patch %.*s: applied %u records, %u bytes\n", (int)section_name_length, section_name,
					       result.records, result.bytes);
				}
				section_index++;
				brain_menu_progress(progress_start + (1000 - progress_start) * section_index / sections, 1000);
			}
			if (at_end)
				break;
			section_name = line + 6;
			section_name_length = line_end_of(line) - section_name;
			while (section_name_length && (section_name[section_name_length - 1] == '\r'))
				section_name_length--;
			section_start = line_end_of(line) + 1;
		}
	}
	printf("nk patches: %u of %u applied in %lu ms\n", applied, sections, get_timer(started));
}
