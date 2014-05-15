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

struct virtual_usb_hcd_driver {
	const char *name;
	struct module *module;
	struct list_head list;


	struct hc_driver virtual_hcd;
	void (*shutdown)(struct platform_device *);
	int (*suspend)(struct platform_device *, pm_message_t state);
	int (*resume)(struct platform_device *);
};

struct virtual_usb_hcd {
	const char *name;
	struct virtual_usb_hcd_driver *hcd_drv;
	struct list_head list;

	enum usb_device_speed speed;
	struct platform_device *hcd_dev;
};

struct virtual_usb_udc;

/*
name - name of device driver and will be most part of devices name
 */
struct virtual_usb_udc_driver {
	const char *name;
	struct module *module;
	struct list_head list;

	int (*probe)(struct virtual_usb_udc *);
	int (*remove)(struct virtual_usb_udc *);

	/* Don't touch probe and remove and name */
	struct platform_driver driver;
};

/*
name of this hub (alias only, real name depends on driver name)
 */
struct virtual_usb_udc {
	
	int id;
	struct virtual_usb_udc_driver *udc_drv;

	enum usb_device_speed speed;
	struct platform_device *udc_dev;
	void *data;
};


void virtual_usb_hcd_unregister(struct virtual_usb_hcd_driver *h);
int virtual_usb_hcd_register(struct virtual_usb_hcd_driver *h);

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
