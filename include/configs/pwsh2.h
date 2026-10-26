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
#ifndef __CONFIGS_PWSH2_H__
#define __CONFIGS_PWSH2_H__

/* Common configurations of Brain 2G, 3G, 4G */
#include <configs/brain_mx28_common.h>

/* The rest of the configuration is shared */
#include <configs/mxs.h>

#ifdef CONFIG_SHARP_WINCE
#undef CONFIG_VIDEO_MXS
#undef CONFIG_VIDEO_MXS_MODE_SYSTEM
#undef CONFIG_VIDEO_LOGO
#undef CONFIG_VIDEO_BMP_LOGO
#undef CONFIG_VIDEO_FONT_6X11
#endif

#endif /* __CONFIGS_PWSH2_H__ */
