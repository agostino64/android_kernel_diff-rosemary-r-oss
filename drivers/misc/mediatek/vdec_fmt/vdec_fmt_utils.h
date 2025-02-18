/*
 * Copyright (C) 2021 MediaTek Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#ifndef MTK_FMT_UTILS_H
#define MTK_FMT_UTILS_H

extern int fmt_dbg_level;

#define fmt_debug(level, format, args...)                       \
	do {                                                        \
		if ((fmt_dbg_level & level) == level)              \
			pr_info("[VDEC-FMT] level=%d %s(),%d: " format "\n",\
				level, __func__, __LINE__, ##args);      \
	} while (0)

#define fmt_err(format, args...)                                        \
	pr_info("[VDEC-FMT][ERROR] %s:%d: " format "\n", __func__, __LINE__, \
		   ##args)

#define FMT_TIMER_GET_DURATION_IN_US(start, end, duration)		\
	do {                                    \
		u64 time1;                          \
		u64 time2;                          \
\
		time1 = (u64)(start.tv_sec) * 1000000 +             \
			(u64)(start.tv_usec);                   \
		time2 = (u64)(end.tv_sec) * 1000000   +             \
			(u64)(end.tv_usec);                 \
\
		duration = (s32)(time2 - time1);                \
\
		if (duration <= 0)                      \
			duration = 1;                       \
	} while (0)

#define FMT_BANDWIDTH(data, pixel, throughput, bandwidth)		\
	do {                                    \
		u64 numerator;                          \
		u64 denominator;                        \
\
		/* ocucpied bw efficiency is 1.33 while accessing DRAM */\
		numerator =                         \
			(u64)(div_u64((u64)(data) * 4 * (u64)(throughput), 3));\
		denominator = (u64)(pixel);                 \
		if (denominator == 0)                       \
			denominator = 1;                    \
		bandwidth = (u32)(div_u64(numerator, denominator));\
	} while (0)


#endif /* _MTK_FMT_UTILS_H */
