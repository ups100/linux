#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>

#include <linux/usb/dummy_hcd.h>

#define DRIVER_DESC	"USB HCD Emulator"
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

static unsigned int dummy_get_ep_idx(const struct usb_endpoint_descriptor *desc)
{
	unsigned int index;

	index = usb_endpoint_num(desc) << 1;
	if (usb_endpoint_dir_in(desc))
		index |= 1;
	return index;
}

/* MASTER/HOST SIDE DRIVER
 *
 * this uses the hcd framework to hook up to host side drivers.
 * its root hub will only have one device, otherwise it acts like
 * a normal host controller.
 *
 * when urbs are queued, they're just stuck on a list that we
 * scan in a timer callback.  that callback connects writes from
 * the host with reads from the device, and so on, based on the
 * usb 2.0 rules.
 */

static int dummy_ep_stream_en(struct dummy_hcd *dum_hcd, struct urb *urb)
{
	const struct usb_endpoint_descriptor *desc = &urb->ep->desc;
	u32 index;

	if (!usb_endpoint_xfer_bulk(desc))
		return 0;

	index = dummy_get_ep_idx(desc);
	return (1 << index) & dum_hcd->stream_en_ep;
}

/*
 * The max stream number is saved as a nibble so for the 30 possible endpoints
 * we only 15 bytes of memory. Therefore we are limited to max 16 streams (0
 * means we use only 1 stream). The maximum according to the spec is 16bit so
 * if the 16 stream limit is about to go, the array size should be incremented
 * to 30 elements of type u16.
 */
static int get_max_streams_for_pipe(struct dummy_hcd *dum_hcd,
		unsigned int pipe)
{
	int max_streams;

	max_streams = dum_hcd->num_stream[usb_pipeendpoint(pipe)];
	if (usb_pipeout(pipe))
		max_streams >>= 4;
	else
		max_streams &= 0xf;
	max_streams++;
	return max_streams;
}

static void set_max_streams_for_pipe(struct dummy_hcd *dum_hcd,
		unsigned int pipe, unsigned int streams)
{
	int max_streams;

	streams--;
	max_streams = dum_hcd->num_stream[usb_pipeendpoint(pipe)];
	if (usb_pipeout(pipe)) {
		streams <<= 4;
		max_streams &= 0xf;
	} else {
		max_streams &= 0xf0;
	}
	max_streams |= streams;
	dum_hcd->num_stream[usb_pipeendpoint(pipe)] = max_streams;
}

static int dummy_validate_stream(struct dummy_hcd *dum_hcd, struct urb *urb)
{
	unsigned int max_streams;
	int enabled;

	enabled = dummy_ep_stream_en(dum_hcd, urb);
	if (!urb->stream_id) {
		if (enabled)
			return -EINVAL;
		return 0;
	}
	if (!enabled)
		return -EINVAL;

	max_streams = get_max_streams_for_pipe(dum_hcd,
			usb_pipeendpoint(urb->pipe));
	if (urb->stream_id > max_streams) {
		dev_err(dummy_dev(dum_hcd), "Stream id %d is out of range.\n",
				urb->stream_id);
		BUG();
		return -EINVAL;
	}
	return 0;
}

static int dummy_urb_enqueue(
	struct usb_hcd			*hcd,
	struct urb			*urb,
	gfp_t				mem_flags
) {
	struct dummy_hcd *dum_hcd;
	struct urbp	*urbp;
	unsigned long	flags;
	int		rc;

	urbp = kmalloc(sizeof *urbp, mem_flags);
	if (!urbp)
		return -ENOMEM;
	urbp->urb = urb;
	urbp->miter_started = 0;

	dum_hcd = hcd_to_dummy_hcd(hcd);
	spin_lock_irqsave(&dum_hcd->link->lock, flags);

	rc = dummy_validate_stream(dum_hcd, urb);
	if (rc) {
		kfree(urbp);
		goto done;
	}

	rc = usb_hcd_link_urb_to_ep(hcd, urb);
	if (rc) {
		kfree(urbp);
		goto done;
	}

	if (!dum_hcd->udev) {
		dum_hcd->udev = urb->dev;
		usb_get_dev(dum_hcd->udev);
	} else if (unlikely(dum_hcd->udev != urb->dev))
		dev_err(dummy_dev(dum_hcd), "usb_device address has changed!\n");

	list_add_tail(&urbp->urbp_list, &dum_hcd->urbp_list);
	urb->hcpriv = urbp;
	if (usb_pipetype(urb->pipe) == PIPE_CONTROL)
		urb->error_count = 1;		/* mark as a new urb */

	/* kick the scheduler, it'll do the rest */
	if (!timer_pending(&dum_hcd->timer))
		mod_timer(&dum_hcd->timer, jiffies + 1);

 done:
	spin_unlock_irqrestore(&dum_hcd->link->lock, flags);
	return rc;
}

static int dummy_urb_dequeue(struct usb_hcd *hcd, struct urb *urb, int status)
{
	struct dummy_hcd *dum_hcd;
	unsigned long	flags;
	int		rc;

	/* giveback happens automatically in timer callback,
	 * so make sure the callback happens */
	dum_hcd = hcd_to_dummy_hcd(hcd);
	spin_lock_irqsave(&dum_hcd->link->lock, flags);

	rc = usb_hcd_check_unlink_urb(hcd, urb, status);
	if (!rc && dum_hcd->rh_state != DUMMY_RH_RUNNING &&
			!list_empty(&dum_hcd->urbp_list))
		mod_timer(&dum_hcd->timer, jiffies);

	spin_unlock_irqrestore(&dum_hcd->link->lock, flags);
	return rc;
}

static int dummy_perform_transfer(struct urb *urb, struct dummy_request *req,
		u32 len)
{
	void *ubuf, *rbuf;
	struct urbp *urbp = urb->hcpriv;
	int to_host;
	struct sg_mapping_iter *miter = &urbp->miter;
	u32 trans = 0;
	u32 this_sg;
	bool next_sg;

	to_host = usb_pipein(urb->pipe);
	rbuf = req->req.buf + req->req.actual;

	if (!urb->num_sgs) {
		ubuf = urb->transfer_buffer + urb->actual_length;
		if (to_host)
			memcpy(ubuf, rbuf, len);
		else
			memcpy(rbuf, ubuf, len);
		return len;
	}

	if (!urbp->miter_started) {
		u32 flags = SG_MITER_ATOMIC;

		if (to_host)
			flags |= SG_MITER_TO_SG;
		else
			flags |= SG_MITER_FROM_SG;

		sg_miter_start(miter, urb->sg, urb->num_sgs, flags);
		urbp->miter_started = 1;
	}
	next_sg = sg_miter_next(miter);
	if (next_sg == false) {
		WARN_ON_ONCE(1);
		return -EINVAL;
	}
	do {
		ubuf = miter->addr;
		this_sg = min_t(u32, len, miter->length);
		miter->consumed = this_sg;
		trans += this_sg;

		if (to_host)
			memcpy(ubuf, rbuf, this_sg);
		else
			memcpy(rbuf, ubuf, this_sg);
		len -= this_sg;

		if (!len)
			break;
		next_sg = sg_miter_next(miter);
		if (next_sg == false) {
			WARN_ON_ONCE(1);
			return -EINVAL;
		}

		rbuf += this_sg;
	} while (1);

	sg_miter_stop(miter);
	return trans;
}

/* transfer up to a frame's worth; caller must own lock */
static int transfer(struct dummy_hcd *dum_hcd, struct urb *urb,
		struct dummy_ep *ep, int limit, int *status)
{
	struct dummy_link	*dum = dum_hcd->link;
	struct dummy_request	*req;

top:
	/* if there's no request queued, the device is NAKing; return */
	list_for_each_entry(req, &ep->queue, queue) {
		unsigned	host_len, dev_len, len;
		int		is_short, to_host;
		int		rescan = 0;

		if (dummy_ep_stream_en(dum_hcd, urb)) {
			if ((urb->stream_id != req->req.stream_id))
				continue;
		}

		/* 1..N packets of ep->ep.maxpacket each ... the last one
		 * may be short (including zero length).
		 *
		 * writer can send a zlp explicitly (length 0) or implicitly
		 * (length mod maxpacket zero, and 'zero' flag); they always
		 * terminate reads.
		 */
		host_len = urb->transfer_buffer_length - urb->actual_length;
		dev_len = req->req.length - req->req.actual;
		len = min(host_len, dev_len);

		/* FIXME update emulated data toggle too */

		to_host = usb_pipein(urb->pipe);
		if (unlikely(len == 0))
			is_short = 1;
		else {
			/* not enough bandwidth left? */
			if (limit < ep->ep.maxpacket && limit < len)
				break;
			len = min_t(unsigned, len, limit);
			if (len == 0)
				break;

			/* use an extra pass for the final short packet */
			if (len > ep->ep.maxpacket) {
				rescan = 1;
				len -= (len % ep->ep.maxpacket);
			}
			is_short = (len % ep->ep.maxpacket) != 0;

			len = dummy_perform_transfer(urb, req, len);

			ep->last_io = jiffies;
			if ((int)len < 0) {
				req->req.status = len;
			} else {
				limit -= len;
				urb->actual_length += len;
				req->req.actual += len;
			}
		}

		/* short packets terminate, maybe with overflow/underflow.
		 * it's only really an error to write too much.
		 *
		 * partially filling a buffer optionally blocks queue advances
		 * (so completion handlers can clean up the queue) but we don't
		 * need to emulate such data-in-flight.
		 */
		if (is_short) {
			if (host_len == dev_len) {
				req->req.status = 0;
				*status = 0;
			} else if (to_host) {
				req->req.status = 0;
				if (dev_len > host_len)
					*status = -EOVERFLOW;
				else
					*status = 0;
			} else if (!to_host) {
				*status = 0;
				if (host_len > dev_len)
					req->req.status = -EOVERFLOW;
				else
					req->req.status = 0;
			}

		/* many requests terminate without a short packet */
		} else {
			if (req->req.length == req->req.actual
					&& !req->req.zero)
				req->req.status = 0;
			if (urb->transfer_buffer_length == urb->actual_length
					&& !(urb->transfer_flags
						& URB_ZERO_PACKET))
				*status = 0;
		}

		/* device side completion --> continuable */
		if (req->req.status != -EINPROGRESS) {
			list_del_init(&req->queue);

			spin_unlock(&dum->lock);
			req->req.complete(&ep->ep, &req->req);
			spin_lock(&dum->lock);

			/* requests might have been unlinked... */
			rescan = 1;
		}

		/* host side completion --> terminate */
		if (*status != -EINPROGRESS)
			break;

		/* rescan to continue with any other queued i/o */
		if (rescan)
			goto top;
	}
	return limit;
}

/* TODO rework this function */
static int periodic_bytes(struct dummy_link *dum, struct dummy_ep *ep)
{
	int	limit = ep->ep.maxpacket;

	if (dum->device->gadget.speed == USB_SPEED_HIGH) {
		int	tmp;

		/* high bandwidth mode */
		tmp = usb_endpoint_maxp(ep->desc);
		tmp = (tmp >> 11) & 0x03;
		tmp *= 8 /* applies to entire frame */;
		limit += limit * tmp;
	}
	if (dum->device->gadget.speed == USB_SPEED_SUPER) {
		switch (usb_endpoint_type(ep->desc)) {
		case USB_ENDPOINT_XFER_ISOC:
			/* Sec. 4.4.8.2 USB3.0 Spec */
			limit = 3 * 16 * 1024 * 8;
			break;
		case USB_ENDPOINT_XFER_INT:
			/* Sec. 4.4.7.2 USB3.0 Spec */
			limit = 3 * 1024 * 8;
			break;
		case USB_ENDPOINT_XFER_BULK:
		default:
			break;
		}
	}
	return limit;
}

#define is_active(dum_hcd)	((dum_hcd->port_status & \
		(USB_PORT_STAT_CONNECTION | USB_PORT_STAT_ENABLE | \
			USB_PORT_STAT_SUSPEND)) \
		== (USB_PORT_STAT_CONNECTION | USB_PORT_STAT_ENABLE))

/* TODO totaly rework this function*/
static struct dummy_ep *find_endpoint(struct dummy_link *dum, u8 address)
{
	int		i;

	if (!is_active((dum->device->gadget.speed == USB_SPEED_SUPER || !dum->host2 ?
			 dum->host : dum->host2)))
		return NULL;
	if ((address & ~USB_DIR_IN) == 0)
		return &dum->device->ep[0];
	for (i = 1; i < DUMMY_ENDPOINTS; i++) {
		struct dummy_ep	*ep = &dum->device->ep[i];

		if (!ep->desc)
			continue;
		if (ep->desc->bEndpointAddress == address)
			return ep;
	}
	return NULL;
}

#undef is_active

#define Dev_Request	(USB_TYPE_STANDARD | USB_RECIP_DEVICE)
#define Dev_InRequest	(Dev_Request | USB_DIR_IN)
#define Intf_Request	(USB_TYPE_STANDARD | USB_RECIP_INTERFACE)
#define Intf_InRequest	(Intf_Request | USB_DIR_IN)
#define Ep_Request	(USB_TYPE_STANDARD | USB_RECIP_ENDPOINT)
#define Ep_InRequest	(Ep_Request | USB_DIR_IN)


/**
 * handle_control_request() - handles all control transfers
 * @dum: pointer to dummy (the_controller)
 * @urb: the urb request to handle
 * @setup: pointer to the setup data for a USB device control
 *	 request
 * @status: pointer to request handling status
 *
 * Return 0 - if the request was handled
 *	  1 - if the request wasn't handles
 *	  error code on error
 */
static int handle_control_request(struct dummy_hcd *dum_hcd, struct urb *urb,
				  struct usb_ctrlrequest *setup,
				  int *status)
{
	struct dummy_ep		*ep2;
	struct dummy_link	*dum = dum_hcd->link;
	struct dummy_udc        *dum_udc = dum->device;
	int			ret_val = 1;
	unsigned	w_index;
	unsigned	w_value;

	w_index = le16_to_cpu(setup->wIndex);
	w_value = le16_to_cpu(setup->wValue);
	switch (setup->bRequest) {
	case USB_REQ_SET_ADDRESS:
		if (setup->bRequestType != Dev_Request)
			break;
		dum->address = w_value;
		*status = 0;
		dev_dbg(udc_dev(dum), "set_address = %d\n",
				w_value);
		ret_val = 0;
		break;
	case USB_REQ_SET_FEATURE:
		if (setup->bRequestType == Dev_Request) {
			ret_val = 0;
			/* TODO avoid direct access to gadget */
			switch (w_value) {
			case USB_DEVICE_REMOTE_WAKEUP:
				break;
			case USB_DEVICE_B_HNP_ENABLE:
				dum_udc->gadget.b_hnp_enable = 1;
				break;
			case USB_DEVICE_A_HNP_SUPPORT:
				dum_udc->gadget.a_hnp_support = 1;
				break;
			case USB_DEVICE_A_ALT_HNP_SUPPORT:
				dum_udc->gadget.a_alt_hnp_support = 1;
				break;
			case USB_DEVICE_U1_ENABLE:
				if (dummy_hcd_to_hcd(dum_hcd)->speed ==
				    HCD_USB3)
					w_value = USB_DEV_STAT_U1_ENABLED;
				else
					ret_val = -EOPNOTSUPP;
				break;
			case USB_DEVICE_U2_ENABLE:
				if (dummy_hcd_to_hcd(dum_hcd)->speed ==
				    HCD_USB3)
					w_value = USB_DEV_STAT_U2_ENABLED;
				else
					ret_val = -EOPNOTSUPP;
				break;
			case USB_DEVICE_LTM_ENABLE:
				if (dummy_hcd_to_hcd(dum_hcd)->speed ==
				    HCD_USB3)
					w_value = USB_DEV_STAT_LTM_ENABLED;
				else
					ret_val = -EOPNOTSUPP;
				break;
			default:
				ret_val = -EOPNOTSUPP;
			}
			if (ret_val == 0) {
				dum_udc->devstatus |= (1 << w_value);
				*status = 0;
			}
		} else if (setup->bRequestType == Ep_Request) {
			/* endpoint halt */
			ep2 = find_endpoint(dum, w_index);
			if (!ep2 || ep2->ep.name == ep0name) {
				ret_val = -EOPNOTSUPP;
				break;
			}
			ep2->halted = 1;
			ret_val = 0;
			*status = 0;
		}
		break;
	case USB_REQ_CLEAR_FEATURE:
		if (setup->bRequestType == Dev_Request) {
			ret_val = 0;
			/* TODO avoid getting direct to gadget structure */
			switch (w_value) {
			case USB_DEVICE_REMOTE_WAKEUP:
				w_value = USB_DEVICE_REMOTE_WAKEUP;
				break;
			case USB_DEVICE_U1_ENABLE:
				if (dummy_hcd_to_hcd(dum_hcd)->speed ==
				    HCD_USB3)
					w_value = USB_DEV_STAT_U1_ENABLED;
				else
					ret_val = -EOPNOTSUPP;
				break;
			case USB_DEVICE_U2_ENABLE:
				if (dummy_hcd_to_hcd(dum_hcd)->speed ==
				    HCD_USB3)
					w_value = USB_DEV_STAT_U2_ENABLED;
				else
					ret_val = -EOPNOTSUPP;
				break;
			case USB_DEVICE_LTM_ENABLE:
				if (dummy_hcd_to_hcd(dum_hcd)->speed ==
				    HCD_USB3)
					w_value = USB_DEV_STAT_LTM_ENABLED;
				else
					ret_val = -EOPNOTSUPP;
				break;
			default:
				ret_val = -EOPNOTSUPP;
				break;
			}
			if (ret_val == 0) {
				dum_udc->devstatus &= ~(1 << w_value);
				*status = 0;
			}
		} else if (setup->bRequestType == Ep_Request) {
			/* endpoint halt */
			ep2 = find_endpoint(dum, w_index);
			if (!ep2) {
				ret_val = -EOPNOTSUPP;
				break;
			}
			if (!ep2->wedged)
				ep2->halted = 0;
			ret_val = 0;
			*status = 0;
		}
		break;
	case USB_REQ_GET_STATUS:
		if (setup->bRequestType == Dev_InRequest
				|| setup->bRequestType == Intf_InRequest
				|| setup->bRequestType == Ep_InRequest) {
			char *buf;
			/*
			 * device: remote wakeup, selfpowered
			 * interface: nothing
			 * endpoint: halt
			 */
			buf = (char *)urb->transfer_buffer;
			if (urb->transfer_buffer_length > 0) {
				if (setup->bRequestType == Ep_InRequest) {
					ep2 = find_endpoint(dum, w_index);
					if (!ep2) {
						ret_val = -EOPNOTSUPP;
						break;
					}
					buf[0] = ep2->halted;
				} else if (setup->bRequestType ==
					   Dev_InRequest) {
					buf[0] = (u8)dum_udc->devstatus;
				} else
					buf[0] = 0;
			}
			if (urb->transfer_buffer_length > 1)
				buf[1] = 0;
			urb->actual_length = min_t(u32, 2,
				urb->transfer_buffer_length);
			ret_val = 0;
			*status = 0;
		}
		break;
	}
	return ret_val;
}

/* drive both sides of the transfers; looks like irq handlers to
 * both drivers except the callbacks aren't in_irq().
 */
static void dummy_timer(unsigned long _dum_hcd)
{
	struct dummy_hcd	*dum_hcd = (struct dummy_hcd *) _dum_hcd;
	struct dummy_link	*dum = dum_hcd->link;
	struct dummy_udc        *dum_udc = dum->device;
 	struct urbp		*urbp, *tmp;
	unsigned long		flags;
	int			limit, total;
	int			i;

	/* simplistic model for one frame's bandwidth */
	/* TODO avoid direct getting of device data */
	switch (dum_udc->gadget.speed) {
	case USB_SPEED_LOW:
		total = 8/*bytes*/ * 12/*packets*/;
		break;
	case USB_SPEED_FULL:
		total = 64/*bytes*/ * 19/*packets*/;
		break;
	case USB_SPEED_HIGH:
		total = 512/*bytes*/ * 13/*packets*/ * 8/*uframes*/;
		break;
	case USB_SPEED_SUPER:
		/* Bus speed is 500000 bytes/ms, so use a little less */
		total = 490000;
		break;
	default:
		dev_err(dummy_dev(dum_hcd), "bogus device speed\n");
		return;
	}

	/* FIXME if HZ != 1000 this will probably misbehave ... */

	/* look at each urb queued by the host side driver */
	spin_lock_irqsave(&dum->lock, flags);

	if (!dum_hcd->udev) {
		dev_err(dummy_dev(dum_hcd),
				"timer fired with no URBs pending?\n");
		spin_unlock_irqrestore(&dum->lock, flags);
		return;
	}

	/* TODO avoid iteration on device endpoints here */
	for (i = 0; i < DUMMY_ENDPOINTS; i++) {
		if (!ep_name[i])
			break;
		dum_udc->ep[i].already_seen = 0;
	}

restart:
	list_for_each_entry_safe(urbp, tmp, &dum_hcd->urbp_list, urbp_list) {
		struct urb		*urb;
		struct dummy_request	*req;
		u8			address;
		struct dummy_ep		*ep = NULL;
		int			type;
		int			status = -EINPROGRESS;

		urb = urbp->urb;
		if (urb->unlinked)
			goto return_urb;
		else if (dum_hcd->rh_state != DUMMY_RH_RUNNING)
			continue;
		type = usb_pipetype(urb->pipe);

		/* used up this frame's non-periodic bandwidth?
		 * FIXME there's infinite bandwidth for control and
		 * periodic transfers ... unrealistic.
		 */
		if (total <= 0 && type == PIPE_BULK)
			continue;

		/* find the gadget's ep for this request (if configured) */
		address = usb_pipeendpoint (urb->pipe);
		if (usb_pipein(urb->pipe))
			address |= USB_DIR_IN;
		ep = find_endpoint(dum, address);
		if (!ep) {
			/* set_configuration() disagreement */
			dev_dbg(dummy_dev(dum_hcd),
				"no ep configured for urb %p\n",
				urb);
			status = -EPROTO;
			goto return_urb;
		}

		if (ep->already_seen)
			continue;
		ep->already_seen = 1;
		if (ep == &dum_udc->ep[0] && urb->error_count) {
			ep->setup_stage = 1;	/* a new urb */
			urb->error_count = 0;
		}
		if (ep->halted && !ep->setup_stage) {
			/* NOTE: must not be iso! */
			dev_dbg(dummy_dev(dum_hcd), "ep %s halted, urb %p\n",
					ep->ep.name, urb);
			status = -EPIPE;
			goto return_urb;
		}
		/* FIXME make sure both ends agree on maxpacket */

		/* handle control requests */
		if (ep == &dum_udc->ep[0] && ep->setup_stage) {
			struct usb_ctrlrequest		setup;
			int				value = 1;

			setup = *(struct usb_ctrlrequest *) urb->setup_packet;
			/* paranoia, in case of stale queued data */
			list_for_each_entry(req, &ep->queue, queue) {
				list_del_init(&req->queue);
				req->req.status = -EOVERFLOW;
				dev_dbg(udc_dev(dum), "stale req = %p\n",
						req);

				spin_unlock(&dum->lock);
				req->req.complete(&ep->ep, &req->req);
				spin_lock(&dum->lock);
				ep->already_seen = 0;
				goto restart;
			}

			/* gadget driver never sees set_address or operations
			 * on standard feature flags.  some hardware doesn't
			 * even expose them.
			 */
			ep->last_io = jiffies;
			ep->setup_stage = 0;
			ep->halted = 0;

			value = handle_control_request(dum_hcd, urb, &setup,
						       &status);

			/* gadget driver handles all other requests.  block
			 * until setup() returns; no reentrancy issues etc.
			 */
			if (value > 0) {
				spin_unlock(&dum->lock);
				/* TODO avoid getting directly to device */
				value = dum_udc->driver->setup(&dum_udc->gadget,
						&setup);
				spin_lock(&dum->lock);

				if (value >= 0) {
					/* no delays (max 64KB data stage) */
					limit = 64*1024;
					goto treat_control_like_bulk;
				}
				/* error, see below */
			}

			if (value < 0) {
				if (value != -EOPNOTSUPP)
					dev_dbg(udc_dev(dum),
						"setup --> %d\n",
						value);
				status = -EPIPE;
				urb->actual_length = 0;
			}

			goto return_urb;
		}

		/* non-control requests */
		limit = total;
		switch (usb_pipetype(urb->pipe)) {
		case PIPE_ISOCHRONOUS:
			/* FIXME is it urb->interval since the last xfer?
			 * use urb->iso_frame_desc[i].
			 * complete whether or not ep has requests queued.
			 * report random errors, to debug drivers.
			 */
			limit = max(limit, periodic_bytes(dum, ep));
			status = -ENOSYS;
			break;

		case PIPE_INTERRUPT:
			/* FIXME is it urb->interval since the last xfer?
			 * this almost certainly polls too fast.
			 */
			limit = max(limit, periodic_bytes(dum, ep));
			/* FALLTHROUGH */

		default:
treat_control_like_bulk:
			ep->last_io = jiffies;
			total = transfer(dum_hcd, urb, ep, limit, &status);
			break;
		}

		/* incomplete transfer? */
		if (status == -EINPROGRESS)
			continue;

return_urb:
		list_del(&urbp->urbp_list);
		kfree(urbp);
		if (ep)
			ep->already_seen = ep->setup_stage = 0;

		usb_hcd_unlink_urb_from_ep(dummy_hcd_to_hcd(dum_hcd), urb);
		spin_unlock(&dum->lock);
		usb_hcd_giveback_urb(dummy_hcd_to_hcd(dum_hcd), urb, status);
		spin_lock(&dum->lock);

		goto restart;
	}

	if (list_empty(&dum_hcd->urbp_list)) {
		usb_put_dev(dum_hcd->udev);
		dum_hcd->udev = NULL;
	} else if (dum_hcd->rh_state == DUMMY_RH_RUNNING) {
		/* want a 1 msec delay here */
		mod_timer(&dum_hcd->timer, jiffies + msecs_to_jiffies(1));
	}

	spin_unlock_irqrestore(&dum->lock, flags);
}

/*-------------------------------------------------------------------------*/

#define PORT_C_MASK \
	((USB_PORT_STAT_C_CONNECTION \
	| USB_PORT_STAT_C_ENABLE \
	| USB_PORT_STAT_C_SUSPEND \
	| USB_PORT_STAT_C_OVERCURRENT \
	| USB_PORT_STAT_C_RESET) << 16)

static int dummy_hub_status(struct usb_hcd *hcd, char *buf)
{
	struct dummy_hcd	*dum_hcd;
	unsigned long		flags;
	int			retval = 0;

	dum_hcd = hcd_to_dummy_hcd(hcd);

	spin_lock_irqsave(&dum_hcd->link->lock, flags);
	if (!HCD_HW_ACCESSIBLE(hcd))
		goto done;

	if (dum_hcd->resuming && time_after_eq(jiffies, dum_hcd->re_timeout)) {
		dum_hcd->port_status |= (USB_PORT_STAT_C_SUSPEND << 16);
		dum_hcd->port_status &= ~USB_PORT_STAT_SUSPEND;
		set_link_state(dum_hcd);
	}

	if ((dum_hcd->port_status & PORT_C_MASK) != 0) {
		*buf = (1 << 1);
		dev_dbg(dummy_dev(dum_hcd), "port status 0x%08x has changes\n",
				dum_hcd->port_status);
		retval = 1;
		if (dum_hcd->rh_state == DUMMY_RH_SUSPENDED)
			usb_hcd_resume_root_hub(hcd);
	}
done:
	spin_unlock_irqrestore(&dum_hcd->link->lock, flags);
	return retval;
}

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

static inline void
ss_hub_descriptor(struct usb_hub_descriptor *desc)
{
	memset(desc, 0, sizeof *desc);
	desc->bDescriptorType = 0x2a;
	desc->bDescLength = 12;
	desc->wHubCharacteristics = cpu_to_le16(0x0001);
	desc->bNbrPorts = 1;
	desc->u.ss.bHubHdrDecLat = 0x04; /* Worst case: 0.4 micro sec*/
	desc->u.ss.DeviceRemovable = 0xffff;
}

static inline void hub_descriptor(struct usb_hub_descriptor *desc)
{
	memset(desc, 0, sizeof *desc);
	desc->bDescriptorType = 0x29;
	desc->bDescLength = 9;
	desc->wHubCharacteristics = cpu_to_le16(0x0001);
	desc->bNbrPorts = 1;
	desc->u.hs.DeviceRemovable[0] = 0xff;
	desc->u.hs.DeviceRemovable[1] = 0xff;
}

static int dummy_hub_control(
	struct usb_hcd	*hcd,
	u16		typeReq,
	u16		wValue,
	u16		wIndex,
	char		*buf,
	u16		wLength
) {
	struct dummy_hcd *dum_hcd;
	struct dummy_link *dum;
	struct dummy_udc  *dum_udc;
	int		retval = 0;
	unsigned long	flags;

	if (!HCD_HW_ACCESSIBLE(hcd))
		return -ETIMEDOUT;

	dum_hcd = hcd_to_dummy_hcd(hcd);
	dum = dum_hcd->link;
	dum_udc = dum->device;

	spin_lock_irqsave(&dum->lock, flags);
	switch (typeReq) {
	case ClearHubFeature:
		break;
	case ClearPortFeature:
		switch (wValue) {
		case USB_PORT_FEAT_SUSPEND:
			if (hcd->speed == HCD_USB3) {
				dev_dbg(dummy_dev(dum_hcd),
					 "USB_PORT_FEAT_SUSPEND req not "
					 "supported for USB 3.0 roothub\n");
				goto error;
			}
			if (dum_hcd->port_status & USB_PORT_STAT_SUSPEND) {
				/* 20msec resume signaling */
				dum_hcd->resuming = 1;
				dum_hcd->re_timeout = jiffies +
						msecs_to_jiffies(20);
			}
			break;
		case USB_PORT_FEAT_POWER:
			if (hcd->speed == HCD_USB3) {
				if (dum_hcd->port_status & USB_PORT_STAT_POWER)
					dev_dbg(dummy_dev(dum_hcd),
						"power-off\n");
			} else
				if (dum_hcd->port_status &
							USB_SS_PORT_STAT_POWER)
					dev_dbg(dummy_dev(dum_hcd),
						"power-off\n");
			/* FALLS THROUGH */
		default:
			dum_hcd->port_status &= ~(1 << wValue);
			set_link_state(dum_hcd);
		}
		break;
	case GetHubDescriptor:
		if (hcd->speed == HCD_USB3 &&
				(wLength < USB_DT_SS_HUB_SIZE ||
				 wValue != (USB_DT_SS_HUB << 8))) {
			dev_dbg(dummy_dev(dum_hcd),
				"Wrong hub descriptor type for "
				"USB 3.0 roothub.\n");
			goto error;
		}
		if (hcd->speed == HCD_USB3)
			ss_hub_descriptor((struct usb_hub_descriptor *) buf);
		else
			hub_descriptor((struct usb_hub_descriptor *) buf);
		break;

	case DeviceRequest | USB_REQ_GET_DESCRIPTOR:
		if (hcd->speed != HCD_USB3)
			goto error;

		if ((wValue >> 8) != USB_DT_BOS)
			goto error;

		memcpy(buf, &usb3_bos_desc, sizeof(usb3_bos_desc));
		retval = sizeof(usb3_bos_desc);
		break;

	case GetHubStatus:
		*(__le32 *) buf = cpu_to_le32(0);
		break;
	case GetPortStatus:
		if (wIndex != 1)
			retval = -EPIPE;

		/* whoever resets or resumes must GetPortStatus to
		 * complete it!!
		 */
		if (dum_hcd->resuming &&
				time_after_eq(jiffies, dum_hcd->re_timeout)) {
			dum_hcd->port_status |= (USB_PORT_STAT_C_SUSPEND << 16);
			dum_hcd->port_status &= ~USB_PORT_STAT_SUSPEND;
		}
		if ((dum_hcd->port_status & USB_PORT_STAT_RESET) != 0 &&
				time_after_eq(jiffies, dum_hcd->re_timeout)) {
			dum_hcd->port_status |= (USB_PORT_STAT_C_RESET << 16);
			dum_hcd->port_status &= ~USB_PORT_STAT_RESET;
			/* TODO avoid direct getting device data */
			if (dum_udc->pullup) {
				dum_hcd->port_status |= USB_PORT_STAT_ENABLE;

				if (hcd->speed < HCD_USB3) {
					/* TODO Also here avoid this */
					switch (dum_udc->gadget.speed) {
					case USB_SPEED_HIGH:
						dum_hcd->port_status |=
						      USB_PORT_STAT_HIGH_SPEED;
						break;
					case USB_SPEED_LOW:
						dum_udc->gadget.ep0->
							maxpacket = 8;
						dum_hcd->port_status |=
							USB_PORT_STAT_LOW_SPEED;
						break;
					default:
						dum_udc->gadget.speed =
							USB_SPEED_FULL;
						break;
					}
				}
			}
		}
		set_link_state(dum_hcd);
		((__le16 *) buf)[0] = cpu_to_le16(dum_hcd->port_status);
		((__le16 *) buf)[1] = cpu_to_le16(dum_hcd->port_status >> 16);
		break;
	case SetHubFeature:
		retval = -EPIPE;
		break;
	case SetPortFeature:
		switch (wValue) {
		case USB_PORT_FEAT_LINK_STATE:
			if (hcd->speed != HCD_USB3) {
				dev_dbg(dummy_dev(dum_hcd),
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
				dev_dbg(dummy_dev(dum_hcd),
					 "USB_PORT_FEAT_U1/2_TIMEOUT req not "
					 "supported for USB 2.0 roothub\n");
				goto error;
			}
			break;
		case USB_PORT_FEAT_SUSPEND:
			/* Applicable only for USB2.0 hub */
			if (hcd->speed == HCD_USB3) {
				dev_dbg(dummy_dev(dum_hcd),
					 "USB_PORT_FEAT_SUSPEND req not "
					 "supported for USB 3.0 roothub\n");
				goto error;
			}
			if (dum_hcd->active) {
				dum_hcd->port_status |= USB_PORT_STAT_SUSPEND;

				/* HNP would happen here; for now we
				 * assume b_bus_req is always true.
				 */
				set_link_state(dum_hcd);
				/* TODO avoid getting data from device structure */
				if (((1 << USB_DEVICE_B_HNP_ENABLE)
						&dum_udc->devstatus) != 0)
					dev_dbg(dummy_dev(dum_hcd),
							"no HNP yet!\n");
			}
			break;
		case USB_PORT_FEAT_POWER:
			if (hcd->speed == HCD_USB3)
				dum_hcd->port_status |= USB_SS_PORT_STAT_POWER;
			else
				dum_hcd->port_status |= USB_PORT_STAT_POWER;
			set_link_state(dum_hcd);
			break;
		case USB_PORT_FEAT_BH_PORT_RESET:
			/* Applicable only for USB3.0 hub */
			if (hcd->speed != HCD_USB3) {
				dev_dbg(dummy_dev(dum_hcd),
					 "USB_PORT_FEAT_BH_PORT_RESET req not "
					 "supported for USB 2.0 roothub\n");
				goto error;
			}
			/* FALLS THROUGH */
		case USB_PORT_FEAT_RESET:
			/* if it's already enabled, disable */
			if (hcd->speed == HCD_USB3) {
				dum_hcd->port_status = 0;
				dum_hcd->port_status =
					(USB_SS_PORT_STAT_POWER |
					 USB_PORT_STAT_CONNECTION |
					 USB_PORT_STAT_RESET);
			} else
				dum_hcd->port_status &= ~(USB_PORT_STAT_ENABLE
					| USB_PORT_STAT_LOW_SPEED
					| USB_PORT_STAT_HIGH_SPEED);
			/*
			 * We want to reset device status. All but the
			 * Self powered feature
			 */
			/* TODO avoid getting direct access to device */
			dum_udc->devstatus &= (1 << USB_DEVICE_SELF_POWERED);

			/*
			 * FIXME USB3.0: what is the correct reset signaling
			 * interval? Is it still 50msec as for HS?
			 */
			dum_hcd->re_timeout = jiffies + msecs_to_jiffies(50);
			/* FALLS THROUGH */
		default:
			if (hcd->speed == HCD_USB3) {
				if ((dum_hcd->port_status &
				     USB_SS_PORT_STAT_POWER) != 0) {
					dum_hcd->port_status |= (1 << wValue);
					set_link_state(dum_hcd);
				}
			} else
				if ((dum_hcd->port_status &
				     USB_PORT_STAT_POWER) != 0) {
					dum_hcd->port_status |= (1 << wValue);
					set_link_state(dum_hcd);
				}
		}
		break;
	case GetPortErrorCount:
		if (hcd->speed != HCD_USB3) {
			dev_dbg(dummy_dev(dum_hcd),
				 "GetPortErrorCount req not "
				 "supported for USB 2.0 roothub\n");
			goto error;
		}
		/* We'll always return 0 since this is a dummy hub */
		*(__le32 *) buf = cpu_to_le32(0);
		break;
	case SetHubDepth:
		if (hcd->speed != HCD_USB3) {
			dev_dbg(dummy_dev(dum_hcd),
				 "SetHubDepth req not supported for "
				 "USB 2.0 roothub\n");
			goto error;
		}
		break;
	default:
		dev_dbg(dummy_dev(dum_hcd),
			"hub control req%04x v%04x i%04x l%d\n",
			typeReq, wValue, wIndex, wLength);
error:
		/* "protocol stall" on error */
		retval = -EPIPE;
	}
	spin_unlock_irqrestore(&dum->lock, flags);

	if ((dum_hcd->port_status & PORT_C_MASK) != 0)
		usb_hcd_poll_rh_status(hcd);
	return retval;
}

static int dummy_bus_suspend(struct usb_hcd *hcd)
{
	struct dummy_hcd *dum_hcd = hcd_to_dummy_hcd(hcd);

	dev_dbg(&hcd->self.root_hub->dev, "%s\n", __func__);

	spin_lock_irq(&dum_hcd->link->lock);
	dum_hcd->rh_state = DUMMY_RH_SUSPENDED;
	set_link_state(dum_hcd);
	hcd->state = HC_STATE_SUSPENDED;
	spin_unlock_irq(&dum_hcd->link->lock);
	return 0;
}

static int dummy_bus_resume(struct usb_hcd *hcd)
{
	struct dummy_hcd *dum_hcd = hcd_to_dummy_hcd(hcd);
	int rc = 0;

	dev_dbg(&hcd->self.root_hub->dev, "%s\n", __func__);

	spin_lock_irq(&dum_hcd->link->lock);
	if (!HCD_HW_ACCESSIBLE(hcd)) {
		rc = -ESHUTDOWN;
	} else {
		dum_hcd->rh_state = DUMMY_RH_RUNNING;
		set_link_state(dum_hcd);
		if (!list_empty(&dum_hcd->urbp_list))
			mod_timer(&dum_hcd->timer, jiffies);
		hcd->state = HC_STATE_RUNNING;
	}
	spin_unlock_irq(&dum_hcd->link->lock);
	return rc;
}

/*-------------------------------------------------------------------------*/

static inline ssize_t show_urb(char *buf, size_t size, struct urb *urb)
{
	int ep = usb_pipeendpoint(urb->pipe);

	return snprintf(buf, size,
		"urb/%p %s ep%d%s%s len %d/%d\n",
		urb,
		({ char *s;
		switch (urb->dev->speed) {
		case USB_SPEED_LOW:
			s = "ls";
			break;
		case USB_SPEED_FULL:
			s = "fs";
			break;
		case USB_SPEED_HIGH:
			s = "hs";
			break;
		case USB_SPEED_SUPER:
			s = "ss";
			break;
		default:
			s = "?";
			break;
		 } s; }),
		ep, ep ? (usb_pipein(urb->pipe) ? "in" : "out") : "",
		({ char *s; \
		switch (usb_pipetype(urb->pipe)) { \
		case PIPE_CONTROL: \
			s = ""; \
			break; \
		case PIPE_BULK: \
			s = "-bulk"; \
			break; \
		case PIPE_INTERRUPT: \
			s = "-int"; \
			break; \
		default: \
			s = "-iso"; \
			break; \
		} s; }),
		urb->actual_length, urb->transfer_buffer_length);
}

static ssize_t urbs_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	struct usb_hcd		*hcd = dev_get_drvdata(dev);
	struct dummy_hcd	*dum_hcd = hcd_to_dummy_hcd(hcd);
	struct urbp		*urbp;
	size_t			size = 0;
	unsigned long		flags;

	spin_lock_irqsave(&dum_hcd->link->lock, flags);
	list_for_each_entry(urbp, &dum_hcd->urbp_list, urbp_list) {
		size_t		temp;

		temp = show_urb(buf, PAGE_SIZE - size, urbp->urb);
		buf += temp;
		size += temp;
	}
	spin_unlock_irqrestore(&dum_hcd->link->lock, flags);

	return size;
}
static DEVICE_ATTR_RO(urbs);

static int dummy_start_ss(struct dummy_hcd *dum_hcd)
{
	init_timer(&dum_hcd->timer);
	dum_hcd->timer.function = dummy_timer;
	dum_hcd->timer.data = (unsigned long)dum_hcd;
	dum_hcd->rh_state = DUMMY_RH_RUNNING;
	dum_hcd->stream_en_ep = 0;
	INIT_LIST_HEAD(&dum_hcd->urbp_list);
	dummy_hcd_to_hcd(dum_hcd)->power_budget = POWER_BUDGET;
	dummy_hcd_to_hcd(dum_hcd)->state = HC_STATE_RUNNING;
	dummy_hcd_to_hcd(dum_hcd)->uses_new_polling = 1;
#ifdef CONFIG_USB_OTG
	dummy_hcd_to_hcd(dum_hcd)->self.otg_port = 1;
#endif
	return 0;

	/* FIXME 'urbs' should be a per-device thing, maybe in usbcore */
	return device_create_file(dummy_dev(dum_hcd), &dev_attr_urbs);
}

static int dummy_start(struct usb_hcd *hcd)
{
	struct dummy_hcd	*dum_hcd = hcd_to_dummy_hcd(hcd);

	/*
	 * MASTER side init ... we emulate a root hub that'll only ever
	 * talk to one device (the slave side).  Also appears in sysfs,
	 * just like more familiar pci-based HCDs.
	 */
	if (!usb_hcd_is_primary_hcd(hcd))
		return dummy_start_ss(dum_hcd);

	spin_lock_init(&dum_hcd->link->lock);
	init_timer(&dum_hcd->timer);
	dum_hcd->timer.function = dummy_timer;
	dum_hcd->timer.data = (unsigned long)dum_hcd;
	dum_hcd->rh_state = DUMMY_RH_RUNNING;

	INIT_LIST_HEAD(&dum_hcd->urbp_list);

	hcd->power_budget = POWER_BUDGET;
	hcd->state = HC_STATE_RUNNING;
	hcd->uses_new_polling = 1;

#ifdef CONFIG_USB_OTG
	hcd->self.otg_port = 1;
#endif

	/* FIXME 'urbs' should be a per-device thing, maybe in usbcore */
	return device_create_file(dummy_dev(dum_hcd), &dev_attr_urbs);
}

static void dummy_stop(struct usb_hcd *hcd)
{
	struct dummy_link	*dum;

	dum = hcd_to_dummy_hcd(hcd)->link;
	device_remove_file(dummy_dev(hcd_to_dummy_hcd(hcd)), &dev_attr_urbs);
	/* TODO we are getting here to device structure, this should be changed */
	usb_gadget_unregister_driver(dum->device->driver);
	dev_info(dummy_dev(hcd_to_dummy_hcd(hcd)), "stopped\n");
}

/*-------------------------------------------------------------------------*/

static int dummy_h_get_frame(struct usb_hcd *hcd)
{
	return dummy_g_get_frame(NULL);
}

static int dummy_setup(struct usb_hcd *hcd)
{
	struct dummy_link *dum;

	dum = *((void **)dev_get_platdata(hcd->self.controller));
	hcd->self.sg_tablesize = ~0;
	if (usb_hcd_is_primary_hcd(hcd)) {
		dum->host = hcd_to_dummy_hcd(hcd);
		dum->host->link = dum;
		/*
		 * Mark the first roothub as being USB 2.0.
		 * The USB 3.0 roothub will be registered later by
		 * dummy_hcd_probe()
		 */
		hcd->speed = HCD_USB2;
		hcd->self.root_hub->speed = USB_SPEED_HIGH;
	} else {
		/* We set host field allways to higest speed hcd */
		dum->host2 = dum->host;
		dum->host = hcd_to_dummy_hcd(hcd);
		dum->host->link = dum;
		hcd->speed = HCD_USB3;
		hcd->self.root_hub->speed = USB_SPEED_SUPER;
	}
	return 0;
}

/* Change a group of bulk endpoints to support multiple stream IDs */
static int dummy_alloc_streams(struct usb_hcd *hcd, struct usb_device *udev,
	struct usb_host_endpoint **eps, unsigned int num_eps,
	unsigned int num_streams, gfp_t mem_flags)
{
	struct dummy_hcd *dum_hcd = hcd_to_dummy_hcd(hcd);
	unsigned long flags;
	int max_stream;
	int ret_streams = num_streams;
	unsigned int index;
	unsigned int i;

	if (!num_eps)
		return -EINVAL;

	spin_lock_irqsave(&dum_hcd->link->lock, flags);
	for (i = 0; i < num_eps; i++) {
		index = dummy_get_ep_idx(&eps[i]->desc);
		if ((1 << index) & dum_hcd->stream_en_ep) {
			ret_streams = -EINVAL;
			goto out;
		}
		max_stream = usb_ss_max_streams(&eps[i]->ss_ep_comp);
		if (!max_stream) {
			ret_streams = -EINVAL;
			goto out;
		}
		if (max_stream < ret_streams) {
			dev_dbg(dummy_dev(dum_hcd), "Ep 0x%x only supports %u "
					"stream IDs.\n",
					eps[i]->desc.bEndpointAddress,
					max_stream);
			ret_streams = max_stream;
		}
	}

	for (i = 0; i < num_eps; i++) {
		index = dummy_get_ep_idx(&eps[i]->desc);
		dum_hcd->stream_en_ep |= 1 << index;
		set_max_streams_for_pipe(dum_hcd,
				usb_endpoint_num(&eps[i]->desc), ret_streams);
	}
out:
	spin_unlock_irqrestore(&dum_hcd->link->lock, flags);
	return ret_streams;
}

/* Reverts a group of bulk endpoints back to not using stream IDs. */
static int dummy_free_streams(struct usb_hcd *hcd, struct usb_device *udev,
	struct usb_host_endpoint **eps, unsigned int num_eps,
	gfp_t mem_flags)
{
	struct dummy_hcd *dum_hcd = hcd_to_dummy_hcd(hcd);
	unsigned long flags;
	int ret;
	unsigned int index;
	unsigned int i;

	spin_lock_irqsave(&dum_hcd->link->lock, flags);
	for (i = 0; i < num_eps; i++) {
		index = dummy_get_ep_idx(&eps[i]->desc);
		if (!((1 << index) & dum_hcd->stream_en_ep)) {
			ret = -EINVAL;
			goto out;
		}
	}

	for (i = 0; i < num_eps; i++) {
		index = dummy_get_ep_idx(&eps[i]->desc);
		dum_hcd->stream_en_ep &= ~(1 << index);
		set_max_streams_for_pipe(dum_hcd,
				usb_endpoint_num(&eps[i]->desc), 0);
	}
	ret = 0;
out:
	spin_unlock_irqrestore(&dum_hcd->link->lock, flags);
	return ret;
}

static struct hc_driver dummy_hcd = {
	.description =		(char *) driver_name,
	.product_desc =		"Dummy host controller",
	.hcd_priv_size =	sizeof(struct dummy_hcd),

	.flags =		HCD_USB3 | HCD_SHARED,

	.reset =		dummy_setup,
	.start =		dummy_start,
	.stop =			dummy_stop,

	.urb_enqueue =		dummy_urb_enqueue,
	.urb_dequeue =		dummy_urb_dequeue,

	.get_frame_number =	dummy_h_get_frame,

	.hub_status_data =	dummy_hub_status,
	.hub_control =		dummy_hub_control,
	.bus_suspend =		dummy_bus_suspend,
	.bus_resume =		dummy_bus_resume,

	.alloc_streams =	dummy_alloc_streams,
	.free_streams =		dummy_free_streams,
};

static int dummy_hcd_probe(struct platform_device *pdev)
{
	struct dummy_link	*dum;
	struct usb_hcd		*hs_hcd;
	struct usb_hcd		*ss_hcd;
	int			retval;

	dev_info(&pdev->dev, "%s, driver " DRIVER_VERSION "\n", driver_desc);
	dum = *((void **)dev_get_platdata(&pdev->dev));

	if (!mod_data.is_super_speed)
		dummy_hcd.flags = HCD_USB2;
	hs_hcd = usb_create_hcd(&dummy_hcd, &pdev->dev, dev_name(&pdev->dev));
	if (!hs_hcd)
		return -ENOMEM;
	hs_hcd->has_tt = 1;

	retval = usb_add_hcd(hs_hcd, 0, 0);
	if (retval)
		goto put_usb2_hcd;

	if (mod_data.is_super_speed) {
		ss_hcd = usb_create_shared_hcd(&dummy_hcd, &pdev->dev,
					dev_name(&pdev->dev), hs_hcd);
		if (!ss_hcd) {
			retval = -ENOMEM;
			goto dealloc_usb2_hcd;
		}

		retval = usb_add_hcd(ss_hcd, 0, 0);
		if (retval)
			goto put_usb3_hcd;
	}
	return 0;

put_usb3_hcd:
	usb_put_hcd(ss_hcd);
dealloc_usb2_hcd:
	usb_remove_hcd(hs_hcd);
put_usb2_hcd:
	usb_put_hcd(hs_hcd);
	dum->host = dum->host2 = NULL;
	return retval;
}

static int dummy_hcd_remove(struct platform_device *pdev)
{
	struct dummy_link	*dum;

	dum = hcd_to_dummy_hcd(platform_get_drvdata(pdev))->link;

	if (dum->host2) {
		usb_remove_hcd(dummy_hcd_to_hcd(dum->host));
		usb_put_hcd(dummy_hcd_to_hcd(dum->host));
		dum->host = dum->host2;
	}

	usb_remove_hcd(dummy_hcd_to_hcd(dum->host));
	usb_put_hcd(dummy_hcd_to_hcd(dum->host));

	dum->host = NULL;
	dum->host2 = NULL;

	return 0;
}

static int dummy_hcd_suspend(struct platform_device *pdev, pm_message_t state)
{
	struct usb_hcd		*hcd;
	struct dummy_hcd	*dum_hcd;
	int			rc = 0;

	dev_dbg(&pdev->dev, "%s\n", __func__);

	hcd = platform_get_drvdata(pdev);
	dum_hcd = hcd_to_dummy_hcd(hcd);
	if (dum_hcd->rh_state == DUMMY_RH_RUNNING) {
		dev_warn(&pdev->dev, "Root hub isn't suspended!\n");
		rc = -EBUSY;
	} else
		clear_bit(HCD_FLAG_HW_ACCESSIBLE, &hcd->flags);
	return rc;
}

static int dummy_hcd_resume(struct platform_device *pdev)
{
	struct usb_hcd		*hcd;

	dev_dbg(&pdev->dev, "%s\n", __func__);

	hcd = platform_get_drvdata(pdev);
	set_bit(HCD_FLAG_HW_ACCESSIBLE, &hcd->flags);
	usb_hcd_poll_rh_status(hcd);
	return 0;
}

static struct platform_driver dummy_hcd_driver = {
	.probe		= dummy_hcd_probe,
	.remove		= dummy_hcd_remove,
	.suspend	= dummy_hcd_suspend,
	.resume		= dummy_hcd_resume,
	.driver		= {
		.name	= (char *) driver_name,
		.owner	= THIS_MODULE,
	},
};

static int __init init(void)
{
	int ret = -ENODEV;

	if (usb_disabled())
		return ret;

	if (!mod_data.is_high_speed && mod_data.is_super_speed)
		return -EINVAL;

	ret = platform_driver_register(&dummy_hcd_driver);

	return ret;
}
module_init(init);

static void __exit cleanup(void)
{
	platform_driver_unregister(&dummy_hcd_driver);
}
module_exit(cleanup);
