ifdef CONFIG_VIDEO_MXS
obj-$(CONFIG_VIDEO_MXS)	+= lcd.o
obj-$(CONFIG_VIDEO_MXS)	+= dma.o
else
obj- = __dummy__.o
endif
ifdef CONFIG_ARCH_MX28
obj-$(CONFIG_ARCH_MX28)	+= cpu_clkdiv.o
endif
ifndef CONFIG_SPL_BUILD
obj-$(CONFIG_SHARP_WINCE)	+= wince/
endif
