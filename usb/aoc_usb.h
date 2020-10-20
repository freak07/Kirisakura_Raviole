/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 Google Corp.
 *
 * Author:
 *  Howard.Yen <howardyen@google.com>
 */

#ifndef __LINUX_AOC_USB_H
#define __LINUX_AOC_USB_H

#include <linux/notifier.h>

#include "xhci.h"

/*
 * This variable used to present if aoc_usb module was probed done. If offload
 * is enabled, the controller needs to wait for the aoc_usb probe done and then
 * continue the controller's probe.
 */
extern bool aoc_usb_probe_done;

enum aoc_usb_msg {
	SYNC_DEVICE_CONTEXT,
	GET_DCBAA_PTR,
	GET_TR_DEQUEUE_PTR,
	SETUP_DONE,
	GET_ISOC_TR_INFO
};

enum usb_offload_op_mode {
	USB_OFFLOAD_STOP,
	USB_OFFLOAD_SIMPLE_AUDIO_ACCESSORY
};

struct xhci_vendor_data {
	struct xhci_hcd *xhci;

	bool usb_audio_offload;

	enum usb_offload_op_mode op_mode;

	struct workqueue_struct *irq_wq;
	struct work_struct xhci_vendor_irq_work;
	struct work_struct xhci_vendor_reset_ws;
};

struct aoc_usb_drvdata {
	struct aoc_service_dev *adev;

	struct mutex lock;

	struct notifier_block nb;
};

struct get_dev_ctx_args {
	unsigned int slot_id;
	size_t length;
	u8 *dev_ctx;
};

struct get_isoc_tr_info_args {
	u16 ep_id;
	u16 dir;
	u32 type;
	u32 num_segs;
	u32 seg_ptr;
	u32 max_packet;
	u32 deq_ptr;
	u32 enq_ptr;
	u32 cycle_state;
	u32 num_trbs_free;
};

int xhci_vendor_helper_init(void);

extern int xhci_handle_event(struct xhci_hcd *xhci);
extern void xhci_update_erst_dequeue(struct xhci_hcd *xhci,
				     union xhci_trb *event_ring_deq);
extern int xhci_plat_register_vendor_ops(struct xhci_vendor_ops *vendor_ops);

int register_aoc_usb_notifier(struct notifier_block *nb);
int unregister_aoc_usb_notifier(struct notifier_block *nb);

#endif /* __LINUX_AOC_USB_H */
