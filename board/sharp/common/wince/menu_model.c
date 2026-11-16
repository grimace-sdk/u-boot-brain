// SPDX-License-Identifier: GPL-2.0+

#include <common.h>
#include "menu_model.h"

#define MENU_KEY_LENGTH		32

struct menu_action_text {
	const char *name;
	const char *label;
};

static const struct menu_action_text action_text[MENU_ACTIONS] = {
	[MENU_WINDOWS_CE] = { "windows-ce", "Windows CE" },
	[MENU_WINDOWS_CE_STOCK] = { "windows-ce-stock", "CE stock" },
	[MENU_LINUX] = { "linux", "Linux" },
	[MENU_CONSOLE] = { "console", "U-Boot" },
};

const char *menu_action_label(enum menu_action action)
{
	return action < MENU_ACTIONS ? action_text[action].label : "";
}

const char *menu_action_name(enum menu_action action)
{
	return action < MENU_ACTIONS ? action_text[action].name : "";
}

void menu_config_defaults(struct menu_config *config, int timeout)
{
	memset(config, 0, sizeof(*config));
	config->default_action = MENU_WINDOWS_CE;
	config->timeout = timeout;
	strcpy(config->linux_partition, "1:1");
}

static bool is_space(char character)
{
	return character == ' ' || character == '\t' || character == '\r';
}

static bool is_digit(char character)
{
	return character >= '0' && character <= '9';
}

static bool parse_yes_no(const char *value, bool *result)
{
	if (!strcmp(value, "yes")) {
		*result = true;
		return true;
	}
	if (!strcmp(value, "no")) {
		*result = false;
		return true;
	}
	return false;
}

static bool parse_timeout(const char *value, int *timeout)
{
	int seconds = 0;

	if (!*value)
		return false;
	for (; *value; value++) {
		if (!is_digit(*value))
			return false;
		seconds = seconds * 10 + *value - '0';
		if (seconds > MENU_MAXIMUM_TIMEOUT)
			return false;
	}
	*timeout = seconds;
	return true;
}

static bool parse_action(const char *value, enum menu_action *action)
{
	unsigned int index;

	for (index = 0; index < MENU_ACTIONS; index++) {
		if (!strcmp(value, action_text[index].name)) {
			*action = index;
			return true;
		}
	}
	return false;
}

static bool parse_partition(const char *value, char *partition)
{
	const char *character = value;

	if (!is_digit(*character))
		return false;
	while (is_digit(*character))
		character++;
	if (*character++ != ':' || !is_digit(*character))
		return false;
	while (is_digit(*character))
		character++;
	if (*character || strlen(value) >= MENU_PARTITION_LENGTH)
		return false;
	strcpy(partition, value);
	return true;
}

static bool is_path_character(char character)
{
	return is_digit(character) || (character >= 'a' && character <= 'z') ||
	       (character >= 'A' && character <= 'Z') || character == '/' || character == '.' || character == '_' ||
	       character == '-' || character == '+';
}

static bool parse_path(const char *value, char *path)
{
	const char *character;

	if (!*value || strlen(value) >= MENU_PATH_LENGTH)
		return false;
	for (character = value; *character; character++)
		if (!is_path_character(*character))
			return false;
	strcpy(path, value);
	return true;
}

static bool parse_text(const char *value, char *text, size_t size)
{
	if (strlen(value) >= size)
		return false;
	strcpy(text, value);
	return true;
}

static bool apply_setting(struct menu_config *config, const char *key, const char *value)
{
	if (!strcmp(key, "default"))
		return parse_action(value, &config->default_action);
	if (!strcmp(key, "timeout"))
		return parse_timeout(value, &config->timeout);
	if (!strcmp(key, "console"))
		return parse_yes_no(value, &config->console);
	if (!strcmp(key, "linux"))
		return parse_yes_no(value, &config->linux_enabled);
	if (!strcmp(key, "linux-partition"))
		return parse_partition(value, config->linux_partition);
	if (!strcmp(key, "linux-kernel"))
		return parse_path(value, config->linux_kernel);
	if (!strcmp(key, "linux-dtb"))
		return parse_path(value, config->linux_dtb);
	if (!strcmp(key, "linux-initrd"))
		return parse_path(value, config->linux_initrd);
	if (!strcmp(key, "linux-script"))
		return parse_path(value, config->linux_script);
	if (!strcmp(key, "linux-cmdline"))
		return parse_text(value, config->linux_cmdline, sizeof(config->linux_cmdline));
	return false;
}

static void parse_line(struct menu_config *config, const char *line, size_t length)
{
	char key[MENU_KEY_LENGTH];
	char value[MENU_CMDLINE_LENGTH];
	size_t key_length, value_start, value_length;
	const char *separator;

	while (length && is_space(*line)) {
		line++;
		length--;
	}
	while (length && is_space(line[length - 1]))
		length--;
	if (!length || *line == '#')
		return;
	separator = memchr(line, '=', length);
	if (!separator) {
		config->ignored_lines++;
		return;
	}
	key_length = separator - line;
	while (key_length && is_space(line[key_length - 1]))
		key_length--;
	value_start = separator - line + 1;
	while (value_start < length && is_space(line[value_start]))
		value_start++;
	value_length = length - value_start;
	if (!key_length || key_length >= sizeof(key) || value_length >= sizeof(value)) {
		config->ignored_lines++;
		return;
	}
	memcpy(key, line, key_length);
	key[key_length] = 0;
	memcpy(value, line + value_start, value_length);
	value[value_length] = 0;
	if (!apply_setting(config, key, value))
		config->ignored_lines++;
}

void menu_config_parse(struct menu_config *config, const char *text, size_t length)
{
	const char *line = text;
	const char *end = text + length;
	const char *newline;

	if (length >= 3 && (u8)text[0] == 0xef && (u8)text[1] == 0xbb && (u8)text[2] == 0xbf)
		line += 3;
	while (line < end) {
		newline = memchr(line, '\n', end - line);
		if (!newline)
			newline = end;
		parse_line(config, line, newline - line);
		line = newline + 1;
	}
}

enum menu_linux_target menu_linux_target(const struct menu_config *config)
{
	if (config->linux_script[0])
		return MENU_LINUX_SCRIPT;
	if (config->linux_kernel[0])
		return MENU_LINUX_KERNEL;
	return MENU_LINUX_BRAINUX;
}

const char *menu_linux_probe_path(const struct menu_config *config)
{
	switch (menu_linux_target(config)) {
	case MENU_LINUX_SCRIPT:
		return config->linux_script;
	case MENU_LINUX_KERNEL:
		return config->linux_kernel;
	default:
		return NULL;
	}
}

static int append(char *command, size_t size, size_t *used, const char *format, const char *first,
		  const char *second)
{
	int written = snprintf(command + *used, size - *used, format, first, second);

	if (written < 0 || (size_t)written >= size - *used)
		return -ENOSPC;
	*used += written;
	return 0;
}

int menu_linux_command(const struct menu_config *config, char *command, size_t size)
{
	const char *partition = config->linux_partition;
	size_t used = 0;
	int result;

	if (!size)
		return -ENOSPC;
	command[0] = 0;
	switch (menu_linux_target(config)) {
	case MENU_LINUX_SCRIPT:
		return append(command, size, &used, "load mmc %s " MENU_LINUX_KERNEL_ADDRESS " %s; source "
			      MENU_LINUX_KERNEL_ADDRESS, partition, config->linux_script);
	case MENU_LINUX_KERNEL:
		result = append(command, size, &used, "load mmc %s " MENU_LINUX_KERNEL_ADDRESS " %s", partition,
				config->linux_kernel);
		if (!result && config->linux_dtb[0])
			result = append(command, size, &used, "; load mmc %s " MENU_LINUX_DTB_ADDRESS " %s", partition,
					config->linux_dtb);
		if (!result && config->linux_initrd[0])
			result = append(command, size, &used, "; load mmc %s " MENU_LINUX_INITRD_ADDRESS " %s",
					partition, config->linux_initrd);
		if (!result)
			result = append(command, size, &used, "; bootz " MENU_LINUX_KERNEL_ADDRESS " %s%s",
					config->linux_initrd[0] ? MENU_LINUX_INITRD_ADDRESS ":${filesize}" : "-",
					config->linux_dtb[0] ? " " MENU_LINUX_DTB_ADDRESS : "");
		return result;
	default:
		return append(command, size, &used, "%s%s", "run linux_boot", "");
	}
}

void menu_model_build(struct menu_model *model, const struct menu_config *config, bool linux_present,
		      bool console_shown, enum menu_action selected)
{
	unsigned int index;

	model->count = 0;
	model->entries[model->count++] = MENU_WINDOWS_CE;
	model->entries[model->count++] = MENU_WINDOWS_CE_STOCK;
	if (config->linux_enabled && linux_present)
		model->entries[model->count++] = MENU_LINUX;
	if (config->console || console_shown)
		model->entries[model->count++] = MENU_CONSOLE;
	model->selected = 0;
	for (index = 0; index < model->count; index++)
		if (model->entries[index] == selected)
			model->selected = index;
}

void menu_model_move(struct menu_model *model, int step)
{
	int count = model->count;

	if (!count)
		return;
	model->selected = ((int)model->selected + step % count + count) % count;
}

enum menu_action menu_model_selected(const struct menu_model *model)
{
	return model->entries[model->selected];
}
