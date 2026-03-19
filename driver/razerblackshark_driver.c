
#include <linux/hid.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/usb.h>

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

static ssize_t razer_attr_read_charge_level(struct device *dev,
                                            struct device_attribute *attr,
                                            char *buf) {
  struct razer_blackshark_device *blackshark = dev_get_drvdata(dev);
  return sprintf(buf, "%d\n", blackshark->charge_level);
}

static DEVICE_ATTR(charge_level, 0440, razer_attr_read_charge_level, NULL);

static ssize_t razer_attr_read_charge_status(struct device *dev,
                                             struct device_attribute *attr,
                                             char *buf) {
  struct razer_blackshark_device *blackshark = dev_get_drvdata(dev);

  return sprintf(buf, "%d\n", blackshark->charge_status);
}

static DEVICE_ATTR(charge_status, 0440, razer_attr_read_charge_status, NULL);

static int razer_blackshark_send_sidetone(struct razer_blackshark_device *dev,
		unsigned char level)
{
	unsigned char buf[RAZER_BLACKSHARK_REPORT_LEN];
	unsigned char hw_level;
	unsigned char crc;
	int i, retval;

	hw_level = (unsigned char)(level * 15 / 100);

	memset(buf,0, sizeof(buf));
	buf[0] = 0x02;
	buf[1] = 0x00;
	buf[2] = dev->transaction_id++;
	buf[6] = 0x05;
	buf[9] = 0x80;
	buf[10] = RAZER_BLACKSHARK_SUBCMD_SIDETONE_A;
	buf[12] = 0x01;
	buf[13] = 0x01;

	crc = 0;
	for (i = 0 ; i < 62; i++) {
	crc ^= buf[i];
	}
	buf[62] = crc;

	retval = hid_hw_raw_request(dev->hdev, buf[0], buf,
			RAZER_BLACKSHARK_REPORT_LEN,
			HID_OUTPUT_REPORT, HID_REQ_SET_REPORT);

	if(retval < 0	){
		hid_err(dev->hdev, "razerblackshark: sidetone CMD-A failed (%d)\n", retval);

		return retval;
	}

	usleep_range(RAZER_BLACKSHARK_WAIT_MIN_US, RAZER_BLACKSHARK_WAIT_MAX_US);


	memset(buf, 0, sizeof(buf));
	buf[0] = 0x02;
	buf[2] = dev->transaction_id++;
	buf[6] = 0x05;
	buf[9] = 0x80;
	buf[10] = RAZER_BLACKSHARK_SUBCMD_SIDETONE_B;
	buf[12] = 0x01;
	buf[13] = hw_level;
	
	crc = 0;
	for (i = 0 ; i < 62; i++) {
	crc ^= buf[i];
	}
	buf[62] = crc;

	retval = hid_hw_raw_request(dev->hdev, buf[0], buf,
			RAZER_BLACKSHARK_REPORT_LEN,
			HID_OUTPUT_REPORT, HID_REQ_SET_REPORT);

	if(retval < 0	){
		hid_err(dev->hdev, "razerblackshark: sidetone CMD-B failed (%d)\n", retval);

		return retval;
	}

	return 0;
}

static ssize_t razer_attr_write_sidetone(struct device *dev,
		struct device_attribute *attr, 
		const char *buf, size_t count)
{
	struct razer_blackshark_device *blackshark = dev_get_drvdata(dev);
	unsigned long level;
	int retval;

	if (kstrtoul(buf, 10, &level)) {
	return -EINVAL;
	}

	if (level > 100)
		return -EINVAL;

	mutex_lock(&blackshark->lock);

	retval = razer_blackshark_send_sidetone(blackshark, (unsigned char)level);
	if(retval == 0)
		blackshark->sidetone_level = (unsigned char)level;

	mutex_unlock(&blackshark->lock);

	return retval < 0 ? retval : count;
}

static DEVICE_ATTR(sidetone, 0220, NULL, razer_attr_write_sidetone);

static void razer_blackshark_sidetone_restore_work(struct work_struct *work)
{
	struct razer_blackshark_device *dev = 
		container_of(work, struct razer_blackshark_device, sidetone_restore_work);
	unsigned char level;

	mutex_lock(&dev->lock);
	level = dev->sidetone_level;
	mutex_unlock(&dev->lock);

	if (level > 0)
		razer_blackshark_send_sidetone(dev, level);
}

static int razer_blackshark_raw_event(struct hid_device *hdev,
		struct hid_report *report,
		u8 *data, int size)
{
	struct razer_blackshark_device *dev = hid_get_drvdata(hdev);

	if (size < RAZER_BLACKSHARK_REPORT_LEN)
		return 0;
	if(data[0] != RAZER_BLACKSHARK_PUSH_REPORT_ID)
		return 0;
	if(data[1] != RAZER_BLACKSHARK_PUSH_STATUS ||
			data[2] != RAZER_BLACKSHARK_PUSH_EVENT_ID)
		return 0;

	switch (data[10]) {
		case RAZER_BLACKSHARK_SUBCMD_POWER_STATE:
			dev->is_connected = data[13];

			if (data[13] == 0x01) {
			dev->charge_level = RAZER_BLACKSHARK_BATTERY_UNKNOWN;
			hid_dbg(hdev, "razerblackshark: headset powered ON\n");

			schedule_work(&dev->sidetone_restore_work);
			} else {
				dev->charge_level = RAZER_BLACKSHARK_BATTERY_UNKNOWN;
				dev->charge_status = 0;
				dev->last_unplug_jiffies = RAZER_BLACKSHARK_JIFFIES_INIT;
				hid_dbg(hdev, "razerblackshark: headset powered OFF\n");
			}
			break;
		case RAZER_BLACKSHARK_SUBCMD_BATTERY_LEVEL:
			dev->charge_level = data[13];
			hid_dbg(hdev, "razerblackshark: battery %d%%\n", data[13]);
			break;
		
		case RAZER_BLACKSHARK_SUBCMD_CHARGING_STATUS:
			if (data[13] == 0x00) {
				dev->charge_status = 0;
				dev->last_unplug_jiffies = jiffies;
				hid_dbg(hdev, "razerblackshark: charging disconnected\n");
			} else {
				if (time_before(jiffies, dev->last_unplug_jiffies + 2 * HZ)) {
					hid_dbg(hdev, "razerblackshark: suppressing stale 0x02a/0x01 "
							"after recent unplug\n");
					break;
				}
				dev->charge_status = 1;
				hid_dbg(hdev, "razerblackshark: charging connected\n");
			}
			break;

		default:
			break;
	}

	return 0;
}


static void razer_blackshark_init(struct razer_blackshark_device *dev,
                                  struct hid_device *hdev,
																	struct usb_interface *intf) 
{
  mutex_init(&dev->lock);

	dev->hdev = hdev;
  dev->usb_dev = interface_to_usbdev(intf);
  dev->usb_vid = dev->usb_dev->descriptor.idVendor;
  dev->usb_pid = dev->usb_dev->descriptor.idProduct;

  dev->charge_level = RAZER_BLACKSHARK_BATTERY_UNKNOWN;
  dev->charge_status = 0;
  dev->is_connected = 0;
  dev->sidetone_level = 0;
  dev->transaction_id = 0;
	dev->last_unplug_jiffies = RAZER_BLACKSHARK_JIFFIES_INIT;

	INIT_WORK(&dev->sidetone_restore_work,
			razer_blackshark_sidetone_restore_work);
}

static int razer_blackshark_probe(struct hid_device *hdev,
                                  const struct hid_device_id *id) {
  struct usb_interface *intf = to_usb_interface(hdev->dev.parent);
  struct razer_blackshark_device *dev = NULL;
  int retval = 0;

  if (intf->cur_altsetting->desc.bInterfaceNumber !=
      RAZER_BLACKSHARK_HID_INTERFACE) {
    hid_dbg(hdev, "razerblackshark: skipping interface %d\n",
            intf->cur_altsetting->desc.bInterfaceNumber);
    return -ENODEV;
  }

  hid_info(hdev,
           "razerblackshark: binding to interface %d of %04X:%04X\n",
           RAZER_BLACKSHARK_HID_INTERFACE, hdev->vendor, hdev->product);

  dev = kzalloc(sizeof(struct razer_blackshark_device), GFP_KERNEL);
  if (!dev) {
    return -ENOMEM;
  }

  razer_blackshark_init(dev, hdev, intf);

  hid_set_drvdata(hdev, dev);
  dev_set_drvdata(&hdev->dev, dev);

	CREATE_DEVICE_FILE(&hdev->dev, &dev_attr_version);
	CREATE_DEVICE_FILE(&hdev->dev, &dev_attr_device_type);
	CREATE_DEVICE_FILE(&hdev->dev, &dev_attr_charge_level);
	CREATE_DEVICE_FILE(&hdev->dev, &dev_attr_charge_status);
	CREATE_DEVICE_FILE(&hdev->dev, &dev_attr_sidetone);

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

  usb_disable_autosuspend(dev->usb_dev);


  hid_info(hdev, "razerblackshark: Razer BlackShark V2 HS initialised\n");

  return 0;

exit_free:
  device_remove_file(&hdev->dev, &dev_attr_version);
  device_remove_file(&hdev->dev, &dev_attr_device_type);
  device_remove_file(&hdev->dev, &dev_attr_charge_level);
  device_remove_file(&hdev->dev, &dev_attr_charge_status);
	device_remove_file(&hdev->dev, &dev_attr_sidetone);
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

	cancel_work_sync(&dev->sidetone_restore_work);

  device_remove_file(&hdev->dev, &dev_attr_version);
  device_remove_file(&hdev->dev, &dev_attr_device_type);
  device_remove_file(&hdev->dev, &dev_attr_charge_level);
  device_remove_file(&hdev->dev, &dev_attr_charge_status);
  device_remove_file(&hdev->dev, &dev_attr_sidetone);

  hid_hw_stop(hdev);
  kfree(dev);

  dev_info(&intf->dev,
           "razerblackshark: Razer BlackShark V2 HS disconnected\n");
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
		.raw_event = razer_blackshark_raw_event,
};

module_hid_driver(razer_blackshark_driver);




