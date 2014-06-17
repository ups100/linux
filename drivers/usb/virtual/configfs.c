#include <linux/configfs.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/list.h>

#include <linux/usb/dummy_usb.h>

MODULE_AUTHOR("Krzysztof Opasiak");
MODULE_LICENSE("GPL");

#define MAX_NAME_LEN 40

struct virt_hcd {
	struct mutex lock;
	struct virtual_usb_hcd *hcd;
	struct list_head list;
	int enable;
	struct config_group group;
	int counter;
};

struct virt_udc {
	struct mutex lock;
	struct virtual_usb_udc *udc;
	struct list_head list;
	int enable;
	struct config_group group;
	int counter;
};

struct virtual_usb_info {
	struct mutex lock;
	struct list_head hcd_list;
	struct list_head udc_list;
};

struct virtual_usb_root {
	struct configfs_subsystem subsys;
	struct config_group hcd_group;
	struct config_group udc_group;
	struct config_group *default_groups[3];

	struct virtual_usb_info devices;
};

CONFIGFS_ATTR_STRUCT(virt_hcd);
CONFIGFS_ATTR_STRUCT(virt_udc);

#define group_to_vi(type, ptr) \
	(&container_of(ptr, struct virtual_usb_root, type##_group)->devices)


static inline struct virt_udc * to_virt_udc(struct config_item *item)
{
	return container_of(to_config_group(item) , struct virt_udc, group);
}

static inline struct virt_hcd *to_virt_hcd(struct config_item *item)
{
	return container_of(to_config_group(item) , struct virt_hcd, group);
}

/* HCD part */

#define VIRT_HCD_ATTR(name) \
	static struct virt_hcd_attribute single_hcd_attr_##name = \
		__CONFIGFS_ATTR(name,  S_IRUGO | S_IWUSR, \
	single_hcd_attr_##name##_show,			  \
				single_hcd_attr_##name##_store)

static ssize_t single_hcd_attr_enable_show(struct virt_hcd *vh, char *page)
{
	return sprintf(page, "%d", vh->enable);
}

static ssize_t single_hcd_attr_enable_store(struct virt_hcd *vh, const char *page, size_t len)
{
	u8 val;
	int ret;

	ret = kstrtou8(page, 0, &val);
	if (ret)
		return ret;

	if (val < 0 || val > 1)
		return -EINVAL;

	mutex_lock(&vh->lock);
	if (val == vh->enable) {
		ret = -EINVAL;
		goto out;
	}
	
	if (vh->counter > 1) {
		ret = -ENODEV;
		goto out;
	}

	vh->counter++;
	vh->enable = val;
	if (vh->enable) {
		/* We should turn on our device */
		ret = virtual_usb_add_hcd(vh->hcd);
		if (ret)
			vh->enable = 0;
	} else {
		/* We should turn off our device */
		virtual_usb_del_hcd(vh->hcd);
	}

	ret = len;

out:
	mutex_unlock(&vh->lock);
	return ret;
}

VIRT_HCD_ATTR(enable);

static struct configfs_attribute *virtual_hcd_attrs[] = {
	&single_hcd_attr_enable.attr,
	NULL,
};

static void virt_hcd_attr_release(struct config_item *item)
{
	struct virt_hcd *vh;
       
	vh = to_virt_hcd(item);
	if (vh->enable)
		virtual_usb_del_hcd(vh->hcd);

	virtual_usb_put_hcd(vh->hcd);
	kfree(vh);
}

CONFIGFS_ATTR_OPS(virt_hcd);

static struct configfs_item_operations virt_hcd_item_ops = {
	.release = virt_hcd_attr_release,
	.show_attribute = virt_hcd_attr_show,
	.store_attribute = virt_hcd_attr_store,
};

static struct config_item_type hcd_root_type = {
	.ct_item_ops = &virt_hcd_item_ops,
	.ct_attrs = virtual_hcd_attrs,
	.ct_owner = THIS_MODULE,
};

static struct config_group *hcds_make(struct config_group *group,
		const char *name)
{
	struct virtual_usb_info *vi;
	struct virt_hcd *vh;
	struct virtual_usb_hcd *hcd;
	int ret;
	u8 id;
	char buf[MAX_NAME_LEN];
	char *udc_name, *id_str;

	vi = group_to_vi(hcd, group);

	ret = snprintf(buf, sizeof(buf), "%s", name);
	if (ret >= sizeof(buf))
		return ERR_PTR(-ENAMETOOLONG);

	udc_name = buf;
	id_str = strrchr(udc_name, '.');
	if (!id_str) {
		pr_err("Unable to locate . in DEVICE.ID\n");
		return ERR_PTR(-EINVAL);
	}
	*id_str = '\0';
	++id_str;

	if (id_str - buf == sizeof(*buf)) {
		pr_err("Unable to locate ID\n");
		return ERR_PTR(-EINVAL);
	}

	if (!strlen(id_str))
		return ERR_PTR(-EINVAL);

	ret = kstrtou8(id_str, 0, &id);
	if (ret)
		return ERR_PTR(ret);
	
	vh = kzalloc(sizeof(*vh), GFP_KERNEL);
	if (!vh)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&vh->list);
	mutex_init(&vh->lock);
	vh->enable = 0;
	vh->counter = 0;

	hcd = virtual_usb_alloc_hcd(buf, id);
	if (!hcd || IS_ERR(hcd)) {
		pr_err("Unable to create such udc\n");
		return ERR_CAST(hcd);
	}
	vh->hcd = hcd;

	config_group_init_type_name(&vh->group, name, &hcd_root_type);
	
	/* put it now on the list */
	mutex_lock(&vi->lock);
	list_add_tail(&vh->list, &vi->hcd_list);
	mutex_unlock(&vi->lock);

	printk("Wrzucile vu z enable %d", vh->enable);
	return &vh->group;
}

static void hcds_drop(struct config_group *group, struct config_item *item)
{
	struct virtual_usb_info *vi;
	struct virt_hcd *vh;

	vi = group_to_vi(hcd, group);
	vh = to_virt_hcd(item);

	mutex_lock(&vi->lock);
	list_del(&vh->list);
	mutex_unlock(&vi->lock);
	
	config_item_put(item);
}

static struct configfs_group_operations hcd_part_ops = {
	.make_group = &hcds_make,
	.drop_item = &hcds_drop,
};

static struct config_item_type hcd_part_type = {
	.ct_group_ops = &hcd_part_ops,
	.ct_owner = THIS_MODULE,
};

/* UDC part */

#define VIRT_UDC_ATTR(name) \
	static struct virt_udc_attribute single_udc_attr_##name = \
		__CONFIGFS_ATTR(name,  S_IRUGO | S_IWUSR, \
	single_udc_attr_##name##_show,			  \
				single_udc_attr_##name##_store)

static ssize_t single_udc_attr_enable_show(struct virt_udc *vu, char *page)
{
	return sprintf(page, "%d", vu->enable);
}

static ssize_t single_udc_attr_enable_store(struct virt_udc *vu, const char *page, size_t len)
{
	u8 val;
	int ret;

	ret = kstrtou8(page, 0, &val);
	if (ret)
		return ret;

	if (val < 0 || val > 1)
		return -EINVAL;

	mutex_lock(&vu->lock);
	if (val == vu->enable) {
		ret = -EINVAL;
		goto out;
	}
	
	if (vu->counter > 1) {
		ret = -ENODEV;
		goto out;
	}

	vu->counter++;
	vu->enable = val;
	if (vu->enable) {
		/* We should turn on our device */
		ret = virtual_usb_add_udc(vu->udc);
		if (ret)
			vu->enable = 0;
	} else {
		/* We should turn off our device */
		virtual_usb_del_udc(vu->udc);
	}

	ret = len;

out:
	mutex_unlock(&vu->lock);
	return ret;
}

VIRT_UDC_ATTR(enable);

static struct configfs_attribute *virtual_udc_attrs[] = {
	&single_udc_attr_enable.attr,
	NULL,
};

static void virt_udc_attr_release(struct config_item *item)
{
	struct virt_udc *vu;
       
	vu = to_virt_udc(item);
	if (vu->enable)
		virtual_usb_del_udc(vu->udc);

	virtual_usb_put_udc(vu->udc);
	kfree(vu);
}

CONFIGFS_ATTR_OPS(virt_udc);

static struct configfs_item_operations virt_udc_item_ops = {
	.release = virt_udc_attr_release,
	.show_attribute = virt_udc_attr_show,
	.store_attribute = virt_udc_attr_store,
};

static struct config_item_type udc_root_type = {
	.ct_item_ops = &virt_udc_item_ops,
	.ct_attrs = virtual_udc_attrs,
	.ct_owner = THIS_MODULE,
};

static struct config_group *udcs_make(struct config_group *group,
		const char *name)
{
	struct virtual_usb_info *vi;
	struct virt_udc *vu;
	struct virtual_usb_udc *udc;
	int ret;
	u8 id;
	char buf[MAX_NAME_LEN];
	char *udc_name, *id_str;

	vi = group_to_vi(udc, group);

	ret = snprintf(buf, sizeof(buf), "%s", name);
	if (ret >= sizeof(buf))
		return ERR_PTR(-ENAMETOOLONG);

	udc_name = buf;
	id_str = strrchr(udc_name, '.');
	if (!id_str) {
		pr_err("Unable to locate . in DEVICE.ID\n");
		return ERR_PTR(-EINVAL);
	}
	*id_str = '\0';
	++id_str;

	if (id_str - buf == sizeof(*buf)) {
		pr_err("Unable to locate ID\n");
		return ERR_PTR(-EINVAL);
	}

	if (!strlen(id_str))
		return ERR_PTR(-EINVAL);

	ret = kstrtou8(id_str, 0, &id);
	if (ret)
		return ERR_PTR(ret);
	
	vu = kzalloc(sizeof(*vu), GFP_KERNEL);
	if (!vu)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&vu->list);
	mutex_init(&vu->lock);
	vu->enable = 0;
	vu->counter = 0;

	udc = virtual_usb_alloc_udc(buf, id);
	if (!udc || IS_ERR(udc)) {
		pr_err("Unable to create such udc\n");
		return ERR_CAST(udc);
	}
	vu->udc = udc;

	config_group_init_type_name(&vu->group, name, &udc_root_type);
	
	/* put it now on the list */
	mutex_lock(&vi->lock);
	list_add_tail(&vu->list, &vi->udc_list);
	mutex_unlock(&vi->lock);

	printk("Wrzucile vu z enable %d", vu->enable);
	return &vu->group;
}

static void udcs_drop(struct config_group *group, struct config_item *item)
{
	struct virtual_usb_info *vi;
	struct virt_udc *vu;

	vi = group_to_vi(udc, group);
	vu = to_virt_udc(item);

	mutex_lock(&vi->lock);
	list_del(&vu->list);
	mutex_unlock(&vi->lock);
	
	config_item_put(item);
}

static struct configfs_group_operations udc_part_ops = {
	.make_group = &udcs_make,
	.drop_item = &udcs_drop,
};

static struct config_item_type udc_part_type = {
	.ct_group_ops = &udc_part_ops,
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

	.hcd_group = {
		.cg_item = {
			.ci_namebuf = "hcd",
			.ci_type = &hcd_part_type,
		},
	},


	.udc_group = {
		.cg_item = {
			.ci_namebuf = "udc",
			.ci_type = &udc_part_type,
		},
	},


	.default_groups = {
		&virtual_usb_subsys.udc_group,
		&virtual_usb_subsys.hcd_group,
		NULL
	},

	.devices = {
		.lock = __MUTEX_INITIALIZER(virtual_usb_subsys.devices.lock),
		.udc_list = LIST_HEAD_INIT(virtual_usb_subsys.devices.udc_list),
		.hcd_list = LIST_HEAD_INIT(virtual_usb_subsys.devices.hcd_list),
	},
};

static int __init init(void)
{
	int ret;

	config_group_init(&virtual_usb_subsys.subsys.su_group);
	config_group_init(&virtual_usb_subsys.hcd_group);
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
