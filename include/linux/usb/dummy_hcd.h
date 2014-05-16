#ifndef __LINUX_USB_DUMMY_HCD_H
#define __LINUX_USB_DUMMY_HCD_H

/* ========================przeniesiono ============*/

/* gadget side driver data structres */
struct dummy_ep {
	struct list_head		queue;
	unsigned long			last_io;	/* jiffies timestamp */
	struct usb_gadget		*gadget;
	const struct usb_endpoint_descriptor *desc;
	struct usb_ep			ep;
	unsigned			halted:1;
	unsigned			wedged:1;
	unsigned			already_seen:1;
	unsigned			setup_stage:1;
	unsigned			stream_en:1;
};

struct dummy_request {
	struct list_head		queue;		/* ep's requests */
	struct usb_request		req;
};

inline struct dummy_ep *usb_ep_to_dummy_ep(struct usb_ep *_ep)
{
	return container_of(_ep, struct dummy_ep, ep);
}

inline struct dummy_request *usb_request_to_dummy_request
		(struct usb_request *_req)
{
	return container_of(_req, struct dummy_request, req);
}

const char ep0name[];

const char *const ep_name[];

#define DUMMY_ENDPOINTS	ARRAY_SIZE(ep_name)

/*-------------------------------------------------------------------------*/

#define FIFO_SIZE		64

struct urbp {
	struct urb		*urb;
	struct list_head	urbp_list;
	struct sg_mapping_iter	miter;
	u32			miter_started;
};


enum dummy_rh_state {
	DUMMY_RH_RESET,
	DUMMY_RH_SUSPENDED,
	DUMMY_RH_RUNNING
};

struct dummy_hcd {
	struct dummy			*dum;
	enum dummy_rh_state		rh_state;
	struct timer_list		timer;
	u32				port_status;
	u32				old_status;
	unsigned long			re_timeout;

	struct usb_device		*udev;
	struct list_head		urbp_list;
	u32				stream_en_ep;
	u8				num_stream[30 / 2];

	unsigned			active:1;
	unsigned			old_active:1;
	unsigned			resuming:1;
};

struct dummy {
	spinlock_t			lock;

	/*
	 * SLAVE/GADGET side support
	 */
	struct dummy_ep			ep[DUMMY_ENDPOINTS];
	int				address;
	struct usb_gadget		gadget;
	struct usb_gadget_driver	*driver;
	struct dummy_request		fifo_req;
	u8				fifo_buf[FIFO_SIZE];
	u16				devstatus;
	unsigned			udc_suspended:1;
	unsigned			pullup:1;

	/*
	 * MASTER/HOST side support
	 */
	struct dummy_hcd		*hs_hcd;
	struct dummy_hcd		*ss_hcd;
};

inline struct dummy_hcd *hcd_to_dummy_hcd(struct usb_hcd *hcd)
{
	return (struct dummy_hcd *) (hcd->hcd_priv);
}

inline struct usb_hcd *dummy_hcd_to_hcd(struct dummy_hcd *dum)
{
	return container_of((void *) dum, struct usb_hcd, hcd_priv);
}

inline struct device *dummy_dev(struct dummy_hcd *dum)
{
	return dummy_hcd_to_hcd(dum)->self.controller;
}

inline struct device *udc_dev(struct dummy *dum)
{
	return dum->gadget.dev.parent;
}

inline struct dummy *ep_to_dummy(struct dummy_ep *ep)
{
	return container_of(ep->gadget, struct dummy, gadget);
}

inline struct dummy_hcd *gadget_to_dummy_hcd(struct usb_gadget *gadget)
{
	struct dummy *dum = container_of(gadget, struct dummy, gadget);
	if (dum->gadget.speed == USB_SPEED_SUPER)
		return dum->ss_hcd;
	else
		return dum->hs_hcd;
}

inline struct dummy *gadget_dev_to_dummy(struct device *dev)
{
	return container_of(dev, struct dummy, gadget.dev);
}

/*-------------------------------------------------------------------------*/

#endif
