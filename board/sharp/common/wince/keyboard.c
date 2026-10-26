// SPDX-License-Identifier: GPL-2.0+

#include <common.h>
#include <command.h>
#include <env.h>
#include <i2c.h>
#include <stdio_dev.h>
#include <time.h>
#include <watchdog.h>
#include <linux/delay.h>
#include "brain.h"

#define KEYBOARD_BUS		CONFIG_SHARP_KEYBOARD_I2C_BUS
#define KEYBOARD_ADDRESS	0x28
#define KEYBOARD_KEYCODE	0x04
#define KEYBOARD_POLL_MS	20
#define KEY_CODE(value)		((value) & 0x3f)
#define KEY_PRESSED(value)	(((value) & 0x40) == 0)
#define KEY_IS_SWITCH(value)	(((value) & 0x80) != 0)
#define KEY_ENTER		0x23
#define DEFAULT_WAIT_MS		1000
#define QUEUE_SIZE		32

struct key_sequence {
	unsigned char code;
	const char *sequence;
};

static const struct key_sequence key_sequences[] = {
	{ 0x22, "\x1b[A" },
	{ 0x2a, "\x1b[B" },
	{ 0x30, "\x1b[C" },
	{ 0x1a, "\x1b[D" },
	{ 0x23, "\r" },
	{ 0x1c, " " },
	{ 0x1b, "\x1b" },
	{ 0x07, "\x1b" },
	{ 0x31, "\b" },
};

static char queue[QUEUE_SIZE];
static unsigned int queue_head;
static unsigned int queue_tail;
static unsigned long last_poll;

static int keyboard_read_byte(unsigned int reg)
{
	unsigned char value;

	if (i2c_read(KEYBOARD_ADDRESS, reg, 1, &value, 1))
		return -1;
	return value;
}

static int keyboard_read_word(unsigned char *first, unsigned char *second)
{
	unsigned char data[2];

	if (i2c_read(KEYBOARD_ADDRESS, KEYBOARD_KEYCODE, 1, data, 2))
		return -1;
	*first = data[0];
	*second = data[1];
	return 0;
}

static void keyboard_write_byte(unsigned int reg, unsigned char value)
{
	i2c_write(KEYBOARD_ADDRESS, reg, 1, &value, 1);
}

static void queue_push(const char *sequence)
{
	while (*sequence) {
		unsigned int next = (queue_head + 1) % QUEUE_SIZE;

		if (next == queue_tail)
			return;
		queue[queue_head] = *sequence++;
		queue_head = next;
	}
}

static void handle_key(unsigned char value)
{
	unsigned int index;

	if (!value || KEY_IS_SWITCH(value) || !KEY_PRESSED(value))
		return;
	for (index = 0; index < ARRAY_SIZE(key_sequences); index++) {
		if (key_sequences[index].code == KEY_CODE(value)) {
			queue_push(key_sequences[index].sequence);
			return;
		}
	}
}

static unsigned int keyboard_read_keys(unsigned char keys[3])
{
	unsigned char count;

	if (keyboard_read_word(&count, &keys[0]) || count < 1 || !keys[0])
		return 0;
	if (count < 2 || keyboard_read_word(&keys[1], &keys[2]))
		return 1;
	return count >= 3 ? 3 : 2;
}

static void keyboard_poll(void)
{
	unsigned char keys[3];
	unsigned int previous_bus, count, index;

	if (get_timer(last_poll) < KEYBOARD_POLL_MS)
		return;
	last_poll = get_timer(0);
	previous_bus = i2c_get_bus_num();
	i2c_set_bus_num(KEYBOARD_BUS);
	count = keyboard_read_keys(keys);
	for (index = 0; index < count; index++)
		handle_key(keys[index]);
	i2c_set_bus_num(previous_bus);
}

__weak void brain_display_refresh(void)
{
}

static int keyboard_tstc(struct stdio_dev *device)
{
	brain_display_refresh();
	if (queue_head == queue_tail)
		keyboard_poll();
	return queue_head != queue_tail;
}

static int keyboard_getc(struct stdio_dev *device)
{
	char character;

	while (!keyboard_tstc(device))
		WATCHDOG_RESET();
	character = queue[queue_tail];
	queue_tail = (queue_tail + 1) % QUEUE_SIZE;
	return character;
}

static void keyboard_start_controller(void)
{
	static const unsigned char reads_before[] = {
		0x00, 0x0a, 0x01, 0x40, 0x41, 0x48, 0x00,
	};
	static const unsigned char reads_after_word[] = {
		0x01, 0x0a, 0x00, 0x0a, 0x01,
	};
	unsigned char first, second;
	unsigned int index;

	for (index = 0; index < ARRAY_SIZE(reads_before); index++)
		keyboard_read_byte(reads_before[index]);
	keyboard_read_word(&first, &second);
	for (index = 0; index < ARRAY_SIZE(reads_after_word); index++)
		keyboard_read_byte(reads_after_word[index]);
	keyboard_write_byte(0x88, 0x08);
	keyboard_write_byte(0x82, 0x05);
	keyboard_read_byte(0x00);
	keyboard_read_byte(0x0a);
	keyboard_write_byte(0x82, 0x02);
	keyboard_write_byte(0x82, 0x03);
	mdelay(500);
}

static bool keyboard_start(void)
{
	i2c_set_bus_num(KEYBOARD_BUS);
	i2c_init(CONFIG_SYS_I2C_SPEED, 0);
	if (keyboard_read_byte(0x00) < 0) {
		printf("Keyboard: no response at 0x%02x\n", KEYBOARD_ADDRESS);
		return false;
	}
	keyboard_start_controller();
	return true;
}

int brain_keyboard_init(void)
{
	struct stdio_dev device;
	unsigned char first, second;

	if (!keyboard_start())
		return 0;
	keyboard_read_word(&first, &second);

	memset(&device, 0, sizeof(device));
	strcpy(device.name, "brainkbd");
	device.flags = DEV_FLAGS_INPUT;
	device.tstc = keyboard_tstc;
	device.getc = keyboard_getc;
	if (stdio_register(&device))
		return 0;
	env_set("stdin", "serial,brainkbd");
	return 0;
}

static bool enter_pressed(unsigned char value)
{
	printf("Keyboard: event 0x%02x\n", value);
	return !KEY_IS_SWITCH(value) && KEY_PRESSED(value) && KEY_CODE(value) == KEY_ENTER;
}

static int do_brainkey(struct cmd_tbl *command, int flag, int argc, char *const argv[])
{
	unsigned long wait_ms = argc > 1 ? simple_strtoul(argv[1], NULL, 10) : DEFAULT_WAIT_MS;
	unsigned char keys[3];
	unsigned int count, index;
	unsigned long started;
	bool pressed = false;

	if (!keyboard_start())
		return CMD_RET_FAILURE;
	started = get_timer(0);
	do {
		count = keyboard_read_keys(keys);
		for (index = 0; index < count; index++)
			pressed |= enter_pressed(keys[index]);
		if (!pressed)
			mdelay(KEYBOARD_POLL_MS);
	} while (!pressed && get_timer(started) < wait_ms);
	printf("Keyboard: Enter %s within %lu ms\n", pressed ? "pressed" : "not pressed", wait_ms);
	return pressed ? CMD_RET_SUCCESS : CMD_RET_FAILURE;
}

U_BOOT_CMD(brainkey, 2, 0, do_brainkey,
	   "wait for Enter on the Brain keyboard",
	   "[ms]\n"
	   "    - succeeds if Enter is pressed within ms (default 1000)");
