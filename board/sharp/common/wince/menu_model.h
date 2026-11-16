/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef __SHARP_WINCE_MENU_MODEL_H
#define __SHARP_WINCE_MENU_MODEL_H

#include <linux/types.h>

#define MENU_DEFAULT_TIMEOUT		10
#define MENU_MAXIMUM_TIMEOUT		3600
#define MENU_PARTITION_LENGTH		16
#define MENU_PATH_LENGTH		128
#define MENU_CMDLINE_LENGTH		512
#define MENU_COMMAND_LENGTH		768
#define MENU_LINUX_KERNEL_ADDRESS	"0x42000000"
#define MENU_LINUX_DTB_ADDRESS		"0x41000000"
#define MENU_LINUX_INITRD_ADDRESS	"0x43000000"

enum menu_action {
	MENU_WINDOWS_CE,
	MENU_WINDOWS_CE_STOCK,
	MENU_LINUX,
	MENU_CONSOLE,
	MENU_ACTIONS,
};

enum menu_linux_target {
	MENU_LINUX_BRAINUX,
	MENU_LINUX_KERNEL,
	MENU_LINUX_SCRIPT,
};

struct menu_config {
	enum menu_action default_action;
	int timeout;
	bool console;
	bool linux_enabled;
	char linux_partition[MENU_PARTITION_LENGTH];
	char linux_kernel[MENU_PATH_LENGTH];
	char linux_dtb[MENU_PATH_LENGTH];
	char linux_initrd[MENU_PATH_LENGTH];
	char linux_script[MENU_PATH_LENGTH];
	char linux_cmdline[MENU_CMDLINE_LENGTH];
	unsigned int ignored_lines;
};

struct menu_model {
	enum menu_action entries[MENU_ACTIONS];
	unsigned int count;
	unsigned int selected;
};

void menu_config_defaults(struct menu_config *config, int timeout);
void menu_config_parse(struct menu_config *config, const char *text, size_t length);
enum menu_linux_target menu_linux_target(const struct menu_config *config);
const char *menu_linux_probe_path(const struct menu_config *config);
int menu_linux_command(const struct menu_config *config, char *command, size_t size);
void menu_model_build(struct menu_model *model, const struct menu_config *config, bool linux_present,
		      bool console_shown, enum menu_action selected);
void menu_model_move(struct menu_model *model, int step);
enum menu_action menu_model_selected(const struct menu_model *model);
const char *menu_action_label(enum menu_action action);
const char *menu_action_name(enum menu_action action);

#endif
