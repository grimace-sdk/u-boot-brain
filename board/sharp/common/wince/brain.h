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

int brain_display_init(void);
void brain_display_hold(bool hold);
void brain_display_refresh(void);
void brain_display_report(void);
void brain_display_show(const u16 *canvas);
void brain_display_show_region(const u16 *canvas, unsigned int left, unsigned int top,
			       unsigned int width, unsigned int height);

int brain_keyboard_init(void);

void brain_board_late_init(void);

void brain_menu_progress(unsigned int done, unsigned int total);

int brain_nk_bundle_load(u32 image_length);
unsigned int brain_nk_bundle_skip_spans(u32 image_length, struct nk_skip_span *spans, unsigned int maximum);
void brain_nk_bundle_apply(u8 *image, u32 image_length, unsigned int progress_start);

bool brain_boot_log_requested(void);
int brain_save_boot_log(void);

void brain_snapshot_eboot_hardware(void);
int brain_eboot_register(u32 address, u32 *value);
void brain_report_hardware_changes(void);

#endif
