#include <linux/module.h>
#include <linux/kernel.h>

#include <linux/usb/dummy_usb.h>

MODULE_DESCRIPTION("Dummy udc creator");
MODULE_AUTHOR("Krzysztof Opasiak");
MODULE_LICENSE("GPL");

static struct virtual_usb_udc *udc;

/*-------------------------------------------------------------------------*/
static int __init init(void)
{
	int ret = -ENODEV;


	if (usb_disabled())
		return ret;

	udc = virtual_usb_alloc_udc("dummy_udc", 0);
	if (!udc || IS_ERR(udc)) {
		printk("Unable to allocate UDC");
		return -1;
	}

	ret = virtual_usb_add_udc(udc);
	if (ret)
		virtual_usb_put_udc(udc);

	return ret;
}
module_init(init);

static void __exit cleanup(void)
{
	virtual_usb_del_udc(udc);
}
module_exit(cleanup);
