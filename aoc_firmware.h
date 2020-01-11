// SPDX-License-Identifier: GPL-2.0-only
/*
 * Google Whitechapel AoC Firmware loading support
 *
 * Copyright (c) 2019 Google LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/firmware.h>

#define AOC_FIRMWARE_OFFSET_INVALID 0xffffffff

bool _aoc_fw_is_valid(const struct firmware *fw);

u32 _aoc_fw_bootloader_offset(const struct firmware *fw);

u32 _aoc_fw_ipc_offset(const struct firmware *fw);

bool _aoc_fw_commit(const struct firmware *fw, void *dest);
