# SPDX-License-Identifier: GPL-2.0+
#
# (C) Copyright 2021 Suguru Saito <sg.sgch07@gmail.com>
# (C) Copyright 2000-2006
# Wolfgang Denk, DENX Software Engineering, wd@denx.de.

ifndef	CONFIG_SPL_BUILD
obj-y	:= pwa7200.o
obj-$(CONFIG_SHARP_WINCE)	+= teprobe.o
else
obj-y	:= iomux.o
endif
