/*
 * Android Composite USB Driver (Linux 4.4 clean port for LineageOS / Android 10)
 *
 * Provides /sys/class/android_usb/android0 sysfs interface expected by legacy
 * Android userspace (init.qcom.usb.rc and init.usb.rc) while using modern
 * Linux 4.4 libcomposite function drivers (FunctionFS, MTP, PTP, Mass Storage,
 * Accessory, Audio Source, MIDI, RNDIS).
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/utsname.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>

#include <linux/usb/ch9.h>
#include <linux/usb/composite.h>
#include <linux/usb/gadget.h>

#include "function/u_fs.h"

#define DRIVER_NAME "android_usb"
#define DRIVER_DESC "Android Composite USB Driver"

#define VENDOR_ID  0x05C6
#define PRODUCT_ID 0x9020

struct android_dev {
	struct usb_composite_dev *cdev;
	struct device *dev;

	struct kobject *kobj_ffs;
	struct kobject *kobj_rndis;
	struct kobject *kobj_mass_storage;
	struct kobject *kobj_diag;
	struct kobject *kobj_serial;

	bool enabled;
	bool connected;
	struct mutex mutex;
	struct work_struct work;

	u16 idVendor;
	u16 idProduct;
	u8  bDeviceClass;
	u8  bDeviceSubClass;
	u8  bDeviceProtocol;

	char manufacturer[256];
	char product[256];
	char serial[256];
	char functions_str[256];
	char ffs_aliases[256];

	/* Function instances */
	struct usb_function_instance *fi_ffs;
	struct usb_function *f_ffs;

	struct usb_function_instance *fi_mtp;
	struct usb_function *f_mtp;

	struct usb_function_instance *fi_ptp;
	struct usb_function *f_ptp;

	struct usb_function_instance *fi_ms;
	struct usb_function *f_ms;

	struct usb_function_instance *fi_acc;
	struct usb_function *f_acc;

	struct usb_function_instance *fi_audio;
	struct usb_function *f_audio;

	struct usb_function_instance *fi_midi;
	struct usb_function *f_midi;

	struct usb_function_instance *fi_rndis;
	struct usb_function *f_rndis;
};

extern struct class *android_class;
static struct android_dev *g_android_dev;

static struct usb_device_descriptor android_device_desc = {
	.bLength            = sizeof(android_device_desc),
	.bDescriptorType    = USB_DT_DEVICE,
	.bcdUSB             = cpu_to_le16(0x0200),
	.bDeviceClass       = USB_CLASS_PER_INTERFACE,
	.idVendor           = cpu_to_le16(VENDOR_ID),
	.idProduct          = cpu_to_le16(PRODUCT_ID),
	.bcdDevice          = cpu_to_le16(0x0404),
	.bNumConfigurations = 1,
};

static struct usb_string android_strings_dev[] = {
	[USB_GADGET_MANUFACTURER_IDX].s = "OPPO",
	[USB_GADGET_PRODUCT_IDX].s      = "A37f",
	[USB_GADGET_SERIAL_IDX].s       = "d443a155",
	{ }
};

static struct usb_gadget_strings android_stringtab_dev = {
	.language = 0x0409,
	.strings  = android_strings_dev,
};

static struct usb_gadget_strings *android_dev_strings[] = {
	&android_stringtab_dev,
	NULL,
};

static struct usb_configuration android_config_driver = {
	.label          = "android",
	.bConfigurationValue = 1,
	.bmAttributes   = USB_CONFIG_ATT_ONE | USB_CONFIG_ATT_SELFPOWER,
	.MaxPower       = 500, /* 500mA */
};

/* Forward declarations */
static int android_bind_config(struct usb_configuration *c);

static void android_work(struct work_struct *data)
{
	struct android_dev *dev = container_of(data, struct android_dev, work);
	char *disconnected[2] = { "USB_STATE=DISCONNECTED", NULL };
	char *connected[2]    = { "USB_STATE=CONNECTED", NULL };
	char *configured[2]   = { "USB_STATE=CONFIGURED", NULL };
	char **uevent_envp = NULL;
	unsigned long flags;

	if (!dev || !dev->dev || !dev->cdev || !dev->cdev->gadget)
		return;

	spin_lock_irqsave(&dev->cdev->lock, flags);
	if (dev->cdev->config)
		uevent_envp = configured;
	else if (dev->connected)
		uevent_envp = connected;
	else
		uevent_envp = disconnected;
	spin_unlock_irqrestore(&dev->cdev->lock, flags);

	if (uevent_envp) {
		kobject_uevent_env(&dev->dev->kobj, KOBJ_CHANGE, uevent_envp);
		pr_info("%s: sent uevent %s\n", __func__, uevent_envp[0]);
	}
}

/* --------------------------------------------------------------------------
 * Function setup and binding
 * -------------------------------------------------------------------------- */

static int android_bind_config(struct usb_configuration *c)
{
	struct android_dev *dev = g_android_dev;
	char *buf, *name;
	int ret = 0;

	if (!dev)
		return -ENODEV;

	buf = kstrdup(dev->functions_str, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	name = buf;
	while (name) {
		char *next = strchr(name, ',');
		if (next)
			*next++ = '\0';

		name = strim(name);

		if (!strcmp(name, "adb") || !strcmp(name, "ffs")) {
			if (!dev->fi_ffs)
				dev->fi_ffs = usb_get_function_instance("ffs");
			if (!IS_ERR_OR_NULL(dev->fi_ffs)) {
				dev->f_ffs = usb_get_function(dev->fi_ffs);
				if (!IS_ERR_OR_NULL(dev->f_ffs)) {
					ret = usb_add_function(c, dev->f_ffs);
					if (ret)
						pr_err("android_usb: add ffs failed %d\n", ret);
				}
			}
		} else if (!strcmp(name, "mtp")) {
			if (!dev->fi_mtp)
				dev->fi_mtp = usb_get_function_instance("mtp");
			if (!IS_ERR_OR_NULL(dev->fi_mtp)) {
				dev->f_mtp = usb_get_function(dev->fi_mtp);
				if (!IS_ERR_OR_NULL(dev->f_mtp))
					usb_add_function(c, dev->f_mtp);
			}
		} else if (!strcmp(name, "ptp")) {
			if (!dev->fi_ptp)
				dev->fi_ptp = usb_get_function_instance("ptp");
			if (!IS_ERR_OR_NULL(dev->fi_ptp)) {
				dev->f_ptp = usb_get_function(dev->fi_ptp);
				if (!IS_ERR_OR_NULL(dev->f_ptp))
					usb_add_function(c, dev->f_ptp);
			}
		} else if (!strcmp(name, "mass_storage")) {
			if (!dev->fi_ms)
				dev->fi_ms = usb_get_function_instance("mass_storage");
			if (!IS_ERR_OR_NULL(dev->fi_ms)) {
				dev->f_ms = usb_get_function(dev->fi_ms);
				if (!IS_ERR_OR_NULL(dev->f_ms))
					usb_add_function(c, dev->f_ms);
			}
		} else if (!strcmp(name, "accessory")) {
			if (!dev->fi_acc)
				dev->fi_acc = usb_get_function_instance("accessory");
			if (!IS_ERR_OR_NULL(dev->fi_acc)) {
				dev->f_acc = usb_get_function(dev->fi_acc);
				if (!IS_ERR_OR_NULL(dev->f_acc))
					usb_add_function(c, dev->f_acc);
			}
		} else if (!strcmp(name, "audio_source")) {
			if (!dev->fi_audio)
				dev->fi_audio = usb_get_function_instance("audio_source");
			if (!IS_ERR_OR_NULL(dev->fi_audio)) {
				dev->f_audio = usb_get_function(dev->fi_audio);
				if (!IS_ERR_OR_NULL(dev->f_audio))
					usb_add_function(c, dev->f_audio);
			}
		} else if (!strcmp(name, "midi")) {
			if (!dev->fi_midi)
				dev->fi_midi = usb_get_function_instance("midi");
			if (!IS_ERR_OR_NULL(dev->fi_midi)) {
				dev->f_midi = usb_get_function(dev->fi_midi);
				if (!IS_ERR_OR_NULL(dev->f_midi))
					usb_add_function(c, dev->f_midi);
			}
		} else if (!strcmp(name, "rndis")) {
			if (!dev->fi_rndis)
				dev->fi_rndis = usb_get_function_instance("rndis");
			if (!IS_ERR_OR_NULL(dev->fi_rndis)) {
				dev->f_rndis = usb_get_function(dev->fi_rndis);
				if (!IS_ERR_OR_NULL(dev->f_rndis))
					usb_add_function(c, dev->f_rndis);
			}
		}

		name = next;
	}

	kfree(buf);
	return 0;
}

static void android_clean_functions(struct android_dev *dev)
{
	if (dev->f_ffs) {
		usb_remove_function(&android_config_driver, dev->f_ffs);
		usb_put_function(dev->f_ffs);
		dev->f_ffs = NULL;
	}
	if (dev->f_mtp) {
		usb_remove_function(&android_config_driver, dev->f_mtp);
		usb_put_function(dev->f_mtp);
		dev->f_mtp = NULL;
	}
	if (dev->f_ptp) {
		usb_remove_function(&android_config_driver, dev->f_ptp);
		usb_put_function(dev->f_ptp);
		dev->f_ptp = NULL;
	}
	if (dev->f_ms) {
		usb_remove_function(&android_config_driver, dev->f_ms);
		usb_put_function(dev->f_ms);
		dev->f_ms = NULL;
	}
	if (dev->f_acc) {
		usb_remove_function(&android_config_driver, dev->f_acc);
		usb_put_function(dev->f_acc);
		dev->f_acc = NULL;
	}
	if (dev->f_audio) {
		usb_remove_function(&android_config_driver, dev->f_audio);
		usb_put_function(dev->f_audio);
		dev->f_audio = NULL;
	}
	if (dev->f_midi) {
		usb_remove_function(&android_config_driver, dev->f_midi);
		usb_put_function(dev->f_midi);
		dev->f_midi = NULL;
	}
	if (dev->f_rndis) {
		usb_remove_function(&android_config_driver, dev->f_rndis);
		usb_put_function(dev->f_rndis);
		dev->f_rndis = NULL;
	}
}

static int android_enable(struct android_dev *dev)
{
	struct usb_composite_dev *cdev = dev->cdev;
	int ret;

	if (dev->enabled)
		return 0;

	if (!cdev || !cdev->gadget)
		return -ENODEV;

	pr_info("android_usb: enabling '%s', VID=0x%04x PID=0x%04x\n",
		dev->functions_str, dev->idVendor, dev->idProduct);

	android_device_desc.idVendor = cpu_to_le16(dev->idVendor);
	android_device_desc.idProduct = cpu_to_le16(dev->idProduct);
	android_device_desc.bDeviceClass = dev->bDeviceClass;
	android_device_desc.bDeviceSubClass = dev->bDeviceSubClass;
	android_device_desc.bDeviceProtocol = dev->bDeviceProtocol;

	cdev->desc.idVendor = cpu_to_le16(dev->idVendor);
	cdev->desc.idProduct = cpu_to_le16(dev->idProduct);
	cdev->desc.bDeviceClass = dev->bDeviceClass;
	cdev->desc.bDeviceSubClass = dev->bDeviceSubClass;
	cdev->desc.bDeviceProtocol = dev->bDeviceProtocol;

	ret = usb_add_config(cdev, &android_config_driver, android_bind_config);
	if (ret) {
		pr_err("android_usb: usb_add_config failed: %d\n", ret);
		return ret;
	}

	usb_gadget_connect(cdev->gadget);
	dev->enabled = true;
	dev->connected = true;

	schedule_work(&dev->work);
	return 0;
}

static void android_disable(struct android_dev *dev)
{
	struct usb_composite_dev *cdev = dev->cdev;

	if (!dev->enabled)
		return;

	pr_info("android_usb: disabling\n");

	if (cdev && cdev->gadget)
		usb_gadget_disconnect(cdev->gadget);

	android_clean_functions(dev);

	dev->enabled = false;
	dev->connected = false;

	schedule_work(&dev->work);
}

/* --------------------------------------------------------------------------
 * Sysfs Attributes
 * -------------------------------------------------------------------------- */

static ssize_t enable_show(struct device *pdev, struct device_attribute *attr, char *buf)
{
	struct android_dev *dev = dev_get_drvdata(pdev);
	return scnprintf(buf, PAGE_SIZE, "%d\n", dev->enabled ? 1 : 0);
}

static ssize_t enable_store(struct device *pdev, struct device_attribute *attr, const char *buf, size_t size)
{
	struct android_dev *dev = dev_get_drvdata(pdev);
	int enabled = 0;

	if (kstrtoint(buf, 10, &enabled))
		return -EINVAL;

	mutex_lock(&dev->mutex);
	if (enabled)
		android_enable(dev);
	else
		android_disable(dev);
	mutex_unlock(&dev->mutex);

	return size;
}

static ssize_t functions_show(struct device *pdev, struct device_attribute *attr, char *buf)
{
	struct android_dev *dev = dev_get_drvdata(pdev);
	return scnprintf(buf, PAGE_SIZE, "%s\n", dev->functions_str);
}

static ssize_t functions_store(struct device *pdev, struct device_attribute *attr, const char *buf, size_t size)
{
	struct android_dev *dev = dev_get_drvdata(pdev);

	mutex_lock(&dev->mutex);
	if (dev->enabled) {
		mutex_unlock(&dev->mutex);
		return -EBUSY;
	}

	strlcpy(dev->functions_str, buf, sizeof(dev->functions_str));
	strim(dev->functions_str);
	mutex_unlock(&dev->mutex);

	return size;
}

static ssize_t idVendor_show(struct device *pdev, struct device_attribute *attr, char *buf)
{
	struct android_dev *dev = dev_get_drvdata(pdev);
	return scnprintf(buf, PAGE_SIZE, "%04x\n", dev->idVendor);
}

static ssize_t idVendor_store(struct device *pdev, struct device_attribute *attr, const char *buf, size_t size)
{
	struct android_dev *dev = dev_get_drvdata(pdev);
	u16 vid;

	if (kstrtou16(buf, 16, &vid))
		return -EINVAL;

	dev->idVendor = vid;
	return size;
}

static ssize_t idProduct_show(struct device *pdev, struct device_attribute *attr, char *buf)
{
	struct android_dev *dev = dev_get_drvdata(pdev);
	return scnprintf(buf, PAGE_SIZE, "%04x\n", dev->idProduct);
}

static ssize_t idProduct_store(struct device *pdev, struct device_attribute *attr, const char *buf, size_t size)
{
	struct android_dev *dev = dev_get_drvdata(pdev);
	u16 pid;

	if (kstrtou16(buf, 16, &pid))
		return -EINVAL;

	dev->idProduct = pid;
	return size;
}

static ssize_t bDeviceClass_show(struct device *pdev, struct device_attribute *attr, char *buf)
{
	struct android_dev *dev = dev_get_drvdata(pdev);
	return scnprintf(buf, PAGE_SIZE, "%d\n", dev->bDeviceClass);
}

static ssize_t bDeviceClass_store(struct device *pdev, struct device_attribute *attr, const char *buf, size_t size)
{
	struct android_dev *dev = dev_get_drvdata(pdev);
	u8 val;
	if (kstrtou8(buf, 0, &val))
		return -EINVAL;
	dev->bDeviceClass = val;
	return size;
}

static ssize_t bDeviceSubClass_show(struct device *pdev, struct device_attribute *attr, char *buf)
{
	struct android_dev *dev = dev_get_drvdata(pdev);
	return scnprintf(buf, PAGE_SIZE, "%d\n", dev->bDeviceSubClass);
}

static ssize_t bDeviceSubClass_store(struct device *pdev, struct device_attribute *attr, const char *buf, size_t size)
{
	struct android_dev *dev = dev_get_drvdata(pdev);
	u8 val;
	if (kstrtou8(buf, 0, &val))
		return -EINVAL;
	dev->bDeviceSubClass = val;
	return size;
}

static ssize_t bDeviceProtocol_show(struct device *pdev, struct device_attribute *attr, char *buf)
{
	struct android_dev *dev = dev_get_drvdata(pdev);
	return scnprintf(buf, PAGE_SIZE, "%d\n", dev->bDeviceProtocol);
}

static ssize_t bDeviceProtocol_store(struct device *pdev, struct device_attribute *attr, const char *buf, size_t size)
{
	struct android_dev *dev = dev_get_drvdata(pdev);
	u8 val;
	if (kstrtou8(buf, 0, &val))
		return -EINVAL;
	dev->bDeviceProtocol = val;
	return size;
}

static ssize_t iManufacturer_show(struct device *pdev, struct device_attribute *attr, char *buf)
{
	struct android_dev *dev = dev_get_drvdata(pdev);
	return scnprintf(buf, PAGE_SIZE, "%s\n", dev->manufacturer);
}

static ssize_t iManufacturer_store(struct device *pdev, struct device_attribute *attr, const char *buf, size_t size)
{
	struct android_dev *dev = dev_get_drvdata(pdev);
	strlcpy(dev->manufacturer, buf, sizeof(dev->manufacturer));
	strim(dev->manufacturer);
	android_strings_dev[USB_GADGET_MANUFACTURER_IDX].s = dev->manufacturer;
	return size;
}

static ssize_t iProduct_show(struct device *pdev, struct device_attribute *attr, char *buf)
{
	struct android_dev *dev = dev_get_drvdata(pdev);
	return scnprintf(buf, PAGE_SIZE, "%s\n", dev->product);
}

static ssize_t iProduct_store(struct device *pdev, struct device_attribute *attr, const char *buf, size_t size)
{
	struct android_dev *dev = dev_get_drvdata(pdev);
	strlcpy(dev->product, buf, sizeof(dev->product));
	strim(dev->product);
	android_strings_dev[USB_GADGET_PRODUCT_IDX].s = dev->product;
	return size;
}

static ssize_t iSerial_show(struct device *pdev, struct device_attribute *attr, char *buf)
{
	struct android_dev *dev = dev_get_drvdata(pdev);
	return scnprintf(buf, PAGE_SIZE, "%s\n", dev->serial);
}

static ssize_t iSerial_store(struct device *pdev, struct device_attribute *attr, const char *buf, size_t size)
{
	struct android_dev *dev = dev_get_drvdata(pdev);
	strlcpy(dev->serial, buf, sizeof(dev->serial));
	strim(dev->serial);
	android_strings_dev[USB_GADGET_SERIAL_IDX].s = dev->serial;
	return size;
}

static ssize_t state_show(struct device *pdev, struct device_attribute *attr, char *buf)
{
	struct android_dev *dev = dev_get_drvdata(pdev);
	const char *state = "DISCONNECTED";

	if (dev->cdev && dev->cdev->config)
		state = "CONFIGURED";
	else if (dev->connected)
		state = "CONNECTED";

	return scnprintf(buf, PAGE_SIZE, "%s\n", state);
}

static DEVICE_ATTR_RW(enable);
static DEVICE_ATTR_RW(functions);
static DEVICE_ATTR_RW(idVendor);
static DEVICE_ATTR_RW(idProduct);
static DEVICE_ATTR_RW(bDeviceClass);
static DEVICE_ATTR_RW(bDeviceSubClass);
static DEVICE_ATTR_RW(bDeviceProtocol);
static DEVICE_ATTR_RW(iManufacturer);
static DEVICE_ATTR_RW(iProduct);
static DEVICE_ATTR_RW(iSerial);
static DEVICE_ATTR_RO(state);

static struct attribute *android_usb_attrs[] = {
	&dev_attr_enable.attr,
	&dev_attr_functions.attr,
	&dev_attr_idVendor.attr,
	&dev_attr_idProduct.attr,
	&dev_attr_bDeviceClass.attr,
	&dev_attr_bDeviceSubClass.attr,
	&dev_attr_bDeviceProtocol.attr,
	&dev_attr_iManufacturer.attr,
	&dev_attr_iProduct.attr,
	&dev_attr_iSerial.attr,
	&dev_attr_state.attr,
	NULL,
};

static const struct attribute_group android_usb_attr_group = {
	.attrs = android_usb_attrs,
};

/* --------------------------------------------------------------------------
 * Subdirectory attributes (f_ffs, f_rndis, f_mass_storage, f_diag, f_serial)
 * -------------------------------------------------------------------------- */

static ssize_t ffs_aliases_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct android_dev *dev = g_android_dev;
	return scnprintf(buf, PAGE_SIZE, "%s\n", dev->ffs_aliases);
}

static ssize_t ffs_aliases_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t size)
{
	struct android_dev *dev = g_android_dev;
	strlcpy(dev->ffs_aliases, buf, sizeof(dev->ffs_aliases));
	strim(dev->ffs_aliases);

	/* If aliases specifies adb, pre-register with ffs */
	if (strstr(dev->ffs_aliases, "adb")) {
		if (!dev->fi_ffs)
			dev->fi_ffs = usb_get_function_instance("ffs");
		if (!IS_ERR_OR_NULL(dev->fi_ffs)) {
			struct f_fs_opts *opts = to_f_fs_opts(dev->fi_ffs);
			ffs_name_dev(opts->dev, "adb");
			opts->no_configfs = true;
			pr_info("android_usb: registered ffs 'adb' device alias\n");
		}
	}

	return size;
}

static struct kobj_attribute ffs_aliases_attr = __ATTR(aliases, 0660, ffs_aliases_show, ffs_aliases_store);

static struct attribute *ffs_attrs[] = {
	&ffs_aliases_attr.attr,
	NULL,
};

static const struct attribute_group ffs_attr_group = {
	.attrs = ffs_attrs,
};

static ssize_t rndis_wceis_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "1\n");
}
static ssize_t rndis_wceis_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t size)
{
	return size;
}
static struct kobj_attribute rndis_wceis_attr = __ATTR(wceis, 0660, rndis_wceis_show, rndis_wceis_store);

static ssize_t rndis_dummy_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "\n");
}
static ssize_t rndis_dummy_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t size)
{
	return size;
}
static struct kobj_attribute rndis_ethaddr_attr = __ATTR(ethaddr, 0660, rndis_dummy_show, rndis_dummy_store);
static struct kobj_attribute rndis_vendor_attr = __ATTR(vendorID, 0660, rndis_dummy_show, rndis_dummy_store);
static struct kobj_attribute rndis_mfr_attr = __ATTR(manufacturer, 0660, rndis_dummy_show, rndis_dummy_store);

static struct attribute *rndis_attrs[] = {
	&rndis_wceis_attr.attr,
	&rndis_ethaddr_attr.attr,
	&rndis_vendor_attr.attr,
	&rndis_mfr_attr.attr,
	NULL,
};
static const struct attribute_group rndis_attr_group = {
	.attrs = rndis_attrs,
};

static struct kobj_attribute ms_file_attr = __ATTR(file, 0660, rndis_dummy_show, rndis_dummy_store);
static struct attribute *ms_attrs[] = {
	&ms_file_attr.attr,
	NULL,
};
static const struct attribute_group ms_attr_group = {
	.attrs = ms_attrs,
};

static struct kobj_attribute diag_clients_attr = __ATTR(clients, 0660, rndis_dummy_show, rndis_dummy_store);
static struct attribute *diag_attrs[] = {
	&diag_clients_attr.attr,
	NULL,
};
static const struct attribute_group diag_attr_group = {
	.attrs = diag_attrs,
};

static struct kobj_attribute serial_transports_attr = __ATTR(transports, 0660, rndis_dummy_show, rndis_dummy_store);
static struct attribute *serial_attrs[] = {
	&serial_transports_attr.attr,
	NULL,
};
static const struct attribute_group serial_attr_group = {
	.attrs = serial_attrs,
};

/* --------------------------------------------------------------------------
 * Composite Driver Bind & Unbind
 * -------------------------------------------------------------------------- */

static int android_bind(struct usb_composite_dev *cdev)
{
	struct android_dev *dev = g_android_dev;
	struct usb_gadget *gadget = cdev->gadget;
	int ret;

	pr_info("android_usb: composite bind with gadget %s\n", gadget->name);

	dev->cdev = cdev;

	/* Allocate string IDs */
	ret = usb_string_ids_tab(cdev, android_strings_dev);
	if (ret < 0)
		return ret;

	android_device_desc.iManufacturer = android_strings_dev[USB_GADGET_MANUFACTURER_IDX].id;
	android_device_desc.iProduct      = android_strings_dev[USB_GADGET_PRODUCT_IDX].id;
	android_device_desc.iSerialNumber = android_strings_dev[USB_GADGET_SERIAL_IDX].id;

	/* Pre-allocate FunctionFS for adb */
	if (!dev->fi_ffs)
		dev->fi_ffs = usb_get_function_instance("ffs");
	if (!IS_ERR_OR_NULL(dev->fi_ffs)) {
		struct f_fs_opts *opts = to_f_fs_opts(dev->fi_ffs);
		ffs_name_dev(opts->dev, "adb");
		opts->no_configfs = true;
		pr_info("android_usb: FunctionFS 'adb' device pre-registered\n");
	}

	return 0;
}

static int android_unbind(struct usb_composite_dev *cdev)
{
	struct android_dev *dev = g_android_dev;

	pr_info("android_usb: composite unbind\n");

	if (dev) {
		android_disable(dev);
		dev->cdev = NULL;
	}
	return 0;
}

static void android_disconnect(struct usb_composite_dev *cdev)
{
	struct android_dev *dev = g_android_dev;

	pr_info("android_usb: disconnect\n");
	if (dev) {
		dev->connected = false;
		schedule_work(&dev->work);
	}
}

static struct usb_composite_driver android_usb_driver = {
	.name       = DRIVER_NAME,
	.dev        = &android_device_desc,
	.strings    = android_dev_strings,
	.bind       = android_bind,
	.unbind     = android_unbind,
	.disconnect = android_disconnect,
	.max_speed  = USB_SPEED_HIGH,
};

/* --------------------------------------------------------------------------
 * Module Init & Exit
 * -------------------------------------------------------------------------- */

static int android_usb_probe(struct platform_device *pdev)
{
	struct android_dev *dev;
	int ret;

	pr_info("android_usb: probing\n");

	if (g_android_dev)
		return 0;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	g_android_dev = dev;
	mutex_init(&dev->mutex);
	INIT_WORK(&dev->work, android_work);

	dev->idVendor = VENDOR_ID;
	dev->idProduct = PRODUCT_ID;
	strlcpy(dev->manufacturer, "OPPO", sizeof(dev->manufacturer));
	strlcpy(dev->product, "A37f", sizeof(dev->product));
	strlcpy(dev->serial, "d443a155", sizeof(dev->serial));
	strlcpy(dev->functions_str, "adb", sizeof(dev->functions_str));
	strlcpy(dev->ffs_aliases, "adb", sizeof(dev->ffs_aliases));

	/* Create or reuse android_class */
	if (!android_class) {
		android_class = class_create(THIS_MODULE, "android_usb");
		if (IS_ERR(android_class)) {
			ret = PTR_ERR(android_class);
			pr_err("android_usb: failed to create android_class: %d\n", ret);
			android_class = NULL;
			g_android_dev = NULL;
			kfree(dev);
			return ret;
		}
	}

	dev->dev = device_create(android_class, &pdev->dev, MKDEV(0, 0), dev, "android0");
	if (IS_ERR(dev->dev)) {
		ret = PTR_ERR(dev->dev);
		pr_err("android_usb: failed to create android0: %d\n", ret);
		dev->dev = NULL;
		g_android_dev = NULL;
		kfree(dev);
		return ret;
	}

	ret = sysfs_create_group(&dev->dev->kobj, &android_usb_attr_group);
	if (ret) {
		device_destroy(android_class, MKDEV(0, 0));
		dev->dev = NULL;
		g_android_dev = NULL;
		kfree(dev);
		return ret;
	}

	/* Create subdirectories expected by init.qcom.usb.rc and init.usb.rc */
	dev->kobj_ffs = kobject_create_and_add("f_ffs", &dev->dev->kobj);
	if (dev->kobj_ffs) {
		ret = sysfs_create_group(dev->kobj_ffs, &ffs_attr_group);
		(void)ret;
	}

	dev->kobj_rndis = kobject_create_and_add("f_rndis", &dev->dev->kobj);
	if (dev->kobj_rndis) {
		ret = sysfs_create_group(dev->kobj_rndis, &rndis_attr_group);
		(void)ret;
	}

	dev->kobj_mass_storage = kobject_create_and_add("f_mass_storage", &dev->dev->kobj);
	if (dev->kobj_mass_storage) {
		struct kobject *lun = kobject_create_and_add("lun", dev->kobj_mass_storage);
		if (lun) {
			ret = sysfs_create_group(lun, &ms_attr_group);
			(void)ret;
		}
	}

	dev->kobj_diag = kobject_create_and_add("f_diag", &dev->dev->kobj);
	if (dev->kobj_diag) {
		ret = sysfs_create_group(dev->kobj_diag, &diag_attr_group);
		(void)ret;
	}

	dev->kobj_serial = kobject_create_and_add("f_serial", &dev->dev->kobj);
	if (dev->kobj_serial) {
		ret = sysfs_create_group(dev->kobj_serial, &serial_attr_group);
		(void)ret;
	}

	/* Register composite driver - if UDC is not ready yet, defer! */
	ret = usb_composite_probe(&android_usb_driver);
	if (ret) {
		if (ret == -ENODEV) {
			pr_info("android_usb: UDC not ready yet, deferring probe\n");
			ret = -EPROBE_DEFER;
		} else {
			pr_err("android_usb: usb_composite_probe failed: %d\n", ret);
		}
		device_destroy(android_class, MKDEV(0, 0));
		dev->dev = NULL;
		g_android_dev = NULL;
		kfree(dev);
		return ret;
	}

	platform_set_drvdata(pdev, dev);
	pr_info("android_usb: registered successfully\n");
	return 0;
}

static int android_usb_remove(struct platform_device *pdev)
{
	struct android_dev *dev = platform_get_drvdata(pdev);

	if (!dev)
		return 0;

	usb_composite_unregister(&android_usb_driver);
	if (android_class && dev->dev)
		device_destroy(android_class, MKDEV(0, 0));
	g_android_dev = NULL;
	kfree(dev);
	return 0;
}

static const struct of_device_id android_usb_dt_match[] = {
	{ .compatible = "qcom,android-usb", },
	{ }
};
MODULE_DEVICE_TABLE(of, android_usb_dt_match);

static struct platform_driver android_usb_platform_driver = {
	.driver = {
		.name = "android_usb",
		.of_match_table = android_usb_dt_match,
	},
	.probe = android_usb_probe,
	.remove = android_usb_remove,
};

static int __init android_usb_init(void)
{
	return platform_driver_register(&android_usb_platform_driver);
}
late_initcall(android_usb_init);

static void __exit android_usb_exit(void)
{
	platform_driver_unregister(&android_usb_platform_driver);
}
module_exit(android_usb_exit);

MODULE_AUTHOR("Google / LineageOS Port");
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");
