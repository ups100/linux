#include <linux/module.h>
#include <linux/kernel.h>

#include <linux/usb/dummy_usb.h>

MODULE_DESCRIPTION("Driver for virtual udc");
MODULE_AUTHOR("Krzysztof Opasiak");
MODULE_LICENSE("GPL");

static int virt_udc_probe(struct virtual_usb_udc *udc)
{
	/* Set data, check name and do some special things */
	/* TODO create device file here */
	return 0;
}

static int virt_udc_remove(struct virtual_usb_udc *udc)
{
	/* Also nothing special here */
	/* TODO remove device file here */
	return 0;
}

const char *ep_name[] = {
	"ep0",	/* everyone has ep0 */

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

	NULL, /* NULL terminated */
};

static struct virtual_usb_udc_driver virtual_udc_driver = {
	.name = "dummy_udc",
	.module = THIS_MODULE,
	.probe = virt_udc_probe,
	.remove = virt_udc_remove,
	.ep_name = ep_name,
	.ep_nmb = ARRAY_SIZE(ep_name) - 1,
};

/*-------------------------------------------------------------------------*/
static int __init init(void)
{
	int ret = -ENODEV;

	if (usb_disabled())
		return ret;

	ret = virtual_usb_udc_register(&virtual_udc_driver);

	return ret;
}
module_init(init);

static void __exit cleanup(void)
{
	virtual_usb_udc_unregister(&virtual_udc_driver);
}
module_exit(cleanup);
