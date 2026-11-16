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

#define BRAIN_KEY_NONE		0
#define BRAIN_KEY_PREVIOUS	1
#define BRAIN_KEY_NEXT		2
#define BRAIN_KEY_SELECT	3
#define BRAIN_KEY_BACK		4

#define BRAIN_PORTRAIT_BUFFER	0x44100000

struct brain_screen {
	unsigned int width;
	unsigned int height;
};

struct brain_progress_bar {
	unsigned int left;
	unsigned int top;
	unsigned int width;
	unsigned int height;
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
int brain_progress_bar_open(struct brain_progress_bar *bar);
int brain_progress_bar_push(const u16 *pixels, const struct brain_progress_bar *bar);

int brain_nk_bundle_load(u32 image_length);
unsigned int brain_nk_bundle_skip_spans(u32 image_length, struct nk_skip_span *spans, unsigned int maximum);
u32 brain_nk_bundle_cost(void);
void brain_nk_bundle_apply(u8 *image, u32 image_length);

bool brain_boot_log_requested(void);
int brain_save_boot_log(void);

void brain_snapshot_eboot_hardware(void);
int brain_eboot_register(u32 address, u32 *value);
void brain_report_hardware_changes(void);

#endif
