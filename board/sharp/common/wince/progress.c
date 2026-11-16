// SPDX-License-Identifier: GPL-2.0+

#include <common.h>
#include "brain.h"

#define BAR_BORDER		2
#define BAR_MAXIMUM_PIXELS	8192
#define COLOUR_BORDER		0x0000
#define COLOUR_TRACK		0xffff
#define COLOUR_FILLED		0x051e

static struct brain_progress_bar bar;
static u16 bar_pixels[BAR_MAXIMUM_PIXELS];
static bool bar_shown;
static unsigned int track_width;
static unsigned int drawn_width;
static u32 progress_total;
static u32 step_start;
static u32 step_units;
static u32 steps_end;
static unsigned int bar_pushes;
static unsigned long bar_push_us;

static bool inside_border(unsigned int x, unsigned int y)
{
	return x >= BAR_BORDER && y >= BAR_BORDER && x < bar.width - BAR_BORDER && y < bar.height - BAR_BORDER;
}

static void draw_bar(unsigned int filled)
{
	unsigned long started = timer_get_us();
	u16 *pixel = bar_pixels;
	unsigned int x, y;

	for (y = 0; y < bar.height; y++)
		for (x = 0; x < bar.width; x++)
			if (!inside_border(x, y))
				*pixel++ = COLOUR_BORDER;
			else
				*pixel++ = x - BAR_BORDER < filled ? COLOUR_FILLED : COLOUR_TRACK;
	if (brain_progress_bar_push(bar_pixels, &bar))
		bar_shown = false;
	drawn_width = filled;
	bar_pushes++;
	bar_push_us += timer_get_us() - started;
}

void brain_progress_begin(u32 total)
{
	progress_total = total ? total : 1;
	step_start = 0;
	step_units = progress_total;
	steps_end = 0;
	bar_shown = !brain_progress_bar_open(&bar) && bar.width > 2 * BAR_BORDER && bar.height > 2 * BAR_BORDER &&
		    bar.width * bar.height <= BAR_MAXIMUM_PIXELS;
	if (!bar_shown)
		return;
	track_width = bar.width - 2 * BAR_BORDER;
	bar_pushes = 0;
	bar_push_us = 0;
	draw_bar(0);
}

void brain_progress_step(u32 units)
{
	step_start = steps_end;
	step_units = units;
	steps_end += units;
}

void brain_progress_update(u32 done, u32 total)
{
	u64 position;
	unsigned int filled;

	if (!bar_shown || !total)
		return;
	if (done > total)
		done = total;
	position = step_start + (u64)step_units * done / total;
	if (position > progress_total)
		position = progress_total;
	filled = position * track_width / progress_total;
	if (filled != drawn_width)
		draw_bar(filled);
}

void brain_progress_end(void)
{
	if (!bar_shown)
		return;
	if (drawn_width != track_width)
		draw_bar(track_width);
	printf("progress: %u bar updates in %lu us\n", bar_pushes, bar_push_us);
	bar_shown = false;
}
