/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * (C) Copyright 2011 Takumi Sueda.
 * Author: Takumi Sueda <puhitaku@gmail.com>
 *
 * (C) Copyright 2011 Freescale Semiconductor, Inc.
 * Author: Fabio Estevam <fabio.estevam@freescale.com>
 *
 * Based on m28evk.h:
 * Copyright (C) 2011 Marek Vasut <marek.vasut@gmail.com>
 * on behalf of DENX Software Engineering GmbH
 */
#ifndef __BRAIN_MX28_COMMON_H__
#define __BRAIN_MX28_COMMON_H__

/* System configurations */
#define CONFIG_MACH_TYPE	MACH_TYPE_MX28EVK

/* Memory configuration */
#define PHYS_SDRAM_1			0x40000000	/* Base address */
#define PHYS_SDRAM_1_SIZE		0x8000000	/* Max 128 MB RAM */
#define CONFIG_SYS_SDRAM_BASE		PHYS_SDRAM_1

/* Environment */

/* Environment is in MMC */
#if defined(CONFIG_CMD_MMC) && defined(CONFIG_ENV_IS_IN_MMC)
#define CONFIG_SYS_MMC_ENV_DEV		0
#endif

/* USB */
#ifdef	CONFIG_CMD_USB
#define CONFIG_EHCI_MXS_PORT1
#define CONFIG_USB_MAX_CONTROLLER_COUNT	1
#endif

/* I2C */
#ifdef CONFIG_SYS_I2C_MXS
#define CONFIG_SYS_I2C
#define CONFIG_SYS_I2C_SPEED	100000
#endif

/* Framebuffer support */
#ifdef CONFIG_VIDEO
#define CONFIG_VIDEO_MXS
#define CONFIG_VIDEO_MXS_MODE_SYSTEM
#define CONFIG_VIDEO_LOGO
#define CONFIG_VIDEO_BMP_LOGO
#define CONFIG_HIDE_LOGO_VERSION
#define CONFIG_BMP_16BPP
#define CONFIG_SYS_VIDEO_LOGO_MAX_SIZE	(512 << 10)
#define CONFIG_VIDEO_FONT_6X11
#define LCD_BPP LCD_COLOR16
#endif

/* Boot Linux */
#define CONFIG_BOOTFILE		"uImage"
#define CONFIG_LOADADDR		0x42000000
#define CONFIG_SYS_LOAD_ADDR	CONFIG_LOADADDR

/* Extra Environment */
#define CONFIG_EXTRA_ENV_SETTINGS \
	"stdin=serial\0" \
	"stdout=serial,vga\0" \
	"stderr=serial,vga\0" \
	"bootdelay=0\0" \
	"videomode=video=ctfb:x:" CONFIG_LCD_X ",y:" CONFIG_LCD_Y ",depth:16,pclk:30857,le:0,ri:0,up:0,lo:0,hs:0,vs:0,sync:0,vmode:0\0" \
	"image=zImage\0" \
	"console_mainline=ttyAMA0\0" \
	"bootargs_custom=fbcon=font:ProFont6x11\0" \
	"bootorder=emmc sd\0" \
	"fdt_file=" CONFIG_DEFAULT_FDT_FILE "\0" \
	"fdt_addr=0x41000000\0" \
	"verbose=no\0" \
	"printfail=echo failed to ${fail}\0" \
	"quiet=" \
		"if test $verbose = no; then " \
			"setenv stdout_orig ${stdout}; " \
			"setenv stdout nulldev; " \
		"fi\0" \
	"unquiet=" \
		"setenv returncode $?; " \
		"if test -n $stdout_orig; then " \
			"setenv stdout ${stdout_orig}; " \
		"fi; " \
		"exit $returncode\0" \
	"sddev=1\0" \
	"sdpart=1\0" \
	"sdroot=/dev/mmcblk1p2 rw rootwait\0" \
	"selectsd=" \
		"setenv dev ${sddev}; " \
		"setenv part ${sdpart}; " \
		"setenv root ${sdroot}; " \
		"setenv devname SD; " \
		"run quiet; " \
		"mmc dev ${dev}; " \
		"run unquiet\0" \
	"emmcdev=0\0" \
	"emmcpart=1\0" \
	"emmcroot=/dev/mmcblk0p3 rw rootwait\0" \
	"selectemmc=" \
		"setenv dev ${emmcdev}; " \
		"setenv part ${emmcpart}; " \
		"setenv root ${emmcroot}; " \
		"setenv devname eMMC; " \
		"run quiet; " \
		"mmc dev ${dev}; " \
		"run unquiet\0" \
	"loadimage=" \
		"run quiet; " \
		"fatload mmc ${dev}:${part} ${loadaddr} ${image}; " \
		"run unquiet\0" \
	"loadfdt=" \
		"run quiet; " \
		"fatload mmc ${dev}:${part} ${fdt_addr} ${fdt_file}; " \
		"run unquiet\0" \
	"checkenvexists=" \
		"run quiet; " \
		"test -e mmc ${dev}:${part} uEnv.txt; " \
		"run unquiet\0" \
	"loadenv=" \
		"run quiet; " \
		"fatload mmc ${dev}:${part} ${loadaddr} uEnv.txt; " \
		"run unquiet\0" \
	"importenv=" \
		"run quiet; " \
		"env import -t ${loadaddr} ${filesize}; " \
		"run unquiet\0" \
	"loadandimportenv=" \
		"if run checkenvexists; then " \
			"echo -n \"Loading environment from ${devname} ... \"; " \
			"if setenv fail load && run loadenv && setenv fail parse && run importenv; then " \
				"echo OK; " \
			"else " \
				"run printfail; " \
			"fi; " \
		"fi\0" \
	"setargs=setenv bootargs console=${console_mainline},${baudrate} console=tty1 " \
		"root=${root} ${bootargs_custom}\0" \
	"mmcboot=" \
		"echo -n \"Loading kernel and device tree from ${devname} ... \"; " \
		"if setenv fail load image && run loadimage && setenv fail load DT && run loadfdt; then " \
			"echo OK; " \
			"echo \"Booting from ${devname} ... \"; " \
			"setenv fail bootz; " \
			"run setargs; " \
			"bootz ${loadaddr} - ${fdt_addr}; " \
		"fi; " \
		"run printfail\0" \
	"preboot=" \
		"for choice in emmc sd; do " /* Env in SD must have higher priority */ \
			"if run select$choice; then " \
				"run loadandimportenv; " \
			"fi; " \
		"done\0" \
	"bootcmd=" \
		"for choice in $bootorder; do " \
			"if run select$choice; then " \
				"run mmcboot; " \
			"fi; " \
		"done; " \
		"echo Failed to boot (X_X)"

/* The rest of the configuration is shared */
#include <configs/mxs.h>

#endif /* __BRAIN_MX28_COMMON_H__ */
