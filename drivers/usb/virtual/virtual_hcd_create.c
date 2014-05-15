#include <linux/module.h>
#include <linux/kernel.h>

#include <linux/usb/dummy_usb.h>

MODULE_DESCRIPTION("Dummy hcd creator");
MODULE_AUTHOR("Krzysztof Opasiak");
MODULE_LICENSE("GPL");

static struct virtual_usb_hcd *hcd;

/*-------------------------------------------------------------------------*/
static int __init init(void)
{
	int ret = -ENODEV;

		printk("Begin");

	if (usb_disabled())
		return ret;

	hcd = virtual_usb_alloc_hcd("dummy_hcd", 0);
	if (!hcd || IS_ERR(hcd)) {
		printk("Unable to allocate HCD");
		return -1;
	}
	printk("przed Add");
	ret = virtual_usb_add_hcd(hcd);
	if (ret)
		virtual_usb_put_hcd(hcd);

	return ret;
}
module_init(init);

static void __exit cleanup(void)
{
	virtual_usb_rm_hcd(hcd);
}
module_exit(cleanup);
