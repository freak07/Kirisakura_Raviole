/* SPDX-License-Identifier: GPL-2.0 OR Apache-2.0 */
/*
 * Google Whitechapel AoC Core Driver
 *
 * Copyright (c) 2020 Google LLC
 *
 */

#define AOC_IOCTL_MAGIC 0xac

#define AOC_IS_ONLINE _IOR(AOC_IOCTL_MAGIC, 5, int)

struct aoc_ion_handle {
	__u32 handle;
	__s32 fd;
};

#define AOC_IOCTL_ION_FD_TO_HANDLE _IOWR(AOC_IOCTL_MAGIC, 204, struct aoc_ion_handle)
