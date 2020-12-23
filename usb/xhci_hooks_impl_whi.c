// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Google Corp.
 *
 * Author:
 *  Howard.Yen <howardyen@google.com>
 */

#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/workqueue.h>
#include <linux/usb/hcd.h>

#include "xhci.h"
#include "xhci-plat.h"
#include "aoc_usb.h"

#define SRAM_BASE 0x19000000
#define SRAM_SIZE 0x600000

static BLOCKING_NOTIFIER_HEAD(aoc_usb_notifier_list);

int register_aoc_usb_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&aoc_usb_notifier_list, nb);
}

int unregister_aoc_usb_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&aoc_usb_notifier_list, nb);
}

static int xhci_sync_dev_ctx(struct xhci_hcd *xhci, unsigned int slot_id)
{
	struct xhci_virt_device *dev = xhci->devs[slot_id];
	struct xhci_container_ctx *out_ctx_ref = dev->out_ctx;
	struct xhci_slot_ctx *slot_ctx;
	struct xhci_ep_ctx *ep_ctx;
	struct get_dev_ctx_args args;
	u8 *dev_ctx;

	xhci_dbg(xhci, "slot_id=%u, out_ctx_ref->size=%u\n", slot_id,
		 out_ctx_ref->size);

	dev_ctx = kcalloc(1, out_ctx_ref->size, GFP_KERNEL);
	if (!dev_ctx)
		return -ENOMEM;

	args.slot_id = slot_id;
	args.length = out_ctx_ref->size;
	args.dev_ctx = dev_ctx;
	blocking_notifier_call_chain(&aoc_usb_notifier_list,
				     SYNC_DEVICE_CONTEXT, &args);

	memcpy(out_ctx_ref->bytes, dev_ctx, out_ctx_ref->size);

	slot_ctx = xhci_get_slot_ctx(xhci, out_ctx_ref);
	ep_ctx = xhci_get_ep_ctx(xhci, out_ctx_ref, 0); /* ep0 */

	xhci_dbg(xhci, "%s\n",
		 xhci_decode_slot_context(
			 slot_ctx->dev_info, slot_ctx->dev_info2,
			 slot_ctx->tt_info, slot_ctx->dev_state));
	xhci_dbg(xhci, "%s\n",
		 xhci_decode_ep_context(ep_ctx->ep_info, ep_ctx->ep_info2,
					ep_ctx->deq, ep_ctx->tx_info));

	kfree(dev_ctx);
	return 0;
}

static int xhci_get_dcbaa_ptr(u64 *aoc_dcbaa_ptr)
{
	blocking_notifier_call_chain(&aoc_usb_notifier_list, GET_DCBAA_PTR,
				     aoc_dcbaa_ptr);
	return 0;
}

static int xhci_setup_done(void)
{
	blocking_notifier_call_chain(&aoc_usb_notifier_list, SETUP_DONE, NULL);
	return 0;
}

static int xhci_get_isoc_tr_info(u16 ep_id, u16 dir, struct xhci_ring *ep_ring)
{
	struct get_isoc_tr_info_args tr_info;

	tr_info.ep_id = ep_id;
	tr_info.dir = dir;
	blocking_notifier_call_chain(&aoc_usb_notifier_list, GET_ISOC_TR_INFO,
				     &tr_info);

	ep_ring->num_segs = tr_info.num_segs;
	ep_ring->bounce_buf_len = tr_info.max_packet;
	ep_ring->type = tr_info.type;
	ep_ring->first_seg->dma = tr_info.seg_ptr;
	ep_ring->cycle_state = tr_info.cycle_state;
	ep_ring->num_trbs_free = tr_info.num_trbs_free;

	return 0;
}

static bool is_compatible_with_usb_audio_offload(struct usb_device *udev)
{
	struct usb_host_config *config;
	struct usb_interface_descriptor *desc;
	int i;
	bool is_audio = false;

	config = udev->config;
	for (i = 0; i < config->desc.bNumInterfaces; i++) {
		desc = &config->intf_cache[i]->altsetting->desc;
		if (desc->bInterfaceClass == USB_CLASS_AUDIO) {
			is_audio = true;
			break;
		}
	}

	return is_audio;
}

static struct xhci_hcd *get_xhci_hcd_by_udev(struct usb_device *udev)
{
	struct usb_hcd *uhcd = container_of(udev->bus, struct usb_hcd, self);

	return hcd_to_xhci(uhcd);
}


static int sync_dev_ctx(struct xhci_hcd *xhci, unsigned int slot_id)
{
	struct xhci_vendor_data *vendor_data = xhci_to_priv(xhci)->vendor_data;
	int ret = 0;

	if (vendor_data->op_mode != USB_OFFLOAD_STOP)
		ret = xhci_sync_dev_ctx(xhci, slot_id);

	return ret;
}

static void xhci_reset_work(struct work_struct *ws)
{
	int rc;
	struct xhci_vendor_data *vendor_data =
		container_of(ws, struct xhci_vendor_data, xhci_vendor_reset_ws);
	struct xhci_hcd *xhci = vendor_data->xhci;

	usb_remove_hcd(xhci->shared_hcd);
	usb_remove_hcd(xhci->main_hcd);

	vendor_data->op_mode = USB_OFFLOAD_SIMPLE_AUDIO_ACCESSORY;

	rc = usb_add_hcd(xhci->main_hcd, xhci->main_hcd->irq, IRQF_SHARED);
	if (rc) {
		xhci_err(xhci, "add main hcd error: %d\n", rc);
		goto fail;
	}
	rc = usb_add_hcd(xhci->shared_hcd, xhci->shared_hcd->irq, IRQF_SHARED);
	if (rc) {
		xhci_err(xhci, "add shared hcd error: %d\n", rc);
		goto fail;
	}

	xhci_dbg(xhci, "xhci reset for usb audio offload was done\n");

fail:
	return;
}

static void xhci_reset_for_usb_audio_offload(struct usb_device *udev)
{
	struct usb_device *rhdev = udev->parent;
	struct xhci_vendor_data *vendor_data;
	struct xhci_hcd *xhci;

	if (!rhdev || rhdev->parent)
		return;

	xhci = get_xhci_hcd_by_udev(udev);
	vendor_data = xhci_to_priv(xhci)->vendor_data;

	if (!vendor_data->usb_audio_offload
	    || vendor_data->op_mode != USB_OFFLOAD_STOP)
		return;

	schedule_work(&vendor_data->xhci_vendor_reset_ws);
}

static int xhci_udev_notify(struct notifier_block *self, unsigned long action,
			    void *dev)
{
	struct usb_device *udev = dev;

	switch (action) {
	case USB_DEVICE_ADD:
		if (is_compatible_with_usb_audio_offload(udev)) {
			dev_dbg(&udev->dev,
				 "Compatible with usb audio offload\n");
			xhci_reset_for_usb_audio_offload(udev);
		}
		break;
	case USB_DEVICE_REMOVE:
		/* TODO: notify AoC usb audio device removed. */
		break;
	}

	return NOTIFY_OK;
}

static struct notifier_block xhci_udev_nb = {
	.notifier_call = xhci_udev_notify,
};

static void xhci_vendor_irq_work(struct work_struct *work)
{
	struct xhci_vendor_data *vendor_data =
		container_of(work, struct xhci_vendor_data, xhci_vendor_irq_work);
	struct xhci_hcd *xhci = vendor_data->xhci;
	struct usb_hcd *hcd = xhci_to_hcd(xhci);
	union xhci_trb *event_ring_deq;
	unsigned long flags;
	u64 temp_64;
	u32 status = 0;
	int event_loop = 0;
	unsigned int slot_id = 1;
	int ret;

	ret = sync_dev_ctx(xhci, slot_id);
	if (ret)
		xhci_warn(xhci, "Failed to sync device context failed, err=%d", ret);

	spin_lock_irqsave(&xhci->lock, flags);

	/*
	 * Clear the op reg interrupt status first,
	 * so we can receive interrupts from other MSI-X interrupters.
	 * Write 1 to clear the interrupt status.
	 */
	status |= STS_EINT;
	writel(status, &xhci->op_regs->status);

	if (!hcd->msi_enabled) {
		u32 irq_pending;

		irq_pending = readl(&xhci->ir_set->irq_pending);
		irq_pending |= IMAN_IP;
		writel(irq_pending, &xhci->ir_set->irq_pending);
	}

	if (xhci->xhc_state & XHCI_STATE_DYING ||
	    xhci->xhc_state & XHCI_STATE_HALTED) {
		xhci_err(xhci, "xHCI dying, ignoring interrupt. Shouldn't IRQs be disabled?\n");
		/*
		 * Clear the event handler busy flag (RW1C);
		 * the event ring should be empty.
		 */
		temp_64 = xhci_read_64(xhci, &xhci->ir_set->erst_dequeue);
		xhci_write_64(xhci, temp_64 | ERST_EHB,
			      &xhci->ir_set->erst_dequeue);
		goto out;
	}

	event_ring_deq = xhci->event_ring->dequeue;
	/*
	 * FIXME this should be a delayed service routine
	 * that clears the EHB.
	 */
	while (xhci_handle_event(xhci) > 0) {
		if (event_loop++ < TRBS_PER_SEGMENT / 2)
			continue;
		xhci_update_erst_dequeue(xhci, event_ring_deq);
		event_loop = 0;
	}

	xhci_update_erst_dequeue(xhci, event_ring_deq);

out:
	spin_unlock_irqrestore(&xhci->lock, flags);
}

static int xhci_vendor_init_irq_workqueue(struct xhci_vendor_data *vendor_data)
{
	vendor_data->irq_wq = alloc_workqueue("xhci_vendor_irq_work", 0, 0);

	if (!vendor_data->irq_wq) {
		return -ENOMEM;
	}

	INIT_WORK(&vendor_data->xhci_vendor_irq_work, xhci_vendor_irq_work);

	return 0;
}


static struct xhci_ring *
xhci_initialize_ring_info_for_remote_isoc(struct xhci_hcd *xhci,
					  u32 endpoint_type,
					  enum xhci_ring_type type, gfp_t flags)
{
	struct xhci_ring *ring;
	struct xhci_segment *seg;
	u16 dir;
	struct device *dev = xhci_to_hcd(xhci)->self.sysdev;

	ring = kzalloc_node(sizeof(*ring), flags, dev_to_node(dev));
	if (!ring)
		return NULL;

	ring->type = TYPE_ISOC;
	INIT_LIST_HEAD(&ring->td_list);

	seg = kzalloc_node(sizeof(*seg), flags, dev_to_node(dev));
	if (!seg) {
		kfree(ring);
		return NULL;
	}

	ring->first_seg = seg;
	ring->enq_seg = ring->first_seg;
	ring->deq_seg = ring->first_seg;
	dir = endpoint_type == ISOC_IN_EP ? 0 : 1;

	xhci_get_isoc_tr_info(0, dir, ring);

	xhci_dbg(xhci, "ring->first_seg->dma=0x%llx\n", ring->first_seg->dma);

	return ring;
}

static int usb_audio_offload_init(struct xhci_hcd *xhci)
{
	struct device *dev = xhci_to_hcd(xhci)->self.sysdev;
	struct xhci_vendor_data *vendor_data;
	int ret;
	u32 out_val;

	if (!aoc_usb_probe_done) {
		dev_dbg(dev, "deferring the probe\n");
		return -EPROBE_DEFER;
	}

	vendor_data = kzalloc(sizeof(struct xhci_vendor_data), GFP_KERNEL);
	if (!vendor_data)
		return -ENOMEM;

	if (!of_property_read_u32(dev->of_node, "offload", &out_val))
		vendor_data->usb_audio_offload = (out_val == 1) ? true : false;

	ret = xhci_vendor_init_irq_workqueue(vendor_data);
	if (ret) {
		kfree(vendor_data);
		return ret;
	}

	INIT_WORK(&vendor_data->xhci_vendor_reset_ws, xhci_reset_work);
	usb_register_notify(&xhci_udev_nb);
	vendor_data->op_mode = USB_OFFLOAD_STOP;
	vendor_data->xhci = xhci;

	xhci_to_priv(xhci)->vendor_data = vendor_data;

	return 0;
}

static void usb_audio_offload_cleanup(struct xhci_hcd *xhci)
{
	struct xhci_vendor_data *vendor_data = xhci_to_priv(xhci)->vendor_data;

	vendor_data->usb_audio_offload = false;
	if (vendor_data->irq_wq)
		destroy_workqueue(vendor_data->irq_wq);
	vendor_data->irq_wq = NULL;
	vendor_data->xhci = NULL;

	usb_unregister_notify(&xhci_udev_nb);

	kfree(vendor_data);
	xhci_to_priv(xhci)->vendor_data = NULL;
}

static bool is_dma_in_sram(dma_addr_t dma)
{
	if (dma >= SRAM_BASE && dma < SRAM_BASE + SRAM_SIZE)
		return true;
	return false;
}

static bool is_usb_offload_enabled(struct xhci_hcd *xhci,
		struct xhci_virt_device *vdev, unsigned int ep_index)
{
	struct xhci_vendor_data *vendor_data = xhci_to_priv(xhci)->vendor_data;
	bool global_enabled = vendor_data->op_mode != USB_OFFLOAD_STOP;
	struct xhci_ring *ep_ring;

	if (vdev == NULL || vdev->eps[ep_index].ring == NULL)
		return global_enabled;

	if (global_enabled) {
		ep_ring = vdev->eps[ep_index].ring;
		if (is_dma_in_sram(ep_ring->first_seg->dma))
			return true;
	}

	return false;
}

static irqreturn_t queue_irq_work(struct xhci_hcd *xhci)
{
	struct xhci_vendor_data *vendor_data = xhci_to_priv(xhci)->vendor_data;
	irqreturn_t ret = IRQ_NONE;
	struct xhci_transfer_event *event;
	u32 trb_comp_code;

	if (is_usb_offload_enabled(xhci, NULL, 0)) {
		event = &xhci->event_ring->dequeue->trans_event;
		trb_comp_code = GET_COMP_CODE(le32_to_cpu(event->transfer_len));
		if (trb_comp_code == COMP_STALL_ERROR) {
			if (!work_pending(&vendor_data->xhci_vendor_irq_work)) {
				queue_work(vendor_data->irq_wq,
					   &vendor_data->xhci_vendor_irq_work);
			}
			ret = IRQ_HANDLED;
		}
	}

	return ret;
}

static struct xhci_device_context_array *alloc_dcbaa(struct xhci_hcd *xhci,
						     gfp_t flags)
{
	dma_addr_t dma;
	struct device *dev = xhci_to_hcd(xhci)->self.sysdev;
	struct xhci_vendor_data *vendor_data = xhci_to_priv(xhci)->vendor_data;

	if (vendor_data->op_mode == USB_OFFLOAD_SIMPLE_AUDIO_ACCESSORY) {
		xhci->dcbaa = kcalloc(1, sizeof(*xhci->dcbaa), flags);
		if (!xhci->dcbaa)
			return NULL;

		if (xhci_get_dcbaa_ptr(&xhci->dcbaa->dma) != 0) {
			xhci_err(xhci, "Get DCBAA pointer failed\n");
			return NULL;
		}
		xhci_setup_done();

		xhci_dbg(xhci, "write dcbaa_ptr=%llx\n", xhci->dcbaa->dma);
	} else {
		xhci->dcbaa = dma_alloc_coherent(dev, sizeof(*xhci->dcbaa),
						 &dma, flags);
		if (!xhci->dcbaa)
			return NULL;

		xhci->dcbaa->dma = dma;
	}

	return xhci->dcbaa;
}

static void free_dcbaa(struct xhci_hcd *xhci)
{
	struct device *dev = xhci_to_hcd(xhci)->self.sysdev;
	struct xhci_vendor_data *vendor_data = xhci_to_priv(xhci)->vendor_data;

	if (!xhci->dcbaa)
		return;

	if (vendor_data->op_mode == USB_OFFLOAD_SIMPLE_AUDIO_ACCESSORY) {
		kfree(xhci->dcbaa);
	} else {
		dma_free_coherent(dev, sizeof(*xhci->dcbaa),
				  xhci->dcbaa, xhci->dcbaa->dma);
	}

	xhci->dcbaa = NULL;
}

static struct xhci_ring *alloc_transfer_ring(struct xhci_hcd *xhci,
		u32 endpoint_type, enum xhci_ring_type ring_type,
		gfp_t mem_flags)
{
	return xhci_initialize_ring_info_for_remote_isoc(xhci, endpoint_type,
							 ring_type, mem_flags);
}

static void free_transfer_ring(struct xhci_hcd *xhci,
		struct xhci_virt_device *virt_dev, unsigned int ep_index)
{
	struct xhci_vendor_data *vendor_data = xhci_to_priv(xhci)->vendor_data;
	struct xhci_ring *ring;
	struct xhci_ep_ctx *ep_ctx;
	u32 ep_type;

	ring = virt_dev->eps[ep_index].ring;
	ep_ctx = xhci_get_ep_ctx(xhci, virt_dev->out_ctx, ep_index);
	ep_type = CTX_TO_EP_TYPE(le32_to_cpu(ep_ctx->ep_info2));

	xhci_dbg(xhci, "ep_index=%u, ep_type=%u, ring type=%u\n", ep_index,
		 ep_type, ring->type);

	if (vendor_data->op_mode != USB_OFFLOAD_STOP &&
	    ring->type == TYPE_ISOC) {
		kfree(ring->first_seg);
		kfree(virt_dev->eps[ep_index].ring);
	} else
		xhci_ring_free(xhci, virt_dev->eps[ep_index].ring);

	virt_dev->eps[ep_index].ring = NULL;
}

static bool usb_offload_skip_urb(struct xhci_hcd *xhci, struct urb *urb)
{
	struct xhci_virt_device *vdev = xhci->devs[urb->dev->slot_id];
	struct usb_endpoint_descriptor *desc = &urb->ep->desc;
	int ep_type = usb_endpoint_type(desc);
	unsigned int ep_index;

	if (ep_type == USB_ENDPOINT_XFER_CONTROL)
		ep_index = (unsigned int)(usb_endpoint_num(desc)*2);
	else
		ep_index = (unsigned int)(usb_endpoint_num(desc)*2) +
			   (usb_endpoint_dir_in(desc) ? 1 : 0) - 1;

	xhci_dbg(xhci, "ep_index=%u, ep_type=%d\n", ep_index, ep_type);

	if (is_usb_offload_enabled(xhci, vdev, ep_index))
		return true;

	return false;
}

static struct xhci_vendor_ops ops = {
	.vendor_init = usb_audio_offload_init,
	.vendor_cleanup = usb_audio_offload_cleanup,
	.is_usb_offload_enabled = is_usb_offload_enabled,
	.queue_irq_work = queue_irq_work,
	.alloc_dcbaa = alloc_dcbaa,
	.free_dcbaa = free_dcbaa,
	.alloc_transfer_ring = alloc_transfer_ring,
	.free_transfer_ring = free_transfer_ring,
	.sync_dev_ctx = sync_dev_ctx,
	.usb_offload_skip_urb = usb_offload_skip_urb,
};

int xhci_vendor_helper_init(void)
{
	return xhci_plat_register_vendor_ops(&ops);
}
