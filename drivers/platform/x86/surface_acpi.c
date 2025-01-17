/*
 *  surface_acpi.c - Microsoft Surface ACPI Notify
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  The full GNU General Public License is included in this distribution in
 *  the file called "COPYING".
 */

#define SURFACE_ACPI_VERSION	"0.1"
#define SURFACE_GEN_VERSION		0x08
#define PROC_SURFACE			"surface"

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/acpi.h>
#include <linux/power_supply.h>
#include <linux/thermal.h>
#include <linux/dmi.h>
#include <linux/seq_file.h>
#include <acpi/acpi_bus.h>
#include <acpi/acpi_drivers.h>

MODULE_AUTHOR("Jake Day");
MODULE_DESCRIPTION("Microsoft Surface ACPI Notify Driver");
MODULE_LICENSE("GPL");

#define SUR_METHOD_DSM			"_DSM"
#define SUR_METHOD_REG			"_REG"
#define SUR_METHOD_STA			"_STA"
#define SUR_METHOD_INI			"_INI"
#define SUR_METHOD_CRS			"_CRS"

#define SUR_QUERY_DEVICE		0x00
#define SUR_SET_DVER			0x01
#define SUR_GET_BOARD_REVID		0x02
#define SUR_BAT1_STATE_CHANGE	0x03
#define SUR_BAT1_INFO_CHANGE	0x04
#define SUR_PSU_STATE_CHANGE	0x05
#define SUR_PSU_INFO_CHANGE		0x06
#define SUR_BAT2_STATE_CHANGE	0x07
#define SUR_BAT2_INFO_CHANGE	0x08
#define SUR_SENSOR_TRIP_POINT	0x09

#define REG_AVAILABLE			0x01
#define REG_INIT				0x09

static char SURFACE_EVENT_GUID[] = "93b666c5-70c6-469f-a215-3d487c91ab3c";
static char SUR_SAN_RQST[] = "\\_SB._SAN.RQST";
static char SUR_SAN_RQSX[] = "\\_SB._SAN.RQSX";

struct surface_acpi_dev {
	acpi_handle handle;
	acpi_handle rqst_handle;
	acpi_handle rqsx_handle;

	struct acpi_device *san_dev;
	struct acpi_device *ssh_dev;
	struct acpi_device *bat1_dev;
	struct acpi_device *bat2_dev;
	struct acpi_device *psu_dev;

	unsigned int bat1_attached:1;
	unsigned int bat2_attached:1;
	unsigned int psu_registered:1;
};

static struct surface_acpi_dev *surface_acpi;

static struct proc_dir_entry *surface_proc_dir;

static acpi_status surface_acpi_check_status(struct acpi_device *dev)
{
	unsigned long long value;
	acpi_status status;

	if (acpi_has_method(dev->handle, SUR_METHOD_STA)) {
		status = acpi_evaluate_integer(dev->handle,
				SUR_METHOD_STA, NULL, &value);

		if (ACPI_FAILURE(status)) {
			pr_err("surface_acpi: ACPI event failure status %s\n",
					acpi_format_exception(status));
			return AE_ERROR;
		}
	}
	else
		return AE_NOT_FOUND;

	return AE_OK;
}

static acpi_status surface_acpi_san_reg(void)
{
	union acpi_object in_objs[2], out_objs[1];
	struct acpi_object_list params;
	struct acpi_buffer results;
	acpi_status status;

	params.count = ARRAY_SIZE(in_objs);
	params.pointer = in_objs;
	in_objs[0].type = ACPI_TYPE_INTEGER;
	in_objs[0].integer.value = REG_INIT;
	in_objs[1].type = ACPI_TYPE_INTEGER;
	in_objs[1].integer.value = REG_AVAILABLE;
	results.length = sizeof(out_objs);
	results.pointer = out_objs;

	if (acpi_has_method(surface_acpi->handle, SUR_METHOD_REG)) {
		status = acpi_evaluate_object(surface_acpi->handle,
				SUR_METHOD_REG, &params, &results);

		if (ACPI_FAILURE(status)) {
			pr_err("surface_acpi: ACPI event failure status %s\n",
					acpi_format_exception(status));
			return AE_ERROR;
		}
	}
	else
		return AE_NOT_FOUND;

	return AE_OK;
}

static acpi_status surface_acpi_event_handler(u32 event)
{
	union acpi_object in_objs[4], out_objs[5];
	struct acpi_object_list params;
	struct acpi_buffer results;
	acpi_status status;

	params.count = ARRAY_SIZE(in_objs);
	params.pointer = in_objs;
	in_objs[0].type = ACPI_TYPE_BUFFER;
	in_objs[0].buffer.length = sizeof(SURFACE_EVENT_GUID);
	in_objs[0].buffer.pointer = SURFACE_EVENT_GUID;
	in_objs[1].type = ACPI_TYPE_INTEGER;
	in_objs[1].integer.value = SUR_QUERY_DEVICE;
	in_objs[2].type = ACPI_TYPE_INTEGER;
	in_objs[2].integer.value = event;
	in_objs[3].type = ACPI_TYPE_PACKAGE;
	in_objs[3].package.count = 0;
	in_objs[3].package.elements = SURFACE_GEN_VERSION;
	results.length = sizeof(out_objs);
	results.pointer = out_objs;

	if (acpi_has_method(surface_acpi->handle, SUR_METHOD_DSM)) {
		status = acpi_evaluate_object(surface_acpi->handle,
				SUR_METHOD_DSM, &params, &results);

		if (ACPI_FAILURE(status)) {
			pr_err("surface_acpi: ACPI event failure status %s\n",
					acpi_format_exception(status));
			return AE_ERROR;
		}
	}
	else
		return AE_NOT_FOUND;

	return AE_OK;
}

static void surface_acpi_san_load(void)
{
	acpi_status ret;

	ret = surface_acpi_event_handler(SUR_SET_DVER);
	if (ACPI_FAILURE(ret))
		pr_err("surface_acpi: Error setting Driver Version\n");

	ret = surface_acpi_event_handler(SUR_SENSOR_TRIP_POINT);
	if (ACPI_FAILURE(ret))
		pr_err("surface_acpi: Error setting Sensor Trip Point\n");

	ret = surface_acpi_event_handler(SUR_BAT1_INFO_CHANGE);
	if (ACPI_FAILURE(ret))
		pr_err("surface_acpi: Error attaching BAT1\n");
	else
		surface_acpi->bat1_attached = 1;

	ret = surface_acpi_event_handler(SUR_BAT2_INFO_CHANGE);
	if (ACPI_FAILURE(ret))
		pr_err("surface_acpi: Error attaching BAT2\n");
	else
		surface_acpi->bat2_attached = 1;

	ret = surface_acpi_event_handler(SUR_PSU_INFO_CHANGE);
	if (ACPI_FAILURE(ret))
		pr_err("surface_acpi: Error registering PSU\n");
	else
		surface_acpi->psu_registered = 1;
}

static acpi_status surface_acpi_ssh_initialize(void)
{
	acpi_status status;

	if (acpi_has_method(surface_acpi->ssh_dev->handle, SUR_METHOD_INI)) {
		status = acpi_evaluate_object(surface_acpi->ssh_dev->handle,
				SUR_METHOD_INI, NULL, NULL);

		if (ACPI_FAILURE(status)) {
			pr_err("surface_acpi: ACPI event failure status %s\n",
					acpi_format_exception(status));
			return AE_ERROR;
		}
	}
	else
		return AE_NOT_FOUND;

	return AE_OK;
}

static int bat1_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "attached: %d\n", surface_acpi->bat1_attached);
	return 0;
}

static int bat1_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, bat1_proc_show, PDE_DATA(inode));
}

static const struct file_operations bat1_proc_fops = {
	.owner		= THIS_MODULE,
	.open		= bat1_proc_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int bat2_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "attached: %d\n", surface_acpi->bat2_attached);
	return 0;
}

static int bat2_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, bat2_proc_show, PDE_DATA(inode));
}

static const struct file_operations bat2_proc_fops = {
	.owner		= THIS_MODULE,
	.open		= bat2_proc_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int psu_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "registered: %d\n", surface_acpi->psu_registered);
	return 0;
}

static int psu_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, psu_proc_show, PDE_DATA(inode));
}

static const struct file_operations psu_proc_fops = {
	.owner		= THIS_MODULE,
	.open		= psu_proc_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int version_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "driver: %s\n", SURFACE_ACPI_VERSION);
	return 0;
}

static int version_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, version_proc_show, PDE_DATA(inode));
}

static const struct file_operations version_proc_fops = {
	.owner		= THIS_MODULE,
	.open		= version_proc_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static void create_surface_proc_entries(void)
{
	proc_create_data("BAT1", 0, surface_proc_dir,
			 &bat1_proc_fops, surface_acpi->bat1_attached);
	proc_create_data("BAT2", 0, surface_proc_dir,
			 &bat2_proc_fops, surface_acpi->bat2_attached);
	proc_create_data("ADP1", 0, surface_proc_dir,
			 &psu_proc_fops, surface_acpi->psu_registered);
	proc_create_data("version", 0, surface_proc_dir,
			 &version_proc_fops, SURFACE_ACPI_VERSION);
}

static void remove_surface_proc_entries(void)
{
	remove_proc_entry("BAT1", surface_proc_dir);
	remove_proc_entry("BAT2", surface_proc_dir);
	remove_proc_entry("ADP1", surface_proc_dir);
	remove_proc_entry("version", surface_proc_dir);
}

static void surface_acpi_notify(struct acpi_device *dev, u32 event)
{
	pr_info("surface_acpi: Event received %x\n", event);
}

static void surface_acpi_register_rqst_handler(void)
{
	acpi_status status;

	status = acpi_get_handle(NULL, SUR_SAN_RQST, &surface_acpi->rqst_handle);
	if (ACPI_FAILURE(status)) {
		pr_err("surface_acpi: ACPI event failure status %s\n",
					acpi_format_exception(status));
	}
}

static void surface_acpi_register_rqsx_handler(void)
{
	acpi_status status;

	status = acpi_get_handle(NULL, SUR_SAN_RQSX, &surface_acpi->rqsx_handle);
	if (ACPI_FAILURE(status)) {
		pr_err("surface_acpi: ACPI event failure status %s\n",
					acpi_format_exception(status));
	}
}

static acpi_status surface_acpi_walk_callback(acpi_handle handle, u32 level,
						void *context, void **return_value)
{
	struct acpi_device_info *info;

	if (ACPI_SUCCESS(acpi_get_object_info(handle, &info))) {
		pr_warn("method: name: %4.4s, args %X\n",
			(char *)&info->name, info->param_count);

		kfree(info);
	}

	return AE_OK;
}

static void surface_acpi_walk_namespace(struct acpi_device *dev)
{
	acpi_status status;

	status = acpi_walk_namespace(ACPI_TYPE_METHOD,
			dev->handle, 1, surface_acpi_walk_callback,
			NULL, NULL, NULL);
	if (ACPI_FAILURE(status))
		pr_warn("surface_acpi: Unable to walk acpi resources\n");
}

static int surface_acpi_add(struct acpi_device *dev)
{
	if (!surface_acpi)
	{
		surface_acpi = kzalloc(sizeof(*surface_acpi), GFP_KERNEL);
		if (!surface_acpi)
			return AE_NO_MEMORY;
	}

	if (acpi_has_method(dev->handle, SUR_METHOD_DSM))
	{
		pr_info("surface_acpi: Attaching device MSHW0091\n");

		surface_acpi->san_dev = dev;
		surface_acpi->handle = dev->handle;

		surface_acpi_walk_namespace(surface_acpi->san_dev);
		surface_acpi_check_status(surface_acpi->san_dev);

		surface_acpi_register_rqst_handler();
		surface_acpi_register_rqsx_handler();

		surface_acpi_san_reg();
		surface_acpi_san_load();

		create_surface_proc_entries();
	}
	else
	{
		pr_info("surface_acpi: Attaching device MSHW0084\n");

		surface_acpi->ssh_dev = dev;

		surface_acpi_walk_namespace(surface_acpi->ssh_dev);
		surface_acpi_check_status(surface_acpi->ssh_dev);

		surface_acpi_ssh_initialize();
		//surface_acpi_ssh_load();
	}

	return AE_OK;
}

static int surface_acpi_remove(struct acpi_device *dev)
{
	remove_surface_proc_entries();

	return AE_OK;
}

static const struct acpi_device_id surface_device_ids[] = {
	{"MSHW0091", 0},
	{"MSHW0084", 0},
	{"", 0},
};
MODULE_DEVICE_TABLE(acpi, surface_device_ids);

static struct acpi_driver surface_acpi_driver = {
	.name	= "surface_acpi",
	.owner	= THIS_MODULE,
	.ids	= surface_device_ids,
	.flags	= ACPI_DRIVER_ALL_NOTIFY_EVENTS,
	.ops	= {
		.add	= surface_acpi_add,
		.remove = surface_acpi_remove,
		.notify = surface_acpi_notify,
	},
};

static int __init surface_acpi_init(void)
{
	int ret;

	pr_info("surface_acpi: Microsoft Surface ACPI Notify version %s\n",
	       SURFACE_ACPI_VERSION);

	surface_proc_dir = proc_mkdir(PROC_SURFACE, acpi_root_dir);
	if (!surface_proc_dir) {
		pr_err("surface_acpi: Unable to create proc dir " PROC_SURFACE "\n");
		return -ENODEV;
	}

	ret = acpi_bus_register_driver(&surface_acpi_driver);
	if (ret) {
		pr_err("surface_acpi: Failed to register ACPI driver: %d\n", ret);
		remove_proc_entry(PROC_SURFACE, acpi_root_dir);
	}

	return ret;
}

static void __exit surface_acpi_exit(void)
{
	acpi_bus_unregister_driver(&surface_acpi_driver);
	if (surface_proc_dir)
		remove_proc_entry(PROC_SURFACE, acpi_root_dir);
}

module_init(surface_acpi_init);
module_exit(surface_acpi_exit);
