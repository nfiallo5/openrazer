
#include <linux/hid.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/usb/input.h>

#include "razerblackshark_driver.h"
#include "razercommon.h"

#define DRIVER_DESC "Razer BlackShark V2 HyperSpeed Device Driver"

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE(DRIVER_LICENSE);

static ssize_t razer_attr_read_device_type(struct device *dev,
                                           struct device_attribute *attr,
                                           char *buf) {
  return sprintf(buf, "Razer BlackShark V2 HS\n");
}

static DEVICE_ATTR(device_type, 0440, razer_attr_read_device_type, NULL);

static ssize_t razer_attr_read_version(struct device *dev,
                                       struct device_attribute *attr,
                                       char *buf) {
  return sprintf(buf, "%s\n", DRIVER_VERSION);
}

static DEVICE_ATTR(version, 0440, razer_attr_read_version, NULL);

static void razer_blackshark_init(struct razer_blackshark_device *dev,
                                  struct usb_interface *intf) {
  struct usb_device *usb_dev = interface_to_usbdev(intf);

  mutex_init(&dev->lock);

  dev->usb_dev = usb_dev;
  dev->usb_vid = usb_dev->descriptor.idVendor;
  dev->usb_pid = usb_dev->descriptor.idProduct;

  dev->charge_level = RAZER_BLACKSHARK_BATTERY_UNKNOWN;
  dev->charge_status = 0;
  dev->is_connected = 0;
  dev->sidetone_level = 0;
  dev->transaction_id = 0;
}

static int razer_blackshark_probe(struct hid_device *hdev,
                                  const struct hid_device_id *id) {
  struct usb_interface *intf = to_usb_interface(hdev->dev.parent);
  struct usb_device *usb_dev = interface_to_usbdev(intf);
  struct razer_blackshark_device *dev = NULL;
  int retval = 0;

  if (intf->cur_altsetting->desc.bInterfaceNumber !=
      RAZER_BLACKSHARK_HID_INTERFACE) {
    dev_dbg(&intf->dev, "razerblackshark: skipping interface %d\n",
            intf->cur_altsetting->desc.bInterfaceNumber);
    return -ENODEV;
  }

  dev_info(&intf->dev,
           "razerblackshark: binding to interface %d of %04X:%04X\n",
           RAZER_BLACKSHARK_HID_INTERFACE, usb_dev->descriptor.idVendor,
           usb_dev->descriptor.idProduct);

  dev = kzalloc(sizeof(struct razer_blackshark_device), GFP_KERNEL);
  if (!dev) {
    dev_err(&intf->dev, "razerblackshark: out of memory\n");
    return -ENOMEM;
  }

  razer_blackshark_init(dev, intf);

  CREATE_DEVICE_FILE(&hdev->dev, &dev_attr_version);
  CREATE_DEVICE_FILE(&hdev->dev, &dev_attr_device_type);

  hid_set_drvdata(hdev, dev);
  dev_set_drvdata(&hdev->dev, dev);

  if (hid_parse(hdev)) {
    hid_err(hdev, "razerblackshark: hid_parse false\n");
    retval = -ENODEV;
    goto exit_free;
  }

  if (hid_hw_start(hdev, HID_CONNECT_DEFAULT)) {
    hid_err(hdev, "razerblackshark: hid_hw_start failed\n");
    retval = -ENODEV;
    goto exit_free;
  }

  usb_disable_autosuspend(usb_dev);

  dev_info(&intf->dev, "razerblackshark: Razer BlackShark V2 HS initialised\n");

  return 0;

exit_free:
  device_remove_file(&hdev->dev, &dev_attr_version);
  device_remove_file(&hdev->dev, &dev_attr_device_type);
  kfree(dev);
  return retval;
}

// disconnect
static void razer_blackshark_disconnect(struct hid_device *hdev) {
  struct razer_blackshark_device *dev = hid_get_drvdata(hdev);
  struct usb_interface *intf = to_usb_interface(hdev->dev.parent);

  if (intf->cur_altsetting->desc.bInterfaceNumber !=
      RAZER_BLACKSHARK_HID_INTERFACE) {
    return;
  }

  device_remove_file(&hdev->dev, &dev_attr_version);
  device_remove_file(&hdev->dev, &dev_attr_device_type);

  hid_hw_stop(hdev);
  kfree(dev);

  dev_info(&intf->dev, "razerblackshark: Razer BlackShark v2 HS disconected\n");
};

static const struct hid_device_id razer_blackshark_devices[] = {
    {HID_USB_DEVICE(USB_VENDOR_ID_RAZER, USB_DEVICE_ID_RAZER_BLACKSHARK_V2_HS)},
    {0}};

MODULE_DEVICE_TABLE(hid, razer_blackshark_devices);

static struct hid_driver razer_blackshark_driver = {
    .name = "razerblackshark",
    .id_table = razer_blackshark_devices,
    .probe = razer_blackshark_probe,
    .remove = razer_blackshark_disconnect,
};

module_hid_driver(razer_blackshark_driver);
