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

/*-------------------------------------------------------------------------*/

/*
 * Every device has ep0 for control requests, plus up to 30 more endpoints,
 * in one of two types:
 *
 *   - Configurable:  direction (in/out), type (bulk, iso, etc), and endpoint
 *     number can be changed.  Names like "ep-a" are used for this type.
 *
 *   - Fixed Function:  in other cases.  some characteristics may be mutable;
 *     that'd be hardware-specific.  Names like "ep12out-bulk" are used.
 *
 * Gadget drivers are responsible for not setting up conflicting endpoint
 * configurations, illegal or unsupported packet lengths, and so on.
 */

const char ep0name[] = "ep0";

const char *const ep_name[] = {
	ep0name,				/* everyone has ep0 */

	/* act like a pxa250: fifteen fixed function endpoints */
	"ep1in-bulk", "ep2out-bulk", "ep3in-iso", "ep4out-iso", "ep5in-int",
	"ep6in-bulk", "ep7out-bulk", "ep8in-iso", "ep9out-iso", "ep10in-int",
	"ep11in-bulk", "ep12out-bulk", "ep13in-iso", "ep14out-iso",
		"ep15in-int",

	/* or like sa1100: two fixed function endpoints */
	"ep1out-bulk", "ep2in-bulk",

	/* and now some generic EPs so we have enough in multi config */
	"ep3out", "ep4in", "ep5out", "ep6out", "ep7in", "ep8out", "ep9in",
	"ep10out", "ep11out", "ep12in", "ep13out", "ep14in", "ep15out",
};



/*------------------------------------------------------------------------*/
static LIST_HEAD(hcd_drv_list);
static DEFINE_MUTEX(hcd_drv_lock);

static LIST_HEAD(udc_drv_list);
static DEFINE_MUTEX(udc_drv_lock);

/* Virtual HCD related functions */

void virtual_usb_hcd_unregister(struct virtual_usb_hcd_driver *h)
{
	mutex_lock(&hcd_drv_lock);
	list_del(&h->list);
	mutex_unlock(&hcd_drv_lock);
}
EXPORT_SYMBOL(virtual_usb_hcd_unregister);

int virtual_usb_hcd_register(struct virtual_usb_hcd_driver *h)
{
	struct virtual_usb_hcd_driver *other;
	int ret = -EEXIST;

	mutex_lock(&hcd_drv_lock);
	list_for_each_entry(other, &hcd_drv_list, list) {
		if (!strcmp(other->name, h->name))
			goto out;
	}

	ret = 0;
	list_add_tail(&h->list, &hcd_drv_list);
out:
	mutex_unlock(&hcd_drv_lock);
	return ret;
}
EXPORT_SYMBOL(virtual_usb_hcd_register);

/* Virtual UDC related functions */

void virtual_usb_udc_unregister(struct virtual_usb_udc_driver *u)
{
	platform_driver_unregister(&u->driver);

	mutex_lock(&udc_drv_lock);
	list_del(&u->list);
	mutex_unlock(&udc_drv_lock);
}
EXPORT_SYMBOL(virtual_usb_udc_unregister);

static int virtual_usb_udc_probe(struct platform_device *pdev)
{
	struct virtual_usb_udc *udc;

	udc = *((void **)dev_get_platdata(&pdev->dev));
	return udc->probe(udc);
}

static int virtual_usb_udc_remove(struct platform_device *pdev)
{
	struct virtual_usb_udc *udc;

	udc = *((void **)dev_get_platdata(&pdev->dev));
	return udc->remove(udc);
}

int virtual_usb_udc_register(struct virtual_usb_udc_driver *u)
{
	struct virtual_usb_udc_driver *other;
	int ret = -EEXIST;

	mutex_lock(&udc_drv_lock);
	list_for_each_entry(other, &udc_drv_list, list) {
		if (!strcmp(other->name, u->name))
			goto err_exist;
	}

	ret = 0;
	list_add_tail(&u->list, &udc_drv_list);
	mutex_unlock(&udc_drv_lock);

	u->driver.probe = virtual_usb_udc_probe;
	u->driver.remove = virtual_usb_udc_remove;
	u->driver.driver.name = u->name;
	u->driver.driver.owner = u->module;

	platform_driver_register(&u->driver);
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

	newu->udc_drv = drv;
	newu->speed = USB_SPEED_HIGH;
	newu->udc_dev = platform_device_alloc(drv->name, id);

	ret = platform_device_add_data(newu->udc_dev, newu, sizeof(void *));
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
	
out:
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



