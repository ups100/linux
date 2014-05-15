#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/types.h>

#define DRIVER_DESC        "USB Device Controler Emulator"
#define DRIVER_VERSION     "13 April 2014"

MODULE_AUTHOR("Krzysztof Opasiak");
MODULE_LICENSE("GPL");

struct dummy_udc_module_parameters {
	bool is_super_speed;
	bool is_high_speed;
	const char *udc_name;
};

static struct dummy_udc_module_parameters mod_data = {
	.is_super_speed = false,
	.is_high_speed = true,
	.udc_name = "dummy_udc",
};

module_param_named(is_super_speed, mod_data.is_super_speed, bool, S_IRUGO);
MODULE_PARM_DESC(is_super_speed, "true to simulate SuperSpeed connection");
module_param_named(is_high_speed, mod_data.is_high_speed, bool, S_IRUGO);
MODULE_PARM_DESC(is_high_speed, "true to simulate HighSpeed connection");
module_param_named(udc_name, mod_data.udc_name, charp, S_IRUGO);
MODULE_PARM_DESC(udc_name, "name for virtual udc");

static int __init init(void)
{
	printk(KERN_INFO "init: %d %d %s", mod_data.is_super_speed,
	       mod_data.is_high_speed, mod_data.udc_name);

	return 0;
}
module_init(init);

static void __exit cleanup(void)
{
	printk(KERN_INFO "exit");
}
module_exit(cleanup);
