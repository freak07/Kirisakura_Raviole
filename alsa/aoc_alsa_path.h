/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Google Whitechapel AoC ALSA Driver
 *
 * Copyright (c) 2021 Google LLC
 */

#ifndef AOC_ALSA_PATH_H
#define AOC_ALSA_PATH_H

bool aoc_alsa_usb_playback_enabled(void);
bool aoc_alsa_usb_capture_enabled(void);
#endif
