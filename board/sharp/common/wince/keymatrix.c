// SPDX-License-Identifier: GPL-2.0+

#include <common.h>
#include <command.h>
#include <time.h>
#include <linux/delay.h>
#include <asm/io.h>

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
#define COLUMN			CONFIG_SHARP_KEYBOARD_MATRIX_ENTER_COLUMN
#define ROW			CONFIG_SHARP_KEYBOARD_MATRIX_ENTER_ROW
#define SETTLE_US		100
#define POLL_MS			20
#define PRESSED_POLLS		3
#define DEFAULT_WAIT_MS		1000

struct pin_state {
	unsigned int bank;
	unsigned int pin;
	u32 function;
	bool output_enabled;
	bool output_high;
};

static void pin_save(struct pin_state *state, unsigned int gpio)
{
	state->bank = GPIO_BANK(gpio);
	state->pin = GPIO_PIN(gpio);
	state->function = (readl(PINCTRL_MUXSEL(state->bank, state->pin)) >> ((state->pin % 16) * 2)) & 3;
	state->output_enabled = readl(PINCTRL_DOE(state->bank)) & BIT(state->pin);
	state->output_high = readl(PINCTRL_DOUT(state->bank)) & BIT(state->pin);
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

static int do_brainkey(struct cmd_tbl *command, int flag, int argc, char *const argv[])
{
	unsigned long wait_ms = argc > 1 ? simple_strtoul(argv[1], NULL, 10) : DEFAULT_WAIT_MS;
	struct pin_state column, row;
	unsigned int low_polls = 0;
	unsigned long started;
	bool pressed = false;

	pin_save(&column, COLUMN);
	pin_save(&row, ROW);
	if (row.output_enabled) {
		printf("Keyboard: row pin is an output, not scanning\n");
		return CMD_RET_FAILURE;
	}
	pin_set_function(&row, MUX_GPIO);
	pin_set_function(&column, MUX_GPIO);
	writel(BIT(column.pin), PINCTRL_DOE(column.bank) + REGISTER_CLEAR);
	udelay(SETTLE_US);
	if (row_low(&row)) {
		printf("Keyboard: row reads low with the column released, not scanning\n");
		pin_restore(&row);
		pin_restore(&column);
		return CMD_RET_FAILURE;
	}

	writel(BIT(column.pin), PINCTRL_DOUT(column.bank) + REGISTER_CLEAR);
	writel(BIT(column.pin), PINCTRL_DOE(column.bank) + REGISTER_SET);
	udelay(SETTLE_US);
	started = get_timer(0);
	do {
		low_polls = row_low(&row) ? low_polls + 1 : 0;
		pressed = low_polls >= PRESSED_POLLS;
		if (!pressed)
			mdelay(POLL_MS);
	} while (!pressed && get_timer(started) < wait_ms);

	pin_restore(&column);
	pin_restore(&row);
	printf("Keyboard: Enter %s within %lu ms\n", pressed ? "pressed" : "not pressed", wait_ms);
	return pressed ? CMD_RET_SUCCESS : CMD_RET_FAILURE;
}

U_BOOT_CMD(brainkey, 2, 0, do_brainkey,
	   "wait for Enter on the Brain keyboard matrix",
	   "[ms]\n"
	   "    - succeeds if Enter is held for three polls within ms (default 1000)");
