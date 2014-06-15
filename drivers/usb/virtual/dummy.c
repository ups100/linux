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
	struct timeval	tv;

	do_gettimeofday(&tv);
	return tv.tv_usec / 1000;
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

	printk("priv data size: %d size: %d\n", drv->ep_priv_data_size, size);
 
	ep = kcalloc(drv->ep_nmb, size, GFP_KERNEL);
	if (!ep)
		goto out;

	udc->ep = ep;

	for (i = 0; i < drv->ep_nmb && drv->ep_name[i]; ++i) {
		printk("loop begin %i ep %p\n", i, ep);
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
	printk("Po forze\n");
	udc->gadget.ep0 = &((struct virtual_usb_ep *)udc->ep)->ep;
	list_del_init(&udc->gadget.ep0->ep_list);
	INIT_LIST_HEAD(&udc->fifo_req.queue);

#ifdef CONFIG_USB_OTG
	udc->gadget.is_otg = 1;
#endif
	ret = 0;
	printk("przed retem\n");
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
	printk("po udc\n");
	drv = udc->udc_drv;
	printk("po drv\n");

	gadget = &udc->gadget;
	printk("po gadget\n");
	if (!gadget->name)
		gadget->name = drv->name;

	printk("po ifie\n");
	gadget->max_speed = udc->max_speed;
	gadget->dev.parent = &pdev->dev;
	gadget->ops = &drv->g_ops;

	printk("po opsach\n");
	/* init our virtual hardware at this point */
	ret = init_virtual_udc_hw(udc);
	if (ret)
		goto out;

	printk("po init\n");
	ret = drv->probe(udc);
	if (ret)
		goto cleanup;

	printk("po probe\n");
	ret = usb_add_gadget_udc(&pdev->dev, gadget);
	if (ret < 0)
		goto err_remove;
	
	printk("po add\n");
	platform_set_drvdata(pdev, udc);
	
	printk("return\n");
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
#define EMPTY_FUNC_RET_0(HW_TYPE, NAME, ...)			\
	static int virtual_usb_##HW_TYPE##_empty_##NAME( __VA_ARGS__ ) { return 0;}

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

	mutex_lock(&udc_drv_lock);
	list_for_each_entry(other, &udc_drv_list, list) {
		if (!strcmp(other->name, u->name))
			goto err_exist;
	}

	ret = 0;
	list_add_tail(&u->list, &udc_drv_list);
	mutex_unlock(&udc_drv_lock);

#define CHECK_FUNC_AND_SET_IF_NULL(hw, ptr, field)	\
	if (!ptr->field) { \
		ptr->field = virtual_usb_##hw##_empty_##field; \
	}

	/* first level */
	CHECK_FUNC_AND_SET_IF_NULL(udc, u, probe);
	CHECK_FUNC_AND_SET_IF_NULL(udc, u, remove);
	CHECK_FUNC_AND_SET_IF_NULL(udc, u, suspend);
	CHECK_FUNC_AND_SET_IF_NULL(udc, u, resume);
	CHECK_FUNC_AND_SET_IF_NULL(udc, u, init_ep);

#undef CHECK_FUNC_AND_SET_IF_NULL

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



