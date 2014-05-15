/*
 * dummy.c - infrastructure for Composite USB Gadgets
 *
 * Copyright (C) 2014 Krzysztof Opasiak
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/platform_device.h>

#include <linux/usb/dummy_usb.h>

MODULE_DESCRIPTION("Driver for virtual udc");
MODULE_AUTHOR("Krzysztof Opasiak");
MODULE_LICENSE("GPL");

static LIST_HEAD(hcd_drv_list);
static DEFINE_MUTEX(hcd_drv_lock);

static LIST_HEAD(udc_drv_list);
static DEFINE_MUTEX(udc_drv_lock);

#define EMPTY_FUNC_RET_0(HW_TYPE, NAME, ...)			\
	static int virtual_usb_##HW_TYPE##_empty_##NAME( __VA_ARGS__ ) { return 0;}

#define CHECK_FUNC_AND_SET_IF_NULL(hw, ptr, field)	\
	if (!ptr->field) { \
		ptr->field = virtual_usb_##hw##_empty_##field; \
	}


/* somme common accessors */
static inline struct virtual_usb_udc *gadget_to_udc(struct usb_gadget *g)
{
	return container_of(g, struct virtual_usb_udc, gadget);
}

static inline struct device *gadget_to_dev(struct usb_gadget *g)
{
	return g->dev.parent;
}

static inline struct virtual_usb_udc *virtual_ep_to_udc(struct virtual_usb_ep *ep)
{
	return container_of(ep->gadget, struct virtual_usb_udc, gadget);
}

static inline struct virtual_usb_ep *ep_to_virtual_ep(struct usb_ep *_ep)
{
	return container_of(_ep, struct virtual_usb_ep, ep);
}

static int is_ep0(struct usb_ep *_ep)
{
	struct virtual_usb_udc *udc;

	udc = virtual_ep_to_udc(ep_to_virtual_ep(_ep));
	
	return _ep->name == udc->udc_drv->ep_name[0];
}

static inline struct virtual_usb_request *req_to_virtual_req(struct usb_request *_req)
{
	return container_of(_req, struct virtual_usb_request, req);
}

static inline struct usb_hcd *virtual_hcd_to_hcd(struct virtual_usb_hcd_instance *i)
{
	return container_of((void *)i, struct usb_hcd, hcd_priv);
}

static inline struct virtual_usb_hcd_instance *hcd_to_virtual_hcd(struct usb_hcd *h)
{
	return (struct virtual_usb_hcd_instance *) h->hcd_priv;
}

static inline struct device *virtual_hcd_to_dev(struct virtual_usb_hcd_instance *i)
{
	return virtual_hcd_to_hcd(i)->self.controller;
}

/* Virtual HCD related functions */

void virtual_usb_hcd_unregister(struct virtual_usb_hcd_driver *h)
{
	platform_driver_unregister(&h->driver);

	mutex_lock(&hcd_drv_lock);
	list_del(&h->list);
	mutex_unlock(&hcd_drv_lock);
}
EXPORT_SYMBOL(virtual_usb_hcd_unregister);
/* ----- transport layer ----- */

static void virtual_usb_timer(unsigned long _dum_hcd)
{

}

static int virtual_usb_get_frame(void)
{
	struct timeval	tv;

	do_gettimeofday(&tv);
	return tv.tv_usec / 1000;
}

/* ----- HCD driver ops ----- */

/* Kind of second phase of probe */
static int virtual_hcd_setup(struct usb_hcd *hcd)
{
	struct virtual_usb_hcd_instance *xx_hcd;


	xx_hcd = hcd_to_virtual_hcd(hcd);
	xx_hcd->parent = *((void **)dev_get_platdata(hcd->self.controller));
	hcd->self.sg_tablesize = ~0;
	if (usb_hcd_is_primary_hcd(hcd)) {
		/* primary hcd is marked always as 2.0 */
		hcd->speed = HCD_USB2;
		hcd->self.root_hub->speed = USB_SPEED_HIGH;
		xx_hcd->parent->hs_hcd = xx_hcd;
	} else {
		/* if secondary hcd is present it is 3.0 */
		hcd->speed = HCD_USB3;
		hcd->self.root_hub->speed = USB_SPEED_SUPER;
		xx_hcd->parent->hs_hcd = xx_hcd;
	}

	return 0;
}

static int virtual_hcd_start(struct usb_hcd *hcd)
{
	struct virtual_usb_hcd_instance *xx_hcd;

	xx_hcd = hcd_to_virtual_hcd(hcd);
	/*
	 * MASTER side init ... we emulate a root hub that'll only ever
	 * talk to one device (the slave side).  Also appears in sysfs,
	 * just like more familiar pci-based HCDs.
	 */
	init_timer(&xx_hcd->timer);
	xx_hcd->timer.function = virtual_usb_timer;
	xx_hcd->timer.data = (unsigned long)xx_hcd;
	xx_hcd->rh_state = VIRTUAL_RH_RUNNING;

	/* Check if we have 3.0 hcd */
	if (!usb_hcd_is_primary_hcd(hcd))
		xx_hcd->stream_en_ep = 0;

	INIT_LIST_HEAD(&xx_hcd->urbp_list);

	hcd->power_budget = xx_hcd->parent->power_budget;
	hcd->state = HC_STATE_RUNNING;
	hcd->uses_new_polling = 1;

#ifdef CONFIG_USB_OTG
	hcd->self.otg_port = 1;
#endif

	return 0;
}

static void virtual_hcd_stop(struct usb_hcd *hcd)
{
	struct virtual_usb_hcd_instance *xx_hcd;

	xx_hcd = hcd_to_virtual_hcd(hcd);
/* We are not connected so nothing special to do */
/* TODO add version when connected */
	dev_info(virtual_hcd_to_dev(xx_hcd), "stopped\n");
}

static int virtual_hcd_urb_enqueue(
	struct usb_hcd			*hcd,
	struct urb			*urb,
	gfp_t				mem_flags)
{
	/* Stab */
	printk(__func__);
	return 0;
}

static int virtual_hcd_urb_dequeue(struct usb_hcd *hcd, struct urb *urb, int status)
{
	/* Stab */
	printk(__func__);
	return 0;
}


static int virtual_hcd_get_frame(struct usb_hcd *hcd)
{
	return virtual_usb_get_frame();
}

/* If any of those bits is true then the state of port has been changed */
#define PORT_C_MASK \
	((USB_PORT_STAT_C_CONNECTION \
	| USB_PORT_STAT_C_ENABLE \
	| USB_PORT_STAT_C_SUSPEND \
	| USB_PORT_STAT_C_OVERCURRENT \
	| USB_PORT_STAT_C_RESET) << 16)

static int virtual_hcd_hub_status(struct usb_hcd *_hcd, char *buf)
{
	struct virtual_usb_hcd_instance *xx_hcd;
	struct virtual_usb_hcd *hcd;
	int ret = 0;
	unsigned long flags;

	xx_hcd = hcd_to_virtual_hcd(_hcd);
	hcd = xx_hcd->parent;

	local_irq_save(flags);
	spin_lock(&hcd->lock);
	/* Check if we have some clients connected */
	if (hcd->link) {
		spin_lock(&hcd->link->lock);
		spin_unlock(&hcd->lock);
		if (!HCD_HW_ACCESSIBLE(_hcd))
			goto unlock_link;

		if (xx_hcd->resuming && time_after_eq(jiffies, xx_hcd->re_timeout)) {
			xx_hcd->port_status |= (USB_PORT_STAT_C_SUSPEND << 16);
			xx_hcd->port_status &= ~USB_PORT_STAT_SUSPEND;
			/* TODO set link state
			   set_link_state(xx_hcd);*/
		}

		if ((xx_hcd->port_status & PORT_C_MASK) != 0) {
			*buf = (1 << 1);
			dev_dbg(virtual_hcd_to_dev(xx_hcd), "port status 0x%08x has changes\n",
				xx_hcd->port_status);
			ret = 1;
			if (xx_hcd->rh_state == VIRTUAL_RH_SUSPENDED)
				usb_hcd_resume_root_hub(_hcd);
		}

	unlock_link:
		spin_unlock(&hcd->link->lock);
	} else {
		/* we are not connected */
		if (!HCD_HW_ACCESSIBLE(_hcd))
			goto unlock_hcd;

		if (xx_hcd->resuming && time_after_eq(jiffies, xx_hcd->re_timeout)) {
			xx_hcd->port_status |= (USB_PORT_STAT_C_SUSPEND << 16);
			xx_hcd->port_status &= ~USB_PORT_STAT_SUSPEND;
			/* Not connected so don't set the link state */
		}

		if ((xx_hcd->port_status & PORT_C_MASK) != 0) {
			*buf = (1 << 1);
			dev_dbg(virtual_hcd_to_dev(xx_hcd), "port status 0x%08x has changes\n",
				xx_hcd->port_status);
			ret = 1;
			if (xx_hcd->rh_state == VIRTUAL_RH_SUSPENDED)
				usb_hcd_resume_root_hub(_hcd);
		}

	unlock_hcd:
		spin_unlock(&hcd->lock);
	}
	
	local_irq_restore(flags);

	return ret;
}

static int virtual_usb_fill_bos_desc(void *buf)
{
        /* usb 3.0 root hub device descriptor */
	static struct {
		struct usb_bos_descriptor bos;
		struct usb_ss_cap_descriptor ss_cap;
	} __packed usb3_bos_desc = {

		.bos = {
			.bLength		= USB_DT_BOS_SIZE,
			.bDescriptorType	= USB_DT_BOS,
			.wTotalLength		= cpu_to_le16(sizeof(usb3_bos_desc)),
			.bNumDeviceCaps		= 1,
		},
		.ss_cap = {
			.bLength		= USB_DT_USB_SS_CAP_SIZE,
			.bDescriptorType	= USB_DT_DEVICE_CAPABILITY,
			.bDevCapabilityType	= USB_SS_CAP_TYPE,
			.wSpeedSupported	= cpu_to_le16(USB_5GBPS_OPERATION),
			.bFunctionalitySupport	= ilog2(USB_5GBPS_OPERATION),
		},
	};

	*(typeof(&usb3_bos_desc))buf = usb3_bos_desc;
	return sizeof(usb3_bos_desc);
}
static void virtual_usb_fill_hub_desc(struct usb_hub_descriptor *desc,
				      enum usb_device_speed speed)
{
	memset(desc, 0, sizeof *desc);
	desc->wHubCharacteristics = cpu_to_le16(0x0001);
	desc->bNbrPorts = 1;

	if (speed == USB_SPEED_SUPER) {
		desc->bDescriptorType = 0x2a;
		desc->bDescLength = 12;
		desc->u.ss.bHubHdrDecLat = 0x04; /* Worst case: 0.4 micro sec*/
		desc->u.ss.DeviceRemovable = 0xffff;
	} else {
		desc->bDescriptorType = 0x29;
		desc->bDescLength = 9;
		desc->u.hs.DeviceRemovable[0] = 0xff;
		desc->u.hs.DeviceRemovable[1] = 0xff;
	}
}

/* should be called with locks taken */
static int virtual_hcd_hub_dispatch_control(
	struct usb_hcd	*hcd,
	u16		typeReq,
	u16		wValue,
	u16		wIndex,
	char		*buf,
	u16		wLength,
	struct virtual_usb_hcd_instance *xx_hcd,
	int is_connected)
{
	int ret = 0;

	switch (typeReq) {
	case ClearHubFeature:
		break;
	case ClearPortFeature:
		switch (wValue) {
		case USB_PORT_FEAT_SUSPEND:
			if (hcd->speed == HCD_USB3) {
				dev_dbg(virtual_hcd_to_dev(xx_hcd),
					 "USB_PORT_FEAT_SUSPEND req not "
					 "supported for USB 3.0 roothub\n");
				goto error;
			}
			if (xx_hcd->port_status & USB_PORT_STAT_SUSPEND) {
				/* 20msec resume signaling */
				xx_hcd->resuming = 1;
				xx_hcd->re_timeout = jiffies +
						msecs_to_jiffies(20);
			}
			break;
		case USB_PORT_FEAT_POWER:
			if (hcd->speed == HCD_USB3) {
				if (xx_hcd->port_status & USB_PORT_STAT_POWER)
					dev_dbg(virtual_hcd_to_dev(xx_hcd),
						"power-off\n");
			} else
				if (xx_hcd->port_status &
							USB_SS_PORT_STAT_POWER)
					dev_dbg(virtual_hcd_to_dev(xx_hcd),
						"power-off\n");
			/* FALLS THROUGH */
		default:
			xx_hcd->port_status &= ~(1 << wValue);
			/* TODO should link state be changed when not connected */
			printk("USB ClearPortFeature");
			/*set_link_state(xx_hcd);*/
		}
		break;
	case GetHubDescriptor:
		if (hcd->speed == HCD_USB3 &&
				(wLength < USB_DT_SS_HUB_SIZE ||
				 wValue != (USB_DT_SS_HUB << 8))) {
			dev_dbg(virtual_hcd_to_dev(xx_hcd),
				"Wrong hub descriptor type for "
				"USB 3.0 roothub.\n");
			goto error;
		}
		virtual_usb_fill_hub_desc((struct usb_hub_descriptor *) buf, hcd->speed);
		break;

	case DeviceRequest | USB_REQ_GET_DESCRIPTOR:
		if (hcd->speed != HCD_USB3)
			goto error;

		if ((wValue >> 8) != USB_DT_BOS)
			goto error;

		ret = virtual_usb_fill_bos_desc(buf);
		break;

	case GetHubStatus:
		*(__le32 *) buf = cpu_to_le32(0);
		break;
	case GetPortStatus:
		/* We have only one port currently */
		if (wIndex != 1)
			ret = -EPIPE;

		/* whoever resets or resumes must GetPortStatus to
		 * complete it!!
		 */
		if (xx_hcd->resuming &&
				time_after_eq(jiffies, xx_hcd->re_timeout)) {
			xx_hcd->port_status |= (USB_PORT_STAT_C_SUSPEND << 16);
			xx_hcd->port_status &= ~USB_PORT_STAT_SUSPEND;
		}
		if ((xx_hcd->port_status & USB_PORT_STAT_RESET) != 0 &&
				time_after_eq(jiffies, xx_hcd->re_timeout)) {
			xx_hcd->port_status |= (USB_PORT_STAT_C_RESET << 16);
			xx_hcd->port_status &= ~USB_PORT_STAT_RESET;
			/* TODO avoid direct getting device data */
			if (is_connected && xx_hcd->parent->link->device->pullup) {
				xx_hcd->port_status |= USB_PORT_STAT_ENABLE;

				if (hcd->speed < HCD_USB3) {
					/* Check our connection speed */
					switch (xx_hcd->parent->link->speed) {
					case USB_SPEED_HIGH:
						xx_hcd->port_status |=
						      USB_PORT_STAT_HIGH_SPEED;
						break;
					case USB_SPEED_LOW:
						/* TODO put in in connection method
						   dum_udc->gadget.ep0->
						   maxpacket = 8; */
						xx_hcd->port_status |=
							USB_PORT_STAT_LOW_SPEED;
						break;
					default:
						/* TODO put it in connection method
						xx_udc->gadget.speed =
						USB_SPEED_FULL;*/
						break;
					}
				}
			}
		}
		/* TODO check if connected and set state
		   set_link_state(xx_hcd);*/
		((__le16 *) buf)[0] = cpu_to_le16(xx_hcd->port_status);
		((__le16 *) buf)[1] = cpu_to_le16(xx_hcd->port_status >> 16);
		break;
	case SetHubFeature:
		ret = -EPIPE;
		break;
	case SetPortFeature:
		switch (wValue) {
		case USB_PORT_FEAT_LINK_STATE:
			if (hcd->speed != HCD_USB3) {
				dev_dbg(virtual_hcd_to_dev(xx_hcd),
					 "USB_PORT_FEAT_LINK_STATE req not "
					 "supported for USB 2.0 roothub\n");
				goto error;
			}
			/*
			 * Since this is dummy we don't have an actual link so
			 * there is nothing to do for the SET_LINK_STATE cmd
			 */
			break;
		case USB_PORT_FEAT_U1_TIMEOUT:
		case USB_PORT_FEAT_U2_TIMEOUT:
			/* TODO: add suspend/resume support! */
			if (hcd->speed != HCD_USB3) {
				dev_dbg(virtual_hcd_to_dev(xx_hcd),
					 "USB_PORT_FEAT_U1/2_TIMEOUT req not "
					 "supported for USB 2.0 roothub\n");
				goto error;
			}
			break;
		case USB_PORT_FEAT_SUSPEND:
			/* Applicable only for USB2.0 hub */
			if (hcd->speed == HCD_USB3) {
				dev_dbg(virtual_hcd_to_dev(xx_hcd),
					 "USB_PORT_FEAT_SUSPEND req not "
					 "supported for USB 3.0 roothub\n");
				goto error;
			}
			if (xx_hcd->active) {
				xx_hcd->port_status |= USB_PORT_STAT_SUSPEND;

				/* HNP would happen here; for now we
				 * assume b_bus_req is always true.
				 */
				/* TODO when connected set link state
				   set_link_state(xx_hcd);*/
				/* TODO avoid getting data from device structure */
				if (is_connected)
					if (((1 << USB_DEVICE_B_HNP_ENABLE)
					     &xx_hcd->parent->link->device->devstatus) != 0)
						dev_dbg(virtual_hcd_to_dev(xx_hcd),
							"no HNP yet!\n");
			}
			break;
		case USB_PORT_FEAT_POWER:
			if (hcd->speed == HCD_USB3)
				xx_hcd->port_status |= USB_SS_PORT_STAT_POWER;
			else
				xx_hcd->port_status |= USB_PORT_STAT_POWER;

			/* TODO if connected set link state
			   set_link_state(xx_hcd);*/
			break;
		case USB_PORT_FEAT_BH_PORT_RESET:
			/* Applicable only for USB3.0 hub */
			if (hcd->speed != HCD_USB3) {
				dev_dbg(virtual_hcd_to_dev(xx_hcd),
					 "USB_PORT_FEAT_BH_PORT_RESET req not "
					 "supported for USB 2.0 roothub\n");
				goto error;
			}
			/* FALLS THROUGH */
		case USB_PORT_FEAT_RESET:
			/* if it's already enabled, disable */
			if (hcd->speed == HCD_USB3) {
				xx_hcd->port_status = 0;
				xx_hcd->port_status =
					(USB_SS_PORT_STAT_POWER |
					 USB_PORT_STAT_CONNECTION |
					 USB_PORT_STAT_RESET);
			} else
				xx_hcd->port_status &= ~(USB_PORT_STAT_ENABLE
					| USB_PORT_STAT_LOW_SPEED
					| USB_PORT_STAT_HIGH_SPEED);
			/*
			 * We want to reset device status. All but the
			 * Self powered feature
			 */
			/* TODO avoid getting direct access to device */
			if (is_connected)
				xx_hcd->parent->link->device->devstatus &=
					(1 << USB_DEVICE_SELF_POWERED);

			/*
			 * FIXME USB3.0: what is the correct reset signaling
			 * interval? Is it still 50msec as for HS?
			 */
			xx_hcd->re_timeout = jiffies + msecs_to_jiffies(50);
			/* FALLS THROUGH */
		default:
			if (hcd->speed == HCD_USB3) {
				if ((xx_hcd->port_status &
				     USB_SS_PORT_STAT_POWER) != 0) {
					xx_hcd->port_status |= (1 << wValue);
					/* TODO if connected
					   set_link_state(xx_hcd); */
				}
			} else
				if ((xx_hcd->port_status &
				     USB_PORT_STAT_POWER) != 0) {
					xx_hcd->port_status |= (1 << wValue);
					/* TODO set link state if connected
					   set_link_state(xx_hcd);*/
				}
		}
		break;
	case GetPortErrorCount:
		if (hcd->speed != HCD_USB3) {
			dev_dbg(virtual_hcd_to_dev(xx_hcd),
				 "GetPortErrorCount req not "
				 "supported for USB 2.0 roothub\n");
			goto error;
		}
		/* We'll always return 0 since this is a virtual hub */
		*(__le32 *) buf = cpu_to_le32(0);
		break;
	case SetHubDepth:
		if (hcd->speed != HCD_USB3) {
			dev_dbg(virtual_hcd_to_dev(xx_hcd),
				 "SetHubDepth req not supported for "
				 "USB 2.0 roothub\n");
			goto error;
		}
		break;
	default:
		dev_dbg(virtual_hcd_to_dev(xx_hcd),
			"hub control req%04x v%04x i%04x l%d\n",
			typeReq, wValue, wIndex, wLength);
error:
		/* "protocol stall" on error */
		ret = -EPIPE;
	}

	return ret;
}

static int virtual_hcd_hub_control(
	struct usb_hcd	*_hcd,
	u16		typeReq,
	u16		wValue,
	u16		wIndex,
	char		*buf,
	u16		wLength)
{
	struct virtual_usb_hcd_instance *xx_hcd;
	struct virtual_usb_hcd *hcd;
	unsigned long flags;
	int ret = 0;

	if (!HCD_HW_ACCESSIBLE(_hcd))
		return -ETIMEDOUT;

	xx_hcd = hcd_to_virtual_hcd(_hcd);
	hcd = xx_hcd->parent;

	local_irq_save(flags);
	spin_lock(&hcd->lock);
	/* Check if we have some clients connected */
	if (hcd->link) {
		spin_lock(&hcd->link->lock);
		spin_unlock(&hcd->lock);

		ret = virtual_hcd_hub_dispatch_control(
			_hcd, typeReq, wValue, wIndex, buf, wLength, xx_hcd, 1);
		spin_unlock(&hcd->link->lock);
	} else {
		/* we are not connected */
		ret = virtual_hcd_hub_dispatch_control(
			_hcd, typeReq, wValue, wIndex, buf, wLength, xx_hcd, 0);
		spin_unlock(&hcd->lock);
	}
	
	local_irq_restore(flags);

	/* TODO what with rh status */

	return ret;
}

static int virtual_hcd_bus_suspend(struct usb_hcd *_hcd)
{
	struct virtual_usb_hcd_instance *xx_hcd;
	struct virtual_usb_hcd *hcd;
	int ret = 0;
	unsigned long flags;

	xx_hcd = hcd_to_virtual_hcd(_hcd);
	hcd = xx_hcd->parent;

	local_irq_save(flags);
	spin_lock(&hcd->lock);
	/* Check if we have some clients connected */
	if (hcd->link) {
		spin_lock(&hcd->link->lock);
		spin_unlock(&hcd->lock);

		xx_hcd->rh_state = VIRTUAL_RH_SUSPENDED;
		/* TODO We are connected what with link state? */
		_hcd->state = HC_STATE_SUSPENDED;

		spin_unlock(&hcd->link->lock);
	} else {
		/* we are not connected */
		xx_hcd->rh_state = VIRTUAL_RH_SUSPENDED;
		_hcd->state = HC_STATE_SUSPENDED;
		spin_unlock(&hcd->lock);
	}
	
	local_irq_restore(flags);

	return ret;
}

static int virtual_hcd_bus_resume(struct usb_hcd *_hcd)
{
	struct virtual_usb_hcd_instance *xx_hcd;
	struct virtual_usb_hcd *hcd;
	int ret = 0;
	unsigned long flags;

	xx_hcd = hcd_to_virtual_hcd(_hcd);
	hcd = xx_hcd->parent;

	local_irq_save(flags);
	spin_lock(&hcd->lock);
	/* Check if we have some clients connected */
	if (hcd->link) {
		spin_lock(&hcd->link->lock);
		spin_unlock(&hcd->lock);
		if (!HCD_HW_ACCESSIBLE(_hcd)) {
			ret = -ESHUTDOWN;
		} else {
		/* TODO We are connected what we should do now? */
		}

		spin_unlock(&hcd->link->lock);
	} else {
		/* we are not connected */
		if (!HCD_HW_ACCESSIBLE(_hcd)) {
			ret = -ESHUTDOWN;
		} else {
			xx_hcd->rh_state = VIRTUAL_RH_RUNNING;
			_hcd->state = HC_STATE_RUNNING;
		}

		spin_unlock(&hcd->lock);
	}
	
	local_irq_restore(flags);

	return ret;
}

/* Change a group of bulk endpoints to support multiple stream IDs */
static int virtual_hcd_alloc_streams(struct usb_hcd *hcd, struct usb_device *udev,
	struct usb_host_endpoint **eps, unsigned int num_eps,
	unsigned int num_streams, gfp_t mem_flags)
{
	/* currently stab only, neccesary only after connect */
	printk(__func__);
	return 0;
}

/* Reverts a group of bulk endpoints back to not using stream IDs. */
static int virtual_hcd_free_streams(struct usb_hcd *hcd, struct usb_device *udev,
	struct usb_host_endpoint **eps, unsigned int num_eps,
	gfp_t mem_flags)
{
	/* currently stab only, neccesary only after connect */
	printk(__func__);
	return 0;
}

/* ----- HCD platform driver functions ----- */

static int virtual_usb_hcd_probe(struct platform_device *pdev)
{
	struct virtual_usb_hcd *hcd;
	struct virtual_usb_hcd_driver *drv;
	struct usb_hcd *hs_hcd = NULL, *ss_hcd = NULL;
	int ret;

	hcd = *((void **)dev_get_platdata(&pdev->dev));
	drv = hcd->hcd_drv;

	if (hcd->max_speed == USB_SPEED_HIGH) {
		drv->hcd_drv.flags = HCD_USB2;
	} else {
		drv->hcd_drv.flags = HCD_USB3 | HCD_SHARED;
	}

	ret = drv->probe(hcd);
	if (ret)
		goto out;

	/* TODO potential place for configfs attr name */
	hs_hcd = usb_create_hcd(&drv->hcd_drv, &pdev->dev, dev_name(&pdev->dev));
	if (!hs_hcd)
		return -ENOMEM;

	hs_hcd->has_tt = 1;

	ret = usb_add_hcd(hs_hcd, 0, 0);
	if (ret)
		goto put_usb2_hcd;

	/* For USB 3.0 we have to create another hcd */
	if (hcd->max_speed == USB_SPEED_SUPER) {
		ss_hcd = usb_create_shared_hcd(&drv->hcd_drv, &pdev->dev,
					       dev_name(&pdev->dev), hs_hcd);

		if (!ss_hcd) {
			ret = -ENOMEM;
			goto dealloc_usb2_hcd;
		}

		ret = usb_add_hcd(ss_hcd, 0, 0);
		if (ret)
			goto put_usb3_hcd;
	}

	/* We cannot set drv data here because it is taken by usb_create_hcd */

	return 0;

put_usb3_hcd:
	usb_put_hcd(ss_hcd);
dealloc_usb2_hcd:
	usb_remove_hcd(hs_hcd);
put_usb2_hcd:
	usb_put_hcd(hs_hcd);
out:
	return ret;
}

static int virtual_usb_hcd_remove(struct platform_device *pdev)
{
	struct virtual_usb_hcd *hcd;
	int ret = 0;

	hcd = *((void **)dev_get_platdata(&pdev->dev));

	ret = hcd->hcd_drv->remove(hcd);
	if (ret)
		goto out;

	if (hcd->ss_hcd) {
		usb_remove_hcd(virtual_hcd_to_hcd(hcd->ss_hcd));
		usb_put_hcd(virtual_hcd_to_hcd(hcd->ss_hcd));
	}

	usb_remove_hcd(virtual_hcd_to_hcd(hcd->hs_hcd));
	usb_put_hcd(virtual_hcd_to_hcd(hcd->hs_hcd));

	hcd->hs_hcd = hcd->ss_hcd = NULL;

out:
	return ret;
}

static int virtual_usb_hcd_suspend(struct platform_device *pdev, pm_message_t state)
{
	struct usb_hcd *_hcd;
	struct virtual_usb_hcd_instance *hs_hcd;
	struct virtual_usb_hcd *hcd;
	int ret = 0;

	/* Driver data contains the primary hub allways so this is 2.0 hub */
	_hcd = platform_get_drvdata(pdev);
	hs_hcd = hcd_to_virtual_hcd(_hcd);
	hcd = hs_hcd->parent;
	
	ret = hcd->hcd_drv->suspend(hcd, state);
	if (ret)
		goto out;

	if (hs_hcd->rh_state == VIRTUAL_RH_RUNNING) {
		dev_warn(&pdev->dev, "Root hub isn't suspended!\n");
		ret = -EBUSY;
	} else {
		clear_bit(HCD_FLAG_HW_ACCESSIBLE, &_hcd->flags);
	}

out:
	return ret;
}

static int virtual_usb_hcd_resume(struct platform_device *pdev)
{
	struct usb_hcd *_hcd;
	struct virtual_usb_hcd_instance *hs_hcd;
	struct virtual_usb_hcd *hcd;
	int ret = 0;

	_hcd = platform_get_drvdata(pdev);
	hs_hcd = hcd_to_virtual_hcd(platform_get_drvdata(pdev));
	hcd = hs_hcd->parent;

	ret = hcd->hcd_drv->resume(hcd);
	if (ret)
		goto out;

	set_bit(HCD_FLAG_HW_ACCESSIBLE, &_hcd->flags);
	usb_hcd_poll_rh_status(_hcd);

out:
	return ret;
}

/* ----- HCD platform driver empty functions stabs ----- */

EMPTY_FUNC_RET_0(hcd, probe, struct virtual_usb_hcd *p1);
EMPTY_FUNC_RET_0(hcd, remove, struct virtual_usb_hcd *p1);
EMPTY_FUNC_RET_0(hcd, suspend, struct virtual_usb_hcd *p1, pm_message_t p2);
EMPTY_FUNC_RET_0(hcd, resume, struct virtual_usb_hcd *p1);

int virtual_usb_hcd_register(struct virtual_usb_hcd_driver *h)
{
	struct virtual_usb_hcd_driver *other;
	struct platform_driver *driver;
	struct hc_driver *hc_drv;
	int ret = -EEXIST;

	INIT_LIST_HEAD(&h->list);

	/* first level */
	CHECK_FUNC_AND_SET_IF_NULL(hcd, h, probe);
	CHECK_FUNC_AND_SET_IF_NULL(hcd, h, remove);
	CHECK_FUNC_AND_SET_IF_NULL(hcd, h, suspend);
	CHECK_FUNC_AND_SET_IF_NULL(hcd, h, resume);

	driver = &h->driver;
	driver->probe = virtual_usb_hcd_probe;
	driver->remove = virtual_usb_hcd_remove;
	driver->suspend = virtual_usb_hcd_suspend;
	driver->resume = virtual_usb_hcd_resume;

	driver->driver.name = h->name;
	driver->driver.owner = h->module;

	hc_drv = &h->hcd_drv;
	hc_drv->description = h->name;
	hc_drv->product_desc = h->description;
	/* Flags are set in probe method according to actual max sped value */
	hc_drv->reset =virtual_hcd_setup;
	hc_drv->start = virtual_hcd_start;
	hc_drv->stop = virtual_hcd_stop;
	hc_drv->urb_enqueue = virtual_hcd_urb_enqueue;
	hc_drv->urb_dequeue = virtual_hcd_urb_dequeue;
	hc_drv->get_frame_number = virtual_hcd_get_frame;
	hc_drv->hub_status_data = virtual_hcd_hub_status;
	hc_drv->hub_control = virtual_hcd_hub_control;
	hc_drv->bus_suspend = virtual_hcd_bus_suspend;
	hc_drv->bus_resume = virtual_hcd_bus_resume;
	hc_drv->alloc_streams = virtual_hcd_alloc_streams;
	hc_drv->free_streams = virtual_hcd_free_streams;
	
	mutex_lock(&hcd_drv_lock);
	list_for_each_entry(other, &hcd_drv_list, list) {
		if (!strcmp(other->name, h->name))
			goto out;
	}

	ret = 0;
	list_add_tail(&h->list, &hcd_drv_list);
	mutex_unlock(&hcd_drv_lock);
	
	platform_driver_register(driver);
	return ret;
out:
	mutex_unlock(&hcd_drv_lock);
	return ret;
}
EXPORT_SYMBOL(virtual_usb_hcd_register);

static struct virtual_usb_hcd *alloc_new_hcd(struct virtual_usb_hcd_driver *drv, int id)
{
	struct virtual_usb_hcd *newh;
	int ret;

	newh = kzalloc(sizeof(*newh), GFP_KERNEL);
	if (IS_ERR(newh))
		goto out;

	spin_lock_init(&newh->lock);
	newh->hcd_drv = drv;
	/* By default we use high speed device */
	newh->max_speed = USB_SPEED_HIGH;
	newh->hcd_dev = platform_device_alloc(drv->name, id);
	newh->id = newh->hcd_dev->id;

	ret = platform_device_add_data(newh->hcd_dev, &newh, sizeof(void *));
	if (ret) {
		platform_device_put(newh->hcd_dev);
		kfree(newh);
		newh = ERR_PTR(ret);
	}

out:
	return newh;
}

static struct virtual_usb_hcd *try_get_virtual_hcd(const char *driver, int id)
{
	struct virtual_usb_hcd_driver *drv;
	struct virtual_usb_hcd *newh;
	
	newh = ERR_PTR(-ENOENT);
	mutex_lock(&hcd_drv_lock);
	list_for_each_entry(drv, &hcd_drv_list, list) {
		if (strcmp(drv->name, driver))
			continue;

		if (!try_module_get(drv->module)) {
			newh = ERR_PTR(-EBUSY);
			break;
		}
		
		newh = alloc_new_hcd(drv, id);
		if (IS_ERR(newh))
			module_put(drv->module);			
		
		break;
	}

	mutex_unlock(&hcd_drv_lock);

	return newh;
}

struct virtual_usb_hcd *virtual_usb_alloc_hcd(const char *driver, int id)
{
	struct virtual_usb_hcd *newh;
	int ret;

	newh = try_get_virtual_hcd(driver, id);

	if (IS_ERR(newh) && PTR_ERR(newh) == -ENOENT) {
		ret = request_module("virtual_usb_hcd:%s", driver);

		newh = ret < 0 ? ERR_PTR(ret)
			: try_get_virtual_hcd(driver, id);
	}

	return newh;
}
EXPORT_SYMBOL(virtual_usb_alloc_hcd);

void virtual_usb_put_hcd(struct virtual_usb_hcd *h)
{
	struct module *mod;

	if (!h)
		return;

	mod = h->hcd_drv->module;
	kfree(h->data);
	kfree(h);
	module_put(mod);
}
EXPORT_SYMBOL(virtual_usb_put_hcd);

int virtual_usb_hcd_add_data(struct virtual_usb_hcd *h, const void *data, size_t size)
{
	void *d = NULL;
	int ret = -ENOMEM;
	
	if (data) {
		d = kmemdup(data, size, GFP_KERNEL);
		if (!d)
			goto out;
	}

	kfree(h->data);
	h->data = d;
	ret = 0;
out:
	return ret;
}
EXPORT_SYMBOL(virtual_usb_hcd_add_data);

void virtual_usb_del_hcd(struct virtual_usb_hcd *h)
{
	if (!h)
		return;

	platform_device_del(h->hcd_dev);
}
EXPORT_SYMBOL(virtual_usb_del_hcd);

int virtual_usb_add_hcd(struct virtual_usb_hcd *h)
{
	int ret;

	ret = platform_device_add(h->hcd_dev);
	if (ret)
		goto out;

	/* Check if probe() was successful */
	if (!h->hs_hcd || (h->max_speed == USB_SPEED_SUPER && !h->ss_hcd)) {
		platform_device_del(h->hcd_dev);
		ret = -EINVAL;
	}
out:
	return ret;
}
EXPORT_SYMBOL(virtual_usb_add_hcd);

void virtual_usb_rm_hcd(struct virtual_usb_hcd *h)
{
	virtual_usb_del_hcd(h);
	virtual_usb_put_hcd(h);
}
EXPORT_SYMBOL(virtual_usb_rm_hcd);

/* Virtual UDC related functions */

/* ----- Endpoints operations ----- */
static int virtual_ep_enable(struct usb_ep *_ep,
		const struct usb_endpoint_descriptor *desc)
{
	printk(__func__);
	return 0;
}

static int virtual_ep_disable(struct usb_ep *_ep)
{
	printk(__func__);
	return 0;
}

static struct usb_request *virtual_ep_alloc_request(struct usb_ep *_ep,
				    gfp_t mem_flags)
{
	struct virtual_usb_request *req;

	if (!_ep)
		return NULL;

	req = kzalloc(sizeof(*req), mem_flags);
	if (!req)
		return NULL;
	INIT_LIST_HEAD(&req->queue);

	printk(__func__);
	return &req->req;
}

static void virtual_ep_free_request(struct usb_ep *_ep, struct usb_request *_req)
{
	struct virtual_usb_request *req;

	if (!_ep || !_req) {
		WARN_ON(1);
		return;
	}

	req = req_to_virtual_req(_req);
	WARN_ON(!list_empty(&req->queue));
	kfree(req);

	printk(__func__);
	return;
}

static int virtual_ep_queue(struct usb_ep *_ep, struct usb_request *_req,
			    gfp_t mem_flags)
{
	printk(__func__);
	return 0;
}

static int virtual_ep_dequeue(struct usb_ep *_ep, struct usb_request *_req)
{
	struct virtual_usb_ep *ep;
	struct virtual_usb_udc *udc;
	int ret = -EINVAL;

	if (!_ep || !_req)
		goto out;

	ep = ep_to_virtual_ep(_ep);
	udc = virtual_ep_to_udc(ep);

	if (!udc->driver)
		return -ESHUTDOWN;

	printk(__func__);
	return 0;
out:
	return ret;
}

static int virtual_ep_set_halt_and_wedge(struct usb_ep *_ep, int val, int wedged)
{
	struct virtual_usb_ep *ep;
	struct virtual_usb_udc *udc;

	if (!_ep)
		return -EINVAL;

	ep = ep_to_virtual_ep(_ep);
	udc = virtual_ep_to_udc(ep);

	if (!udc->driver)
		return -ESHUTDOWN;

	if (!val)
		ep->halted = ep->wedged = 0;
	else if (ep->desc && (ep->desc->bEndpointAddress & USB_DIR_IN) &&
		 !list_empty(&ep->queue))
		return -EAGAIN;
	else {
		ep->halted = 1;
		if (wedged)
			ep->wedged = 1;
	}
	/* FIXME clear emulated data toggle too */
	return 0;
}

static int virtual_ep_set_halt(struct usb_ep *_ep, int value)
{
	printk(__func__);
	return 0;
	return virtual_ep_set_halt_and_wedge(_ep, value, 0);
}

static int virtual_ep_set_wedge(struct usb_ep *_ep)
{
	/* We can't wedge ep0 */
	if (!_ep || is_ep0(_ep))
		return -EINVAL;

	printk(__func__);
	return 0;

	return virtual_ep_set_halt_and_wedge(_ep, 1, 1);
}

/* ----- Gadget operations ----- */

static int virtual_gadget_get_frame(struct usb_gadget *_gadget)
{
	return virtual_usb_get_frame();
}

static int virtual_gadget_wakeup(struct usb_gadget *_gadget)
{
	struct virtual_usb_udc *udc = gadget_to_udc(_gadget);

	if (!(udc->devstatus & ((1 << USB_DEVICE_B_HNP_ENABLE)
				| (1 << USB_DEVICE_REMOTE_WAKEUP))))
		return -EINVAL;

	/* TODO what should we do here? */
	return 0;
}

static int virtual_gadget_set_selfpowered(struct usb_gadget *_gadget, int value)
{
	struct virtual_usb_udc *udc = gadget_to_udc(_gadget);

	if (value)
		udc->devstatus |= (1 << USB_DEVICE_SELF_POWERED);
	else
		udc->devstatus &= ~(1 << USB_DEVICE_SELF_POWERED);
	return 0;
}

static int virtual_gadget_pullup(struct usb_gadget *_gadget, int value)
{
	struct virtual_usb_udc *udc = gadget_to_udc(_gadget);
	unsigned long flags;

	/* check if this call is to enable pullup and
	   if gadget driver has been already choosen */
	if (value && udc->driver) {
		udc->gadget.speed = min_t(u8, udc->max_speed,
					  udc->driver->max_speed);

		if (udc->gadget.speed < udc->driver->max_speed)
			dev_dbg(gadget_to_dev(_gadget), "This device can perform faster"
				" if you connect it to a %s port...\n",
				usb_speed_string(udc->driver->max_speed));
	}

	local_irq_save(flags);
	spin_lock(&udc->lock);
	/* We are connected to some hcd. Block connection for us */
	if (udc->link) {
		spin_lock(&udc->link->lock);
		/* We have connectin lock so this is not neccesary */
		spin_unlock(&udc->lock);

		/* TODO add connection state modification */

		spin_unlock(&udc->link->lock);
	} else {
		udc->pullup = (value != 0);
		spin_unlock(&udc->lock);
	}
	local_irq_restore(flags);

	return 0;
}

static int virtual_gadget_udc_start(struct usb_gadget *g,
	struct usb_gadget_driver *driver)
{
	struct virtual_usb_udc *udc = gadget_to_udc(g);

	if (driver->max_speed == USB_SPEED_UNKNOWN)
		return -EINVAL;

	/*
	 * SLAVE side init ... the layer above hardware, which
	 * can't enumerate without help from the driver we're binding.
	 */

	udc->devstatus = 0;

	udc->driver = driver;
	dev_dbg(gadget_to_dev(g), "binding gadget driver '%s'\n",
			driver->driver.name);
	return 0;

}

static int virtual_gadget_udc_stop(struct usb_gadget *g,
	struct usb_gadget_driver *driver)
{
	struct virtual_usb_udc *udc = gadget_to_udc(g);
	if (driver)
		dev_dbg(gadget_to_dev(g),  "unregister gadget driver '%s'\n",
				driver->driver.name);

	udc->driver = NULL;
	return 0;
}

/* ----- UDC platform driver functions ----- */

static void cleanup_virtual_udc_hw(struct virtual_usb_udc *udc)
{
	kfree(udc->ep);
	udc->ep = NULL;
}

static int init_virtual_udc_hw(struct virtual_usb_udc *udc)
{
	struct virtual_usb_udc_driver *drv = udc->udc_drv;
	struct virtual_usb_ep *ep;
	int size = sizeof(struct virtual_usb_ep) + drv->ep_priv_data_size;
	int ret = -ENOMEM;
	int i;

	INIT_LIST_HEAD(&udc->gadget.ep_list);

	ep = kcalloc(drv->ep_nmb, size, GFP_KERNEL);
	if (!ep)
		goto out;

	udc->ep = ep;

	for (i = 0; i < drv->ep_nmb && drv->ep_name[i]; ++i) {
		ep->ep.name = drv->ep_name[i];
		ep->ep.ops = &drv->ep_ops;
		list_add_tail(&ep->ep.ep_list, &udc->gadget.ep_list);
		ep->halted = ep->wedged = ep->already_seen =
				ep->setup_stage = 0;
		usb_ep_set_maxpacket_limit(&ep->ep, ~0);
		ep->ep.max_streams = 16;
		ep->last_io = jiffies;
		ep->gadget = &udc->gadget;
		ep->desc = NULL;
		INIT_LIST_HEAD(&ep->queue);
		/* go to next ep in array */
		ret = drv->init_ep(ep);
		if (ret)
			goto err;
		ep = (struct virtual_usb_ep *)((uint8_t *)ep + size);
	}

	udc->gadget.ep0 = &((struct virtual_usb_ep *)udc->ep)->ep;
	list_del_init(&udc->gadget.ep0->ep_list);
	INIT_LIST_HEAD(&udc->fifo_req.queue);

#ifdef CONFIG_USB_OTG
	udc->gadget.is_otg = 1;
#endif
	ret = 0;

	return ret;

err:
	/* clear the list */
	INIT_LIST_HEAD(&udc->gadget.ep_list);
	kfree(udc->ep);
	udc->ep = NULL;
out:
	return ret;
}

static int virtual_usb_udc_probe(struct platform_device *pdev)
{
	struct virtual_usb_udc *udc;
	struct virtual_usb_udc_driver *drv;
	struct usb_gadget *gadget;
	int ret;

	udc = *((void **)dev_get_platdata(&pdev->dev));

	drv = udc->udc_drv;


	gadget = &udc->gadget;

	if (!gadget->name)
		gadget->name = drv->name;


	gadget->max_speed = udc->max_speed;
	gadget->dev.parent = &pdev->dev;
	gadget->ops = &drv->g_ops;

	/* init our virtual hardware at this point */
	ret = init_virtual_udc_hw(udc);
	if (ret)
		goto out;

	ret = drv->probe(udc);
	if (ret)
		goto cleanup;

	ret = usb_add_gadget_udc(&pdev->dev, gadget);
	if (ret < 0)
		goto err_remove;
	
	platform_set_drvdata(pdev, udc);
	
	return ret;

err_remove:
	drv->remove(udc);
cleanup:
	cleanup_virtual_udc_hw(udc);
out:
	return ret;
}

static int virtual_usb_udc_remove(struct platform_device *pdev)
{
	struct virtual_usb_udc *udc;
	struct virtual_usb_udc_driver *drv;
	int ret;

	udc = *((void **)dev_get_platdata(&pdev->dev));
	drv = udc->udc_drv;

	ret = drv->remove(udc);
	if (ret)
		goto out;

	usb_del_gadget_udc(&udc->gadget);

	cleanup_virtual_udc_hw(udc);
out:
	return ret;
}

static int virtual_usb_udc_pm(struct virtual_usb_udc *udc, int suspend)
{
	unsigned long flags;

	local_irq_save(flags);
	spin_lock(&udc->lock);
	/* We are connected to some hcd. Block connection for us */
	if (udc->link) {
		spin_lock(&udc->link->lock);
		/* We have connectin lock so this is not neccesary */
		spin_unlock(&udc->lock);

		/* TODO add connection state modification */

		spin_unlock(&udc->link->lock);
	} else {
		udc->suspended = suspend;
		spin_unlock(&udc->lock);
	}
	local_irq_restore(flags);

	return 0;
}

static int virtual_usb_udc_suspend(struct platform_device *pdev, pm_message_t state)
{
	struct virtual_usb_udc *udc;
	struct virtual_usb_udc_driver *drv;
	int ret;

	udc = *((void **)dev_get_platdata(&pdev->dev));
	drv = udc->udc_drv;

	ret = drv->suspend(udc, state);
	if (ret)
		goto out;

	virtual_usb_udc_pm(udc, 1);
out:
	return ret;
}

static int virtual_usb_udc_resume(struct platform_device *pdev)
{
	struct virtual_usb_udc *udc;
	struct virtual_usb_udc_driver *drv;
	int ret;

	udc = *((void **)dev_get_platdata(&pdev->dev));
	drv = udc->udc_drv;

	ret = drv->resume(udc);
	if (ret)
		goto out;

	virtual_usb_udc_pm(udc, 0);
out:
	return ret;
}

/* ----- UDC platform driver empty functions stabs ----- */

EMPTY_FUNC_RET_0(udc, probe, struct virtual_usb_udc *p1);
EMPTY_FUNC_RET_0(udc, remove, struct virtual_usb_udc *p1);
EMPTY_FUNC_RET_0(udc, suspend, struct virtual_usb_udc *p1, pm_message_t p2);
EMPTY_FUNC_RET_0(udc, resume, struct virtual_usb_udc *p1);
EMPTY_FUNC_RET_0(udc, init_ep, struct virtual_usb_ep *p1);

/* Virtual UDC driver registration/unregistration */
void virtual_usb_udc_unregister(struct virtual_usb_udc_driver *u)
{
	platform_driver_unregister(&u->driver);

	mutex_lock(&udc_drv_lock);
	list_del(&u->list);
	mutex_unlock(&udc_drv_lock);
}
EXPORT_SYMBOL(virtual_usb_udc_unregister);

int virtual_usb_udc_register(struct virtual_usb_udc_driver *u)
{
	struct virtual_usb_udc_driver *other;
	struct platform_driver *driver;
	int ret = -EEXIST;

	INIT_LIST_HEAD(&u->list);

	/* first level */
	CHECK_FUNC_AND_SET_IF_NULL(udc, u, probe);
	CHECK_FUNC_AND_SET_IF_NULL(udc, u, remove);
	CHECK_FUNC_AND_SET_IF_NULL(udc, u, suspend);
	CHECK_FUNC_AND_SET_IF_NULL(udc, u, resume);
	CHECK_FUNC_AND_SET_IF_NULL(udc, u, init_ep);

	driver = &u->driver;
	driver->probe = virtual_usb_udc_probe;
	driver->remove = virtual_usb_udc_remove;
	driver->suspend = virtual_usb_udc_suspend;
	driver->resume = virtual_usb_udc_resume;

	driver->driver.name = u->name;
	driver->driver.owner = u->module;

	/* Fill gadget ops */
	u->g_ops.get_frame = virtual_gadget_get_frame;
	u->g_ops.wakeup = virtual_gadget_wakeup;
	u->g_ops.set_selfpowered = virtual_gadget_set_selfpowered;
	u->g_ops.pullup = virtual_gadget_pullup;
	u->g_ops.udc_start = virtual_gadget_udc_start;
	u->g_ops.udc_stop = virtual_gadget_udc_stop;

	/* Fill endpoint ops */
	u->ep_ops.enable = virtual_ep_enable;
	u->ep_ops.disable = virtual_ep_disable;
	u->ep_ops.alloc_request = virtual_ep_alloc_request;
	u->ep_ops.free_request = virtual_ep_free_request;
	u->ep_ops.queue = virtual_ep_queue;
	u->ep_ops.dequeue = virtual_ep_dequeue;
	u->ep_ops.set_halt = virtual_ep_set_halt;
	u->ep_ops.set_wedge = virtual_ep_set_wedge;

	mutex_lock(&udc_drv_lock);
	list_for_each_entry(other, &udc_drv_list, list) {
		if (!strcmp(other->name, u->name))
			goto err_exist;
	}

	ret = 0;
	list_add_tail(&u->list, &udc_drv_list);
	mutex_unlock(&udc_drv_lock);

	platform_driver_register(driver);
	return ret;

err_exist:
	mutex_unlock(&udc_drv_lock);
	return ret;
}
EXPORT_SYMBOL(virtual_usb_udc_register);

static struct virtual_usb_udc *alloc_new_udc(struct virtual_usb_udc_driver *drv, int id)
{
	struct virtual_usb_udc *newu;
	int ret;

	newu = kzalloc(sizeof(*newu), GFP_KERNEL);
	if (IS_ERR(newu))
		goto out;

	spin_lock_init(&newu->lock);
	newu->udc_drv = drv;
	/* By default we use high speed device */
	newu->max_speed = USB_SPEED_HIGH;
	newu->udc_dev = platform_device_alloc(drv->name, id);
	newu->id = newu->udc_dev->id;

	ret = platform_device_add_data(newu->udc_dev, &newu, sizeof(void *));
	if (ret) {
		platform_device_put(newu->udc_dev);
		kfree(newu);
		newu = ERR_PTR(ret);
	}

out:
	return newu;
}

static struct virtual_usb_udc *try_get_virtual_udc(const char *driver, int id)
{
	struct virtual_usb_udc_driver *drv;
	struct virtual_usb_udc *newu;
	
	newu = ERR_PTR(-ENOENT);
	mutex_lock(&udc_drv_lock);
	list_for_each_entry(drv, &udc_drv_list, list) {
		if (strcmp(drv->name, driver))
			continue;

		if (!try_module_get(drv->module)) {
			newu = ERR_PTR(-EBUSY);
			break;
		}
		
		newu = alloc_new_udc(drv, id);
		if (IS_ERR(newu))
			module_put(drv->module);			
		
		break;
	}

	mutex_unlock(&udc_drv_lock);

	return newu;
}

struct virtual_usb_udc *virtual_usb_alloc_udc(const char *driver, int id)
{
	struct virtual_usb_udc *newu;
	int ret;

	newu = try_get_virtual_udc(driver, id);

	if (IS_ERR(newu) && PTR_ERR(newu) == -ENOENT) {
		ret = request_module("virtual_usb_udc:%s", driver);

		newu = ret < 0 ? ERR_PTR(ret)
			: try_get_virtual_udc(driver, id);
	}

	return newu;
}
EXPORT_SYMBOL(virtual_usb_alloc_udc);

void virtual_usb_put_udc(struct virtual_usb_udc *u)
{
	struct module *mod;

	if (!u)
		return;

	mod = u->udc_drv->module;
	kfree(u->data);
	kfree(u);
	module_put(mod);
}
EXPORT_SYMBOL(virtual_usb_put_udc);

int virtual_usb_udc_add_data(struct virtual_usb_udc *u, const void *data, size_t size)
{
	void *d = NULL;
	int ret = -ENOMEM;
	
	if (data) {
		d = kmemdup(data, size, GFP_KERNEL);
		if (!d)
			goto out;
	}

	kfree(u->data);
	u->data = d;
	ret = 0;
out:
	return ret;
}
EXPORT_SYMBOL(virtual_usb_udc_add_data);

void virtual_usb_del_udc(struct virtual_usb_udc *u)
{
	if (!u)
		return;

	platform_device_del(u->udc_dev);
}
EXPORT_SYMBOL(virtual_usb_del_udc);

int virtual_usb_add_udc(struct virtual_usb_udc *u)
{
	int ret;

	ret = platform_device_add(u->udc_dev);
	if (ret)
		goto out;

	/* Check if probe() was successful */
	if (!platform_get_drvdata(u->udc_dev)) {
		platform_device_del(u->udc_dev);
		ret = -EINVAL;
	}
out:
	return ret;
}
EXPORT_SYMBOL(virtual_usb_add_udc);

void virtual_usb_rm_udc(struct virtual_usb_udc *u)
{
	virtual_usb_del_udc(u);
	virtual_usb_put_udc(u);
}
EXPORT_SYMBOL(virtual_usb_rm_udc);



