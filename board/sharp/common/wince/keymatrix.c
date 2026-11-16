// SPDX-License-Identifier: GPL-2.0+

#include <common.h>
#include <command.h>
#include <time.h>
#include <linux/delay.h>
#include <asm/io.h>
#include "brain.h"

#define PINCTRL_BASE		0x80018000
#define PINCTRL_MUXSEL(bank, pin)	(PINCTRL_BASE + 0x100 + (bank) * 0x20 + ((pin) / 16) * 0x10)
#define PINCTRL_DOUT(bank)	(PINCTRL_BASE + 0x700 + (bank) * 0x10)
#define PINCTRL_DIN(bank)	(PINCTRL_BASE + 0x900 + (bank) * 0x10)
#define PINCTRL_DOE(bank)	(PINCTRL_BASE + 0xb00 + (bank) * 0x10)
#define REGISTER_SET		0x4
#define REGISTER_CLEAR		0x8
#define MUX_GPIO		3
#define GPIO_BANK(gpio)		((gpio) / 32)
#define GPIO_PIN(gpio)		((gpio) % 32)
#define SETTLE_US		100
#define POLL_MS			20
#define PRESSED_POLLS		3
#define MENU_PRESSED_POLLS	2
#define DEFAULT_WAIT_MS		1000
#define MAXIMUM_KEYS		16

struct pin_state {
	unsigned int bank;
	unsigned int pin;
	u32 function;
	bool output_enabled;
	bool output_high;
};

struct key_state {
	unsigned int down_polls;
	bool ignored;
};

static struct key_state key_states[MAXIMUM_KEYS];
static unsigned long last_scan;
static bool scanned;

static void pin_save(struct pin_state *state, unsigned int gpio, bool verbose)
{
	state->bank = GPIO_BANK(gpio);
	state->pin = GPIO_PIN(gpio);
	state->function = (readl(PINCTRL_MUXSEL(state->bank, state->pin)) >> ((state->pin % 16) * 2)) & 3;
	state->output_enabled = readl(PINCTRL_DOE(state->bank)) & BIT(state->pin);
	state->output_high = readl(PINCTRL_DOUT(state->bank)) & BIT(state->pin);
	if (verbose)
		printf("Keyboard: pin %u_%u function %u, output %s, %s\n", state->bank, state->pin, state->function,
		       state->output_enabled ? "enabled" : "disabled", state->output_high ? "high" : "low");
}

static void pin_set_function(const struct pin_state *state, u32 function)
{
	u32 address = PINCTRL_MUXSEL(state->bank, state->pin);
	unsigned int shift = (state->pin % 16) * 2;

	writel(3 << shift, address + REGISTER_CLEAR);
	writel(function << shift, address + REGISTER_SET);
}

static void pin_restore(const struct pin_state *state)
{
	u32 bit = BIT(state->pin);

	writel(bit, PINCTRL_DOUT(state->bank) + (state->output_high ? REGISTER_SET : REGISTER_CLEAR));
	writel(bit, PINCTRL_DOE(state->bank) + (state->output_enabled ? REGISTER_SET : REGISTER_CLEAR));
	pin_set_function(state, state->function);
}

static bool row_low(const struct pin_state *row)
{
	return !(readl(PINCTRL_DIN(row->bank)) & BIT(row->pin));
}

static int scan_start(const struct brain_matrix_key *key, struct pin_state *column, struct pin_state *row,
		      bool verbose)
{
	pin_save(column, key->column, verbose);
	pin_save(row, key->row, verbose);
	if (row->output_enabled) {
		if (verbose)
			printf("Keyboard: row pin is an output, not scanning\n");
		return -EBUSY;
	}
	pin_set_function(row, MUX_GPIO);
	pin_set_function(column, MUX_GPIO);
	writel(BIT(column->pin), PINCTRL_DOE(column->bank) + REGISTER_CLEAR);
	udelay(SETTLE_US);
	if (row_low(row)) {
		if (verbose)
			printf("Keyboard: row reads low with the column released, not scanning\n");
		pin_restore(row);
		pin_restore(column);
		return -EBUSY;
	}
	writel(BIT(column->pin), PINCTRL_DOUT(column->bank) + REGISTER_CLEAR);
	writel(BIT(column->pin), PINCTRL_DOE(column->bank) + REGISTER_SET);
	udelay(SETTLE_US);
	return 0;
}

static void scan_end(const struct pin_state *column, const struct pin_state *row)
{
	pin_restore(column);
	pin_restore(row);
}

static bool key_down(const struct brain_matrix_key *key)
{
	struct pin_state column, row;
	bool down;

	if (scan_start(key, &column, &row, false))
		return false;
	down = row_low(&row);
	scan_end(&column, &row);
	return down;
}

static unsigned int key_count(void)
{
	return min(brain_matrix_key_count, (unsigned int)MAXIMUM_KEYS);
}

static int scan_keys(void)
{
	unsigned int index;
	int pressed = BRAIN_KEY_NONE;

	for (index = 0; index < key_count(); index++) {
		struct key_state *state = &key_states[index];

		if (!key_down(&brain_matrix_keys[index])) {
			state->down_polls = 0;
			state->ignored = false;
			continue;
		}
		if (!scanned) {
			state->ignored = true;
			printf("Keyboard: %s held\n", brain_matrix_keys[index].name);
		}
		if (state->ignored || ++state->down_polls != MENU_PRESSED_POLLS)
			continue;
		printf("Keyboard: %s pressed\n", brain_matrix_keys[index].name);
		if (pressed == BRAIN_KEY_NONE)
			pressed = brain_matrix_keys[index].key;
	}
	scanned = true;
	last_scan = get_timer(0);
	return pressed;
}

bool brain_keys_held(void)
{
	unsigned int index;

	if (!scanned)
		scan_keys();
	for (index = 0; index < key_count(); index++)
		if (key_states[index].ignored)
			return true;
	return false;
}

int brain_keys_poll(void)
{
	if (scanned && get_timer(last_scan) < POLL_MS)
		return BRAIN_KEY_NONE;
	return scan_keys();
}

static const struct brain_matrix_key *find_key(int code)
{
	unsigned int index;

	for (index = 0; index < brain_matrix_key_count; index++)
		if (brain_matrix_keys[index].key == code)
			return &brain_matrix_keys[index];
	return NULL;
}

static int do_brainkey(struct cmd_tbl *command, int flag, int argc, char *const argv[])
{
	unsigned long wait_ms = argc > 1 ? simple_strtoul(argv[1], NULL, 10) : DEFAULT_WAIT_MS;
	const struct brain_matrix_key *enter = find_key(BRAIN_KEY_SELECT);
	struct pin_state column, row;
	unsigned int low_polls = 0;
	unsigned long started;
	bool pressed = false;

	if (!enter || scan_start(enter, &column, &row, true))
		return CMD_RET_FAILURE;
	started = get_timer(0);
	do {
		low_polls = row_low(&row) ? low_polls + 1 : 0;
		pressed = low_polls >= PRESSED_POLLS;
		if (!pressed)
			mdelay(POLL_MS);
	} while (!pressed && get_timer(started) < wait_ms);

	scan_end(&column, &row);
	printf("Keyboard: Enter %s within %lu ms\n", pressed ? "pressed" : "not pressed", wait_ms);
	return pressed ? CMD_RET_SUCCESS : CMD_RET_FAILURE;
}

U_BOOT_CMD(brainkey, 2, 0, do_brainkey,
	   "wait for Enter on the Brain keyboard matrix",
	   "[ms]\n"
	   "    - succeeds if Enter is held for three polls within ms (default 1000)");
