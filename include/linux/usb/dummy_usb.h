/*
 * dummy_usb.h -- framework for virtual usb framework (hub and udc)
 *
 * Copyright (C) 2014 Krzysztof Opasiak
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifndef __LINUX_USB_DUMMY_USB_H
#define __LINUX_USB_DUMMY_USB_H

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/list.h>

#include <linux/platform_device.h>
#include <linux/usb.h>
#include <linux/usb/hcd.h>
#include <linux/usb/gadget.h>

/* to mchange this structure you should own both locks */
struct virtual_usb_link {
	spinlock_t lock;
	enum usb_device_speed speed;
	struct virtual_usb_udc *device;
	struct virtual_usb_hcd *host;
};

/* Root hub states */
enum virtual_rh_state {
	VIRTUAL_RH_RESET,
	VIRTUAL_RH_SUSPENDED,
	VIRTUAL_RH_RUNNING
};

struct virtual_usb_hcd;

struct virtual_usb_hcd_instance {
	struct virtual_usb_hcd *parent;
	enum virtual_rh_state rh_state;
	struct timer_list timer;
	u32 port_status;
	u32 old_status;
	unsigned long re_timeout;

	struct usb_device *udev;
	struct list_head urbp_list;
	u32 stream_en_ep;
	u8 num_stream[30/2];
	unsigned active:1;
	unsigned old_active:1;
	unsigned resuming:1;
};

struct virtual_usb_hcd_driver {
	const char *name;
	const char *description;
	struct module *module;
	struct list_head list;

	int (*probe)(struct virtual_usb_hcd *);
	int (*remove)(struct virtual_usb_hcd *);
	int (*suspend)(struct virtual_usb_hcd *, pm_message_t);
	int (*resume)(struct virtual_usb_hcd *);
	int (*connect)(struct virtual_usb_udc *, struct virtual_usb_hcd *);
	struct platform_driver driver;

	struct hc_driver hcd_drv;
};

struct virtual_usb_hcd {
	spinlock_t lock;	
	int id;
	struct virtual_usb_hcd_driver *hcd_drv;
	struct list_head list;

	enum usb_device_speed max_speed;
	int power_budget;
	struct platform_device *hcd_dev;
	void *data;

	struct virtual_usb_hcd_instance *hs_hcd;
	struct virtual_usb_hcd_instance *ss_hcd;


	/* connection between this hub and some udc */
	struct virtual_usb_link *link;

};

struct virtual_usb_udc;

struct virtual_usb_ep {
	struct usb_ep ep;
	struct usb_gadget *gadget;
	const struct usb_endpoint_descriptor *desc;

	struct list_head queue;
	unsigned long last_io; /* jiffies timestamp */
	unsigned halted:1;
	unsigned wedged:1;
	unsigned already_seen:1;
	unsigned setup_stage:1;
	unsigned stream_en:1;
};

struct virtual_usb_request {
	struct usb_request req;
	struct list_head queue;
};

/*
name - name of device driver and will be most part of devices name
 */
struct virtual_usb_udc_driver {
	const char *name;
	struct module *module;
	struct list_head list;

	int (*probe)(struct virtual_usb_udc *);
	int (*remove)(struct virtual_usb_udc *);
	int (*suspend)(struct virtual_usb_udc *, pm_message_t);
	int (*resume)(struct virtual_usb_udc *);
	int (*connect)(struct virtual_usb_udc *, struct virtual_usb_hcd *);
	/* Don't touch probe and remove and name */
	struct platform_driver driver;
	struct usb_gadget_ops g_ops;
	/*struct usb_gadget_ops real_g_ops;*/
	struct usb_ep_ops ep_ops;
	/*struct usb_ep_ops real_ep_ops;*/

	/* Null terminated array of names for endpoints.
	   Name at 0 index is taken as ep0 */
	const char **ep_name;
	/* Number of endpoints including ep0 */
	int ep_nmb;
	/* Size of private data which each ep holds for this driver */
	int ep_priv_data_size;
	/* Function that is called to allow driver to initialize its private
	   data per each endpoint */
	int (*init_ep)(struct virtual_usb_ep *);
};


/*
name of this hub (alias only, real name depends on driver name)
 */
struct virtual_usb_udc {
	spinlock_t lock;
	int id;
	struct virtual_usb_udc_driver *udc_drv;
	/* TODO add initialization */
	struct list_head list;
	enum usb_device_speed max_speed;

	struct platform_device *udc_dev;
	void *data;

	/* now meat is comming */
	struct usb_gadget gadget;
	struct usb_gadget_driver *driver;
	/* Array of endpoints, each entry is a virtual_usb_ep
	   + size of additional data from driver */
	void *ep;
	/* FIFO queue of usb requests */
	struct virtual_usb_request fifo_req;

	unsigned suspended:1;
	unsigned pullup:1;
	u16 devstatus;

	/* connection between this udc and some hub */
	struct virtual_usb_link *link;
};


void virtual_usb_hcd_unregister(struct virtual_usb_hcd_driver *h);
int virtual_usb_hcd_register(struct virtual_usb_hcd_driver *h);

/* Alloc and put hcd */
struct virtual_usb_hcd *virtual_usb_alloc_hcd(const char *driver, int id);
void virtual_usb_put_hcd(struct virtual_usb_hcd *h);

/* Add copy of data to store */
int virtual_usb_hcd_add_data(struct virtual_usb_hcd *h, const void *data, size_t size);

/* Create new device or remove existing */
int virtual_usb_add_hcd(struct virtual_usb_hcd *h);
void virtual_usb_rm_hcd(struct virtual_usb_hcd *h);

/* In error cases */
void virtual_usb_del_hcd(struct virtual_usb_hcd *h);

/* Register/unregister driver */
void virtual_usb_udc_unregister(struct virtual_usb_udc_driver *u);
int virtual_usb_udc_register(struct virtual_usb_udc_driver *u);

/* Alloc and put udc */
struct virtual_usb_udc *virtual_usb_alloc_udc(const char *driver, int id);
void virtual_usb_put_udc(struct virtual_usb_udc *u);

/* Add copy of data to store */
int virtual_usb_udc_add_data(struct virtual_usb_udc *u, const void *data, size_t size);

/* Create new device or remove existing */
int virtual_usb_add_udc(struct virtual_usb_udc *u);
void virtual_usb_rm_udc(struct virtual_usb_udc *u);

/* In error cases */
void virtual_usb_del_udc(struct virtual_usb_udc *u);

#endif  /* __LINUX_USB_DUMMY_USB_H */
