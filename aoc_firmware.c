// SPDX-License-Identifier: GPL-2.0-only
/*
 * Google Whitechapel AoC Firmware Loading Support
 *
 * Copyright (c) 2019 Google LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#define pr_fmt(fmt) "aoc-fw: " fmt

#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/uaccess.h>

#include "aoc.h"
#include "aoc_firmware.h"
#include "aoc-interface.h"

struct aoc_superbin_header {
	u32 magic;
	u32 container_version;
	u32 firmware_version;
	u32 image_size;
	u32 bootloader_low;
	u32 bootloader_high;
	u32 bootloader_offset;
	u32 bootloader_size;
	u32 uuid_table_offset;
	u32 uuid_table_size;
	u32 section_table_offset;
	u32 section_table_entry_size;
	u32 sram_offset;
	u32 a32_data_offset;
	u32 a32_data_size;
	u32 ff1_data_offset;
	u32 ff1_data_size;
	u32 hifi3z_data_offset;
	u32 hifi3z_data_size;
	u32 crc32;
} __packed;

static bool region_is_in_firmware(size_t start, size_t length,
				  const struct firmware *fw)
{
	return ((start + length) < fw->size);
}

bool _aoc_fw_is_valid(const struct firmware *fw)
{
	struct aoc_superbin_header *header;
	u32 ipc_offset, bootloader_offset;
	u32 uuid_offset, uuid_size;

	if (!fw || !fw->data)
		return false;

	if (!region_is_in_firmware(0, sizeof(*header), fw))
		return false;

	header = (struct aoc_superbin_header *)fw->data;
	if (le32_to_cpu(header->magic) != 0xaabbccdd)
		return false;

	/* Validate that the AoC firmware recognizes the messages known at
	 * compile time
	 */

	uuid_offset = le32_to_cpu(header->uuid_table_offset);
	uuid_size = le32_to_cpu(header->uuid_table_size);

	if (!region_is_in_firmware(uuid_offset, uuid_size, fw)) {
		pr_err("invalid method signature region\n");
		return false;
	}

	if (AocInterfaceCheck(fw->data + uuid_offset, uuid_size) != 0) {
		pr_err("failed to validate method signature table\n");
		return false;
	}

	ipc_offset = _aoc_fw_bootloader_offset(fw);
	bootloader_offset = _aoc_fw_bootloader_offset(fw);

	/* The bootloader resides within the FW image, so make sure
	 * that value makes sense
	 */
	if (!region_is_in_firmware(bootloader_offset,
				   le32_to_cpu(header->bootloader_size), fw))
		return false;

	return true;
}

u32 _aoc_fw_bootloader_offset(const struct firmware *fw)
{
	struct aoc_superbin_header *header =
		(struct aoc_superbin_header *)fw->data;
	return le32_to_cpu(header->bootloader_offset);
}

u32 _aoc_fw_ipc_offset(const struct firmware *fw)
{
	struct aoc_superbin_header *header =
		(struct aoc_superbin_header *)fw->data;
	return le32_to_cpu(header->image_size);
}

bool _aoc_fw_commit(const struct firmware *fw, void *dest)
{
	if (!_aoc_fw_is_valid(fw))
		return false;

	memcpy(dest, fw->data, fw->size);
	return true;
}
