#include <linux/configfs.h>
#include <linux/module.h>

MODULE_AUTHOR("Krzysztof Opasiak");
MODULE_LICENSE("GPL");

struct virtual_usb_root {
	struct configfs_subsystem subsys;
	struct config_group hub_group;
	struct config_group udc_group;
	struct config_group *default_groups[3];
};

static struct config_group *hubs_make(struct config_group *group,
		const char *name)
{
	return NULL;
}

static void hubs_drop(struct config_group *group, struct config_item *item)
{
	return;
	config_item_put(item);
}

static struct configfs_group_operations hubs_ops = {
	.make_group = &hubs_make,
	.drop_item = &hubs_drop,
};

static struct config_item_type hubs_type = {
	.ct_group_ops = &hubs_ops,
	.ct_owner = THIS_MODULE,
};

static struct config_group *udcs_make(struct config_group *group,
		const char *name)
{
	return NULL;
}

static void udcs_drop(struct config_group *group, struct config_item *item)
{
	return;
	config_item_put(item);
}

static struct configfs_group_operations udcs_ops = {
	.make_group = &udcs_make,
	.drop_item = &udcs_drop,
};

static struct config_item_type udcs_type = {
	.ct_group_ops = &udcs_ops,
	.ct_owner = THIS_MODULE,
};

static struct config_item_type virtual_usb_type = {
	.ct_owner = THIS_MODULE,
};

static struct virtual_usb_root virtual_usb_subsys = {
	.subsys = {
		.su_group = {
			.cg_item = {
				.ci_namebuf = "virtual_usb",
				.ci_type = &virtual_usb_type,
			},
			.default_groups = virtual_usb_subsys.default_groups,
		},
		.su_mutex = __MUTEX_INITIALIZER(virtual_usb_subsys.subsys.su_mutex),
	},

	.hub_group = {
		.cg_item = {
			.ci_namebuf = "hub",
			.ci_type = &hubs_type,
		},
	},
	.udc_group = {
		.cg_item = {
			.ci_namebuf = "udc",
			.ci_type = &udcs_type,
		},
	},
	.default_groups = {
		&virtual_usb_subsys.hub_group,
		&virtual_usb_subsys.udc_group,
		NULL
	},
};

static int __init init(void)
{
	int ret;

	config_group_init(&virtual_usb_subsys.subsys.su_group);
	config_group_init(&virtual_usb_subsys.hub_group);
	config_group_init(&virtual_usb_subsys.udc_group);

	ret = configfs_register_subsystem(&virtual_usb_subsys.subsys);
	printk("REJESTRACJA SUBSYSTEMU: %d\n", ret);
	return ret;
}
module_init(init);

static void __exit cleanup(void)
{
	configfs_unregister_subsystem(&virtual_usb_subsys.subsys);
}
module_exit(cleanup);
