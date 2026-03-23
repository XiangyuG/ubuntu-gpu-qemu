// SPDX-License-Identifier: GPL-2.0
#include <linux/device.h>
#include "rvt2_drv.h"

static ssize_t status_show(struct device *dev,
                           struct device_attribute *attr, char *buf)
{
    struct rvt2_device *rdev = dev_get_drvdata(dev);
    u32 status = rvt2_read(rdev, RVT2_REG_STATUS);

    if (status & RVT2_STATUS_ERROR)
        return sysfs_emit(buf, "ERROR\n");
    if (status & RVT2_STATUS_READY)
        return sysfs_emit(buf, "OK\n");
    return sysfs_emit(buf, "INIT\n");
}
static DEVICE_ATTR_RO(status);

static ssize_t fw_version_show(struct device *dev,
                               struct device_attribute *attr, char *buf)
{
    struct rvt2_device *rdev = dev_get_drvdata(dev);
    return sysfs_emit(buf, "0x%04x\n", rdev->fw_version);
}
static DEVICE_ATTR_RO(fw_version);

static ssize_t engine_count_show(struct device *dev,
                                 struct device_attribute *attr, char *buf)
{
    struct rvt2_device *rdev = dev_get_drvdata(dev);
    return sysfs_emit(buf, "%u\n", rdev->engine_count);
}
static DEVICE_ATTR_RO(engine_count);

static struct attribute *rvt2_attrs[] = {
    &dev_attr_status.attr,
    &dev_attr_fw_version.attr,
    &dev_attr_engine_count.attr,
    NULL,
};

static const struct attribute_group rvt2_attr_group = {
    .attrs = rvt2_attrs,
};

int rvt2_sysfs_init(struct rvt2_device *rdev)
{
    return sysfs_create_group(&rdev->pdev->dev.kobj, &rvt2_attr_group);
}

void rvt2_sysfs_fini(struct rvt2_device *rdev)
{
    sysfs_remove_group(&rdev->pdev->dev.kobj, &rvt2_attr_group);
}
