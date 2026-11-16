/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef __SHARP_WINCE_BRAIN_H
#define __SHARP_WINCE_BRAIN_H

#include <linux/types.h>

#define CARD_DEVICE		"1:1"
#define CARD_DIRECTORY		"pwsh2-boot"

struct nk_skip_span {
	u32 start;
	u32 end;
};

struct brain_ram_file {
	const char *path;
	const u8 *data;
	u32 length;
};

struct brain_nk_section {
	const char *name;
	size_t name_length;
	const char *problem;
	unsigned int line;
};

enum brain_bundle_choice {
	BRAIN_BUNDLE_READY,
	BRAIN_BUNDLE_GENERATE,
};

typedef int (*brain_sector_reader)(void *context, u32 first_sector, u32 sector_count, void *buffer);

struct nkgen_call;

#define BRAIN_KEY_NONE		0
#define BRAIN_KEY_PREVIOUS	1
#define BRAIN_KEY_NEXT		2
#define BRAIN_KEY_SELECT	3
#define BRAIN_KEY_BACK		4

#define BRAIN_PORTRAIT_BUFFER	0x44100000

struct brain_screen {
	unsigned int width;
	unsigned int height;
	unsigned int update_interval_ms;
};

struct brain_matrix_key {
	unsigned int column;
	unsigned int row;
	int key;
	const char *name;
};

extern const struct brain_matrix_key brain_matrix_keys[];
extern const unsigned int brain_matrix_key_count;

struct brain_progress_bar {
	unsigned int left;
	unsigned int top;
	unsigned int width;
	unsigned int height;
	unsigned int interval_ms;
};

int brain_display_init(void);
void brain_display_hold(bool hold);
void brain_display_refresh(void);
void brain_display_report(void);

int brain_screen_open(struct brain_screen *screen, u16 *background);
int brain_screen_push(const u16 *canvas, unsigned int left, unsigned int top, unsigned int width,
		      unsigned int height);
void brain_portrait_push(unsigned int rows, unsigned int columns);

int brain_keys_poll(void);
bool brain_keys_held(void);

int brain_keyboard_init(void);

void brain_board_late_init(void);

void brain_progress_begin(u32 total);
void brain_progress_step(u32 units);
void brain_progress_update(u32 done, u32 total);
void brain_progress_end(void);
void brain_progress_stage(const char *stage);
int brain_progress_bar_open(struct brain_progress_bar *bar);
int brain_progress_bar_push(const u16 *pixels, const struct brain_progress_bar *bar);
int brain_progress_label_push(const char *text);

int brain_nk_bundle_load(u32 image_length);
unsigned int brain_nk_bundle_skip_spans(u32 image_length, struct nk_skip_span *spans, unsigned int maximum);
u32 brain_nk_bundle_cost(void);
void brain_nk_bundle_apply(u8 *image, u32 image_length);
void brain_nk_bundle_clear(void);
int brain_nk_bundle_load_from(const char *directory);
void brain_nk_bundle_use_ram(const char *text, size_t length, const char *directory, const struct brain_ram_file *files,
			     unsigned int file_count);
const char *brain_nk_bundle_text(size_t *length);
const char *brain_nk_bundle_directory(void);
unsigned int brain_nk_bundle_sections(const struct brain_nk_section **list);

int brain_image_identity(brain_sector_reader read, void *context, u32 image_length, u32 *identity);
void brain_boot_status_begin(u32 image_length, const u32 *identity);
enum brain_bundle_choice brain_bundle_choose(u32 image_length, bool regenerate);
int brain_nkgen_generate(const u8 *image, u32 image_length);
int brain_nkgen_save(void);
void brain_bundle_fallback(u32 image_length);
int brain_boot_status_save(void);
int brain_nkgen_enter(ulong start, ulong length, ulong entry, struct nkgen_call *call);

bool brain_boot_log_requested(void);
int brain_save_boot_log(void);

void brain_snapshot_eboot_hardware(void);
int brain_eboot_register(u32 address, u32 *value);
void brain_report_hardware_changes(void);

#endif
