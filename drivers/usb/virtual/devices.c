#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>

#include <linux/usb/dummy_hcd.h>

#define DRIVER_DESC	"USB Host+Gadget Emulator"
#define DRIVER_VERSION	"02 May 2005"

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

#define MAX_NUM_UDC	2
static struct platform_device *the_udc_pdev[MAX_NUM_UDC];
static struct platform_device *the_hcd_pdev[MAX_NUM_UDC];

static int __init init(void)
{
	int	retval = -ENOMEM;
	int	i;
	struct	dummy_link *dum[MAX_NUM_UDC];

	if (usb_disabled())
		return -ENODEV;

	if (!mod_data.is_high_speed && mod_data.is_super_speed)
		return -EINVAL;

	if (mod_data.num < 1 || mod_data.num > MAX_NUM_UDC) {
		pr_err("Number of emulated UDC must be in range of 1…%d\n",
				MAX_NUM_UDC);
		return -EINVAL;
	}
	for (i = 0; i < mod_data.num; i++) {
		the_udc_pdev[i] = platform_device_alloc(gadget_name, i);
		if (!the_udc_pdev[i]) {
			i--;
			while (i >= 0)
				platform_device_put(the_udc_pdev[i--]);
			return retval;
		}
	}
	for (i = 0; i < mod_data.num; i++) {
		the_hcd_pdev[i] = platform_device_alloc(driver_name, i);
		if (!the_hcd_pdev[i]) {
			i--;
			while (i >= 0)
				platform_device_put(the_hcd_pdev[i--]);
			goto err_alloc_hcd;
		}
	}
	
	for (i = 0; i < mod_data.num; i++) {
		dum[i] = kzalloc(sizeof(struct dummy_link), GFP_KERNEL);
		if (!dum[i]) {
			retval = -ENOMEM;
			goto err_add_pdata;
		}
		retval = platform_device_add_data(the_hcd_pdev[i], &dum[i],
				sizeof(void *));
		if (retval)
			goto err_add_pdata;
		retval = platform_device_add_data(the_udc_pdev[i], &dum[i],
				sizeof(void *));
		if (retval)
			goto err_add_pdata;
	}
	for (i = 0; i < mod_data.num; i++) {
		retval = platform_device_add(the_udc_pdev[i]);
		if (retval < 0) {
			i--;
			while (i >= 0)
				platform_device_del(the_udc_pdev[i]);
			goto err_add_udc;
		}
	}

	for (i = 0; i < mod_data.num; i++) {
		if (!platform_get_drvdata(the_udc_pdev[i])) {
			/*
			 * The udc was added successfully but its probe
			 * function failed for some reason.
			 */
			retval = -EINVAL;
			goto err_add_hcd;
		}
	}

	for (i = 0; i < mod_data.num; i++) {
		retval = platform_device_add(the_hcd_pdev[i]);
		if (retval < 0) {
			i--;
			while (i >= 0)
				platform_device_del(the_hcd_pdev[i--]);
			goto err_add_hcd;
		}
	}
	for (i = 0; i < mod_data.num; i++) {
		if (!dum[i]->host) {
			/*
			 * The hcd was added successfully but its probe
			 * function failed for some reason.
			 */
			retval = -EINVAL;
			goto err_probe_hcd;
		}
	}

	return retval;

err_probe_hcd:
	for (i = 0; i < mod_data.num; i++)
		platform_device_del(the_hcd_pdev[i]);
err_add_hcd:
	for (i = 0; i < mod_data.num; i++)
		platform_device_del(the_udc_pdev[i]);
err_add_udc:
err_add_pdata:
	for (i = 0; i < mod_data.num; i++)
		kfree(dum[i]);
	for (i = 0; i < mod_data.num; i++)
		platform_device_put(the_hcd_pdev[i]);
err_alloc_hcd:
	for (i = 0; i < mod_data.num; i++)
		platform_device_put(the_udc_pdev[i]);
	return retval;
}
module_init(init);

static void __exit cleanup(void)
{
	int i;

	for (i = 0; i < mod_data.num; i++) {
		struct dummy_link *dum;

		dum = *((void **)dev_get_platdata(&the_udc_pdev[i]->dev));

		platform_device_unregister(the_udc_pdev[i]);
		platform_device_unregister(the_hcd_pdev[i]);
		kfree(dum);
	}
}
module_exit(cleanup);
