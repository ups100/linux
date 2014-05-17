#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>


#include <linux/usb/dummy_hcd.h>



#define DRIVER_DESC	"USB UDC Emulator"
#define DRIVER_VERSION	"16 May 2014"

#define POWER_BUDGET	500	/* in mA; use 8 for low-power port testing */

static const char	driver_desc[] = DRIVER_DESC;

MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_AUTHOR("David Brownell, Krzysztof Opasiak");
MODULE_LICENSE("GPL");

struct dummy_hcd_module_parameters {
	bool is_super_speed;
	bool is_high_speed;
	unsigned int num;
};

static struct dummy_hcd_module_parameters mod_data = {
	.is_super_speed = false,
	.is_high_speed = true,
	.num = 1,
};
module_param_named(is_super_speed, mod_data.is_super_speed, bool, S_IRUGO);
MODULE_PARM_DESC(is_super_speed, "true to simulate SuperSpeed connection");
module_param_named(is_high_speed, mod_data.is_high_speed, bool, S_IRUGO);
MODULE_PARM_DESC(is_high_speed, "true to simulate HighSpeed connection");
module_param_named(num, mod_data.num, uint, S_IRUGO);
MODULE_PARM_DESC(num, "number of emulated controllers");
/*-------------------------------------------------------------------------*/

/* SLAVE/GADGET SIDE UTILITY ROUTINES */

/* called with spinlock held */
static void nuke(struct dummy *dum, struct dummy_ep *ep)
{
	while (!list_empty(&ep->queue)) {
		struct dummy_request	*req;

		req = list_entry(ep->queue.next, struct dummy_request, queue);
		list_del_init(&req->queue);
		req->req.status = -ESHUTDOWN;

		spin_unlock(&dum->lock);
		req->req.complete(&ep->ep, &req->req);
		spin_lock(&dum->lock);
	}
}

/* caller must hold lock */
static void stop_activity(struct dummy *dum)
{
	struct dummy_ep	*ep;

	/* prevent any more requests */
	dum->address = 0;

	/* The timer is left running so that outstanding URBs can fail */

	/* nuke any pending requests first, so driver i/o is quiesced */
	list_for_each_entry(ep, &dum->gadget.ep_list, ep.ep_list)
		nuke(dum, ep);

	/* driver now does any non-usb quiescing necessary */
}

/**
 * set_link_state_by_speed() - Sets the current state of the link according to
 *	the hcd speed
 * @dum_hcd: pointer to the dummy_hcd structure to update the link state for
 *
 * This function updates the port_status according to the link state and the
 * speed of the hcd.
 */
static void set_link_state_by_speed(struct dummy_hcd *dum_hcd)
{
	struct dummy *dum = dum_hcd->dum;

	if (dummy_hcd_to_hcd(dum_hcd)->speed == HCD_USB3) {
		if ((dum_hcd->port_status & USB_SS_PORT_STAT_POWER) == 0) {
			dum_hcd->port_status = 0;
		} else if (!dum->pullup || dum->udc_suspended) {
			/* UDC suspend must cause a disconnect */
			dum_hcd->port_status &= ~(USB_PORT_STAT_CONNECTION |
						USB_PORT_STAT_ENABLE);
			if ((dum_hcd->old_status &
			     USB_PORT_STAT_CONNECTION) != 0)
				dum_hcd->port_status |=
					(USB_PORT_STAT_C_CONNECTION << 16);
		} else {
			/* device is connected and not suspended */
			dum_hcd->port_status |= (USB_PORT_STAT_CONNECTION |
						 USB_PORT_STAT_SPEED_5GBPS) ;
			if ((dum_hcd->old_status &
			     USB_PORT_STAT_CONNECTION) == 0)
				dum_hcd->port_status |=
					(USB_PORT_STAT_C_CONNECTION << 16);
			if ((dum_hcd->port_status &
			     USB_PORT_STAT_ENABLE) == 1 &&
				(dum_hcd->port_status &
				 USB_SS_PORT_LS_U0) == 1 &&
				dum_hcd->rh_state != DUMMY_RH_SUSPENDED)
				dum_hcd->active = 1;
		}
	} else {
		if ((dum_hcd->port_status & USB_PORT_STAT_POWER) == 0) {
			dum_hcd->port_status = 0;
		} else if (!dum->pullup || dum->udc_suspended) {
			/* UDC suspend must cause a disconnect */
			dum_hcd->port_status &= ~(USB_PORT_STAT_CONNECTION |
						USB_PORT_STAT_ENABLE |
						USB_PORT_STAT_LOW_SPEED |
						USB_PORT_STAT_HIGH_SPEED |
						USB_PORT_STAT_SUSPEND);
			if ((dum_hcd->old_status &
			     USB_PORT_STAT_CONNECTION) != 0)
				dum_hcd->port_status |=
					(USB_PORT_STAT_C_CONNECTION << 16);
		} else {
			dum_hcd->port_status |= USB_PORT_STAT_CONNECTION;
			if ((dum_hcd->old_status &
			     USB_PORT_STAT_CONNECTION) == 0)
				dum_hcd->port_status |=
					(USB_PORT_STAT_C_CONNECTION << 16);
			if ((dum_hcd->port_status & USB_PORT_STAT_ENABLE) == 0)
				dum_hcd->port_status &= ~USB_PORT_STAT_SUSPEND;
			else if ((dum_hcd->port_status &
				  USB_PORT_STAT_SUSPEND) == 0 &&
					dum_hcd->rh_state != DUMMY_RH_SUSPENDED)
				dum_hcd->active = 1;
		}
	}
}

/* caller must hold lock */
void set_link_state(struct dummy_hcd *dum_hcd)
{
	struct dummy *dum = dum_hcd->dum;

	dum_hcd->active = 0;
	if (dum->pullup)
		if ((dummy_hcd_to_hcd(dum_hcd)->speed == HCD_USB3 &&
		     dum->gadget.speed != USB_SPEED_SUPER) ||
		    (dummy_hcd_to_hcd(dum_hcd)->speed != HCD_USB3 &&
		     dum->gadget.speed == USB_SPEED_SUPER))
			return;

	set_link_state_by_speed(dum_hcd);

	if ((dum_hcd->port_status & USB_PORT_STAT_ENABLE) == 0 ||
	     dum_hcd->active)
		dum_hcd->resuming = 0;

	/* if !connected or reset */
	if ((dum_hcd->port_status & USB_PORT_STAT_CONNECTION) == 0 ||
			(dum_hcd->port_status & USB_PORT_STAT_RESET) != 0) {
		/*
		 * We're connected and not reset (reset occurred now),
		 * and driver attached - disconnect!
		 */
		if ((dum_hcd->old_status & USB_PORT_STAT_CONNECTION) != 0 &&
		    (dum_hcd->old_status & USB_PORT_STAT_RESET) == 0 &&
		    dum->driver) {
			stop_activity(dum);
			spin_unlock(&dum->lock);
			dum->driver->disconnect(&dum->gadget);
			spin_lock(&dum->lock);
		}
	} else if (dum_hcd->active != dum_hcd->old_active) {
		if (dum_hcd->old_active && dum->driver->suspend) {
			spin_unlock(&dum->lock);
			dum->driver->suspend(&dum->gadget);
			spin_lock(&dum->lock);
		} else if (!dum_hcd->old_active &&  dum->driver->resume) {
			spin_unlock(&dum->lock);
			dum->driver->resume(&dum->gadget);
			spin_lock(&dum->lock);
		}
	}

	dum_hcd->old_status = dum_hcd->port_status;
	dum_hcd->old_active = dum_hcd->active;
}
EXPORT_SYMBOL(set_link_state);

/*-------------------------------------------------------------------------*/

/* SLAVE/GADGET SIDE DRIVER
 *
 * This only tracks gadget state.  All the work is done when the host
 * side tries some (emulated) i/o operation.  Real device controller
 * drivers would do real i/o using dma, fifos, irqs, timers, etc.
 */

#define is_enabled(dum) \
	(dum->port_status & USB_PORT_STAT_ENABLE)

static int dummy_enable(struct usb_ep *_ep,
		const struct usb_endpoint_descriptor *desc)
{
	struct dummy		*dum;
	struct dummy_hcd	*dum_hcd;
	struct dummy_ep		*ep;
	unsigned		max;
	int			retval;

	ep = usb_ep_to_dummy_ep(_ep);
	if (!_ep || !desc || ep->desc || _ep->name == ep0name
			|| desc->bDescriptorType != USB_DT_ENDPOINT)
		return -EINVAL;
	dum = ep_to_dummy(ep);
	if (!dum->driver)
		return -ESHUTDOWN;

	dum_hcd = gadget_to_dummy_hcd(&dum->gadget);
	if (!is_enabled(dum_hcd))
		return -ESHUTDOWN;

	/*
	 * For HS/FS devices only bits 0..10 of the wMaxPacketSize represent the
	 * maximum packet size.
	 * For SS devices the wMaxPacketSize is limited by 1024.
	 */
	max = usb_endpoint_maxp(desc) & 0x7ff;

	/* drivers must not request bad settings, since lower levels
	 * (hardware or its drivers) may not check.  some endpoints
	 * can't do iso, many have maxpacket limitations, etc.
	 *
	 * since this "hardware" driver is here to help debugging, we
	 * have some extra sanity checks.  (there could be more though,
	 * especially for "ep9out" style fixed function ones.)
	 */
	retval = -EINVAL;
	switch (usb_endpoint_type(desc)) {
	case USB_ENDPOINT_XFER_BULK:
		if (strstr(ep->ep.name, "-iso")
				|| strstr(ep->ep.name, "-int")) {
			goto done;
		}
		switch (dum->gadget.speed) {
		case USB_SPEED_SUPER:
			if (max == 1024)
				break;
			goto done;
		case USB_SPEED_HIGH:
			if (max == 512)
				break;
			goto done;
		case USB_SPEED_FULL:
			if (max == 8 || max == 16 || max == 32 || max == 64)
				/* we'll fake any legal size */
				break;
			/* save a return statement */
		default:
			goto done;
		}
		break;
	case USB_ENDPOINT_XFER_INT:
		if (strstr(ep->ep.name, "-iso")) /* bulk is ok */
			goto done;
		/* real hardware might not handle all packet sizes */
		switch (dum->gadget.speed) {
		case USB_SPEED_SUPER:
		case USB_SPEED_HIGH:
			if (max <= 1024)
				break;
			/* save a return statement */
		case USB_SPEED_FULL:
			if (max <= 64)
				break;
			/* save a return statement */
		default:
			if (max <= 8)
				break;
			goto done;
		}
		break;
	case USB_ENDPOINT_XFER_ISOC:
		if (strstr(ep->ep.name, "-bulk")
				|| strstr(ep->ep.name, "-int"))
			goto done;
		/* real hardware might not handle all packet sizes */
		switch (dum->gadget.speed) {
		case USB_SPEED_SUPER:
		case USB_SPEED_HIGH:
			if (max <= 1024)
				break;
			/* save a return statement */
		case USB_SPEED_FULL:
			if (max <= 1023)
				break;
			/* save a return statement */
		default:
			goto done;
		}
		break;
	default:
		/* few chips support control except on ep0 */
		goto done;
	}

	_ep->maxpacket = max;
	if (usb_ss_max_streams(_ep->comp_desc)) {
		if (!usb_endpoint_xfer_bulk(desc)) {
			dev_err(udc_dev(dum), "Can't enable stream support on "
					"non-bulk ep %s\n", _ep->name);
			return -EINVAL;
		}
		ep->stream_en = 1;
	}
	ep->desc = desc;

	dev_dbg(udc_dev(dum), "enabled %s (ep%d%s-%s) maxpacket %d stream %s\n",
		_ep->name,
		desc->bEndpointAddress & 0x0f,
		(desc->bEndpointAddress & USB_DIR_IN) ? "in" : "out",
		({ char *val;
		 switch (usb_endpoint_type(desc)) {
		 case USB_ENDPOINT_XFER_BULK:
			 val = "bulk";
			 break;
		 case USB_ENDPOINT_XFER_ISOC:
			 val = "iso";
			 break;
		 case USB_ENDPOINT_XFER_INT:
			 val = "intr";
			 break;
		 default:
			 val = "ctrl";
			 break;
		 } val; }),
		max, ep->stream_en ? "enabled" : "disabled");

	/* at this point real hardware should be NAKing transfers
	 * to that endpoint, until a buffer is queued to it.
	 */
	ep->halted = ep->wedged = 0;
	retval = 0;
done:
	return retval;
}

static int dummy_disable(struct usb_ep *_ep)
{
	struct dummy_ep		*ep;
	struct dummy		*dum;
	unsigned long		flags;
	int			retval;

	ep = usb_ep_to_dummy_ep(_ep);
	if (!_ep || !ep->desc || _ep->name == ep0name)
		return -EINVAL;
	dum = ep_to_dummy(ep);

	spin_lock_irqsave(&dum->lock, flags);
	ep->desc = NULL;
	ep->stream_en = 0;
	retval = 0;
	nuke(dum, ep);
	spin_unlock_irqrestore(&dum->lock, flags);

	dev_dbg(udc_dev(dum), "disabled %s\n", _ep->name);
	return retval;
}

static struct usb_request *dummy_alloc_request(struct usb_ep *_ep,
		gfp_t mem_flags)
{
	struct dummy_ep		*ep;
	struct dummy_request	*req;

	if (!_ep)
		return NULL;
	ep = usb_ep_to_dummy_ep(_ep);

	req = kzalloc(sizeof(*req), mem_flags);
	if (!req)
		return NULL;
	INIT_LIST_HEAD(&req->queue);
	return &req->req;
}

static void dummy_free_request(struct usb_ep *_ep, struct usb_request *_req)
{
	struct dummy_request	*req;

	if (!_ep || !_req) {
		WARN_ON(1);
		return;
	}

	req = usb_request_to_dummy_request(_req);
	WARN_ON(!list_empty(&req->queue));
	kfree(req);
}

static void fifo_complete(struct usb_ep *ep, struct usb_request *req)
{
}

static int dummy_queue(struct usb_ep *_ep, struct usb_request *_req,
		gfp_t mem_flags)
{
	struct dummy_ep		*ep;
	struct dummy_request	*req;
	struct dummy		*dum;
	struct dummy_hcd	*dum_hcd;
	unsigned long		flags;

	req = usb_request_to_dummy_request(_req);
	if (!_req || !list_empty(&req->queue) || !_req->complete)
		return -EINVAL;

	ep = usb_ep_to_dummy_ep(_ep);
	if (!_ep || (!ep->desc && _ep->name != ep0name))
		return -EINVAL;

	dum = ep_to_dummy(ep);
	dum_hcd = gadget_to_dummy_hcd(&dum->gadget);
	if (!dum->driver || !is_enabled(dum_hcd))
		return -ESHUTDOWN;

#if 0
	dev_dbg(udc_dev(dum), "ep %p queue req %p to %s, len %d buf %p\n",
			ep, _req, _ep->name, _req->length, _req->buf);
#endif
	_req->status = -EINPROGRESS;
	_req->actual = 0;
	spin_lock_irqsave(&dum->lock, flags);

	/* implement an emulated single-request FIFO */
	if (ep->desc && (ep->desc->bEndpointAddress & USB_DIR_IN) &&
			list_empty(&dum->fifo_req.queue) &&
			list_empty(&ep->queue) &&
			_req->length <= FIFO_SIZE) {
		req = &dum->fifo_req;
		req->req = *_req;
		req->req.buf = dum->fifo_buf;
		memcpy(dum->fifo_buf, _req->buf, _req->length);
		req->req.context = dum;
		req->req.complete = fifo_complete;

		list_add_tail(&req->queue, &ep->queue);
		spin_unlock(&dum->lock);
		_req->actual = _req->length;
		_req->status = 0;
		_req->complete(_ep, _req);
		spin_lock(&dum->lock);
	}  else
		list_add_tail(&req->queue, &ep->queue);
	spin_unlock_irqrestore(&dum->lock, flags);

	/* real hardware would likely enable transfers here, in case
	 * it'd been left NAKing.
	 */
	return 0;
}

static int dummy_dequeue(struct usb_ep *_ep, struct usb_request *_req)
{
	struct dummy_ep		*ep;
	struct dummy		*dum;
	int			retval = -EINVAL;
	unsigned long		flags;
	struct dummy_request	*req = NULL;

	if (!_ep || !_req)
		return retval;
	ep = usb_ep_to_dummy_ep(_ep);
	dum = ep_to_dummy(ep);

	if (!dum->driver)
		return -ESHUTDOWN;

	local_irq_save(flags);
	spin_lock(&dum->lock);
	list_for_each_entry(req, &ep->queue, queue) {
		if (&req->req == _req) {
			list_del_init(&req->queue);
			_req->status = -ECONNRESET;
			retval = 0;
			break;
		}
	}
	spin_unlock(&dum->lock);

	if (retval == 0) {
		dev_dbg(udc_dev(dum),
				"dequeued req %p from %s, len %d buf %p\n",
				req, _ep->name, _req->length, _req->buf);
		_req->complete(_ep, _req);
	}
	local_irq_restore(flags);
	return retval;
}

static int
dummy_set_halt_and_wedge(struct usb_ep *_ep, int value, int wedged)
{
	struct dummy_ep		*ep;
	struct dummy		*dum;

	if (!_ep)
		return -EINVAL;
	ep = usb_ep_to_dummy_ep(_ep);
	dum = ep_to_dummy(ep);
	if (!dum->driver)
		return -ESHUTDOWN;
	if (!value)
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

static int
dummy_set_halt(struct usb_ep *_ep, int value)
{
	return dummy_set_halt_and_wedge(_ep, value, 0);
}

static int dummy_set_wedge(struct usb_ep *_ep)
{
	if (!_ep || _ep->name == ep0name)
		return -EINVAL;
	return dummy_set_halt_and_wedge(_ep, 1, 1);
}

static const struct usb_ep_ops dummy_ep_ops = {
	.enable		= dummy_enable,
	.disable	= dummy_disable,

	.alloc_request	= dummy_alloc_request,
	.free_request	= dummy_free_request,

	.queue		= dummy_queue,
	.dequeue	= dummy_dequeue,

	.set_halt	= dummy_set_halt,
	.set_wedge	= dummy_set_wedge,
};

/*-------------------------------------------------------------------------*/

/* there are both host and device side versions of this call ... */
int dummy_g_get_frame(struct usb_gadget *_gadget)
{
	struct timeval	tv;

	do_gettimeofday(&tv);
	return tv.tv_usec / 1000;
}
EXPORT_SYMBOL(dummy_g_get_frame);

static int dummy_wakeup(struct usb_gadget *_gadget)
{
	struct dummy_hcd *dum_hcd;

	dum_hcd = gadget_to_dummy_hcd(_gadget);
	if (!(dum_hcd->dum->devstatus & ((1 << USB_DEVICE_B_HNP_ENABLE)
				| (1 << USB_DEVICE_REMOTE_WAKEUP))))
		return -EINVAL;
	if ((dum_hcd->port_status & USB_PORT_STAT_CONNECTION) == 0)
		return -ENOLINK;
	if ((dum_hcd->port_status & USB_PORT_STAT_SUSPEND) == 0 &&
			 dum_hcd->rh_state != DUMMY_RH_SUSPENDED)
		return -EIO;

	/* FIXME: What if the root hub is suspended but the port isn't? */

	/* hub notices our request, issues downstream resume, etc */
	dum_hcd->resuming = 1;
	dum_hcd->re_timeout = jiffies + msecs_to_jiffies(20);
	mod_timer(&dummy_hcd_to_hcd(dum_hcd)->rh_timer, dum_hcd->re_timeout);
	return 0;
}

static int dummy_set_selfpowered(struct usb_gadget *_gadget, int value)
{
	struct dummy	*dum;

	dum = gadget_to_dummy_hcd(_gadget)->dum;
	if (value)
		dum->devstatus |= (1 << USB_DEVICE_SELF_POWERED);
	else
		dum->devstatus &= ~(1 << USB_DEVICE_SELF_POWERED);
	return 0;
}

static void dummy_udc_update_ep0(struct dummy *dum)
{
	if (dum->gadget.speed == USB_SPEED_SUPER)
		dum->ep[0].ep.maxpacket = 9;
	else
		dum->ep[0].ep.maxpacket = 64;
}

static int dummy_pullup(struct usb_gadget *_gadget, int value)
{
	struct dummy_hcd *dum_hcd;
	struct dummy	*dum;
	unsigned long	flags;

	dum = gadget_dev_to_dummy(&_gadget->dev);

	if (value && dum->driver) {
		if (mod_data.is_super_speed)
			dum->gadget.speed = dum->driver->max_speed;
		else if (mod_data.is_high_speed)
			dum->gadget.speed = min_t(u8, USB_SPEED_HIGH,
					dum->driver->max_speed);
		else
			dum->gadget.speed = USB_SPEED_FULL;
		dummy_udc_update_ep0(dum);

		if (dum->gadget.speed < dum->driver->max_speed)
			dev_dbg(udc_dev(dum), "This device can perform faster"
				" if you connect it to a %s port...\n",
				usb_speed_string(dum->driver->max_speed));
	}
	dum_hcd = gadget_to_dummy_hcd(_gadget);

	spin_lock_irqsave(&dum->lock, flags);
	dum->pullup = (value != 0);
	set_link_state(dum_hcd);
	spin_unlock_irqrestore(&dum->lock, flags);

	usb_hcd_poll_rh_status(dummy_hcd_to_hcd(dum_hcd));
	return 0;
}

static int dummy_udc_start(struct usb_gadget *g,
		struct usb_gadget_driver *driver);
static int dummy_udc_stop(struct usb_gadget *g,
		struct usb_gadget_driver *driver);

static const struct usb_gadget_ops dummy_ops = {
	.get_frame	= dummy_g_get_frame,
	.wakeup		= dummy_wakeup,
	.set_selfpowered = dummy_set_selfpowered,
	.pullup		= dummy_pullup,
	.udc_start	= dummy_udc_start,
	.udc_stop	= dummy_udc_stop,
};

/*-------------------------------------------------------------------------*/

/* "function" sysfs attribute */
static ssize_t function_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	struct dummy	*dum = gadget_dev_to_dummy(dev);

	if (!dum->driver || !dum->driver->function)
		return 0;
	return scnprintf(buf, PAGE_SIZE, "%s\n", dum->driver->function);
}
static DEVICE_ATTR_RO(function);

/*-------------------------------------------------------------------------*/

/*
 * Driver registration/unregistration.
 *
 * This is basically hardware-specific; there's usually only one real USB
 * device (not host) controller since that's how USB devices are intended
 * to work.  So most implementations of these api calls will rely on the
 * fact that only one driver will ever bind to the hardware.  But curious
 * hardware can be built with discrete components, so the gadget API doesn't
 * require that assumption.
 *
 * For this emulator, it might be convenient to create a usb slave device
 * for each driver that registers:  just add to a big root hub.
 */

static int dummy_udc_start(struct usb_gadget *g,
		struct usb_gadget_driver *driver)
{
	struct dummy_hcd	*dum_hcd = gadget_to_dummy_hcd(g);
	struct dummy		*dum = dum_hcd->dum;

	if (driver->max_speed == USB_SPEED_UNKNOWN)
		return -EINVAL;

	/*
	 * SLAVE side init ... the layer above hardware, which
	 * can't enumerate without help from the driver we're binding.
	 */

	dum->devstatus = 0;

	dum->driver = driver;
	dev_dbg(udc_dev(dum), "binding gadget driver '%s'\n",
			driver->driver.name);
	return 0;
}

static int dummy_udc_stop(struct usb_gadget *g,
		struct usb_gadget_driver *driver)
{
	struct dummy_hcd	*dum_hcd = gadget_to_dummy_hcd(g);
	struct dummy		*dum = dum_hcd->dum;

	if (driver)
		dev_dbg(udc_dev(dum), "unregister gadget driver '%s'\n",
				driver->driver.name);

	dum->driver = NULL;

	return 0;
}

#undef is_enabled

/* The gadget structure is stored inside the hcd structure and will be
 * released along with it. */
static void init_dummy_udc_hw(struct dummy *dum)
{
	int i;

	INIT_LIST_HEAD(&dum->gadget.ep_list);
	for (i = 0; i < DUMMY_ENDPOINTS; i++) {
		struct dummy_ep	*ep = &dum->ep[i];

		if (!ep_name[i])
			break;
		ep->ep.name = ep_name[i];
		ep->ep.ops = &dummy_ep_ops;
		list_add_tail(&ep->ep.ep_list, &dum->gadget.ep_list);
		ep->halted = ep->wedged = ep->already_seen =
				ep->setup_stage = 0;
		usb_ep_set_maxpacket_limit(&ep->ep, ~0);
		ep->ep.max_streams = 16;
		ep->last_io = jiffies;
		ep->gadget = &dum->gadget;
		ep->desc = NULL;
		INIT_LIST_HEAD(&ep->queue);
	}

	dum->gadget.ep0 = &dum->ep[0].ep;
	list_del_init(&dum->ep[0].ep.ep_list);
	INIT_LIST_HEAD(&dum->fifo_req.queue);

#ifdef CONFIG_USB_OTG
	dum->gadget.is_otg = 1;
#endif
}

static int dummy_udc_probe(struct platform_device *pdev)
{
	struct dummy	*dum;
	int		rc;

	dum = *((void **)dev_get_platdata(&pdev->dev));
	dum->gadget.name = gadget_name;
	dum->gadget.ops = &dummy_ops;
	dum->gadget.max_speed = USB_SPEED_SUPER;

	dum->gadget.dev.parent = &pdev->dev;
	init_dummy_udc_hw(dum);

	rc = usb_add_gadget_udc(&pdev->dev, &dum->gadget);
	if (rc < 0)
		goto err_udc;

	rc = device_create_file(&dum->gadget.dev, &dev_attr_function);
	if (rc < 0)
		goto err_dev;
	platform_set_drvdata(pdev, dum);
	return rc;

err_dev:
	usb_del_gadget_udc(&dum->gadget);
err_udc:
	return rc;
}

static int dummy_udc_remove(struct platform_device *pdev)
{
	struct dummy	*dum = platform_get_drvdata(pdev);

	device_remove_file(&dum->gadget.dev, &dev_attr_function);
	usb_del_gadget_udc(&dum->gadget);
	return 0;
}

static void dummy_udc_pm(struct dummy *dum, struct dummy_hcd *dum_hcd,
		int suspend)
{
	spin_lock_irq(&dum->lock);
	dum->udc_suspended = suspend;
	set_link_state(dum_hcd);
	spin_unlock_irq(&dum->lock);
}

static int dummy_udc_suspend(struct platform_device *pdev, pm_message_t state)
{
	struct dummy		*dum = platform_get_drvdata(pdev);
	struct dummy_hcd	*dum_hcd = gadget_to_dummy_hcd(&dum->gadget);

	dev_dbg(&pdev->dev, "%s\n", __func__);
	dummy_udc_pm(dum, dum_hcd, 1);
	usb_hcd_poll_rh_status(dummy_hcd_to_hcd(dum_hcd));
	return 0;
}

static int dummy_udc_resume(struct platform_device *pdev)
{
	struct dummy		*dum = platform_get_drvdata(pdev);
	struct dummy_hcd	*dum_hcd = gadget_to_dummy_hcd(&dum->gadget);

	dev_dbg(&pdev->dev, "%s\n", __func__);
	dummy_udc_pm(dum, dum_hcd, 0);
	usb_hcd_poll_rh_status(dummy_hcd_to_hcd(dum_hcd));
	return 0;
}

static struct platform_driver dummy_udc_driver = {
	.probe		= dummy_udc_probe,
	.remove		= dummy_udc_remove,
	.suspend	= dummy_udc_suspend,
	.resume		= dummy_udc_resume,
	.driver		= {
		.name	= (char *) gadget_name,
		.owner	= THIS_MODULE,
	},
};

/*-------------------------------------------------------------------------*/

static int __init init(void)
{
	int ret = -ENODEV;

	if (usb_disabled())
		return ret;

	if (!mod_data.is_high_speed && mod_data.is_super_speed)
		return -EINVAL;

	ret = platform_driver_register(&dummy_udc_driver);

	return ret;
}
module_init(init);

static void __exit cleanup(void)
{
	platform_driver_unregister(&dummy_udc_driver);
}
module_exit(cleanup);
