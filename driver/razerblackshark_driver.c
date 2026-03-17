
#include <linux/hid.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/usb.h>
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
	
// CMD-A: slot selector
	memset(buf, 0, sizeof(buf));
	buf[0] = 0x02;										// report ID
	buf[1] = 0x00;										// host->device direction
	buf[2] = dev->transaction_id++;   // sequence counter
	buf[3] = 0x00;										// always 0x00
	buf[6] = 0x05;										// data_size
	buf[9] = 0x80;										// sidetone command class 
	buf[10] = RAZER_BLACKSHARK_SUBCMD_SIDETONE_A;
	buf[12] = 0x01;										// required
	buf[13] = 0x01;										// slot selector


	crc = 0;
	for (i = 0; i < 62 ; i++) {
		crc ^= buf[i];
	}
	buf[62] = crc;

	retval = usb_control_msg(dev->usb_dev,
			usb_sndctrlpipe(dev->usb_dev, 0),
			HID_REQ_SET_REPORT,
			USB_TYPE_CLASS | USB_RECIP_INTERFACE | USB_DIR_OUT,
			RAZER_BLACKSHARK_REPORT_TYPE,
			RAZER_BLACKSHARK_REPORT_INDEX,
			buf, RAZER_BLACKSHARK_REPORT_LEN,
			USB_CTRL_SET_TIMEOUT);

	if (retval < 0 ) {
	dev_err(&dev->usb_dev->dev,
			"razerblackshark: sidetone CMD-A failed (%d)\n", retval);
	return retval;
	}

	usleep_range(RAZER_BLACKSHARK_WAIT_MIN_US, RAZER_BLACKSHARK_WAIT_MAX_US);

// CMD-B: level
	memset(buf, 0, sizeof(buf));
	
	buf[0] = 0x02;
	buf[1] = 0x00;
	buf[2] = dev->transaction_id++;
	buf[3] = 0x00;
	buf[6] = 0x05;
	buf[9] = 0x80;
	buf[10] = RAZER_BLACKSHARK_SUBCMD_SIDETONE_B;
	buf[12] = 0x01;
	buf[13] = hw_level;

	crc = 0;
	for (i = 0; i < 62; i++) 
		crc ^= buf[i];
	buf[62] = crc;

	retval = usb_control_msg(dev->usb_dev,
			usb_sndctrlpipe(dev->usb_dev, 0),
			HID_REQ_SET_REPORT,
			USB_TYPE_CLASS | USB_RECIP_INTERFACE | USB_DIR_OUT,
			RAZER_BLACKSHARK_REPORT_TYPE,
			RAZER_BLACKSHARK_REPORT_INDEX,
			buf, RAZER_BLACKSHARK_REPORT_LEN,
			USB_CTRL_SET_TIMEOUT);

	if (retval < 0) {
	dev_err(&dev->usb_dev->dev,
			"razerblackshark: sidetone CMD-B failed (%d)\n", retval);
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

	if (level > 100) {
	return -EINVAL;
	}

	mutex_lock(&blackshark->lock);

	retval = razer_blackshark_send_sidetone(blackshark, (unsigned char)level);
	if (retval == 0)
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

	if (level > 0 )
		razer_blackshark_send_sidetone(dev, level);
	mutex_unlock(&dev->lock);

}

static void razer_blackshark_irq_callback(struct urb *urb) {
  struct razer_blackshark_device *dev = urb->context;
  unsigned char *buf = urb->transfer_buffer;
  int retval;

  switch (urb->status) {
  case 0:
    break;
  case -ENOENT:
  case -ECONNRESET:
  case -ESHUTDOWN:
    return;
  default:
    dev_dbg(&urb->dev->dev, "razer blackshark: irq status %d\n", urb->status);
    goto resubmit;
  }

  if (urb->actual_length < RAZER_BLACKSHARK_REPORT_LEN) {
    dev_dbg(&urb->dev->dev,
            "razerblackshark: short packet (%d bytes), discarding\n",
            urb->actual_length);
    goto resubmit;
  }

  if (buf[1] != RAZER_BLACKSHARK_PUSH_STATUS ||
      buf[2] != RAZER_BLACKSHARK_PUSH_EVENT_ID) {
    goto resubmit;
  }

  switch (buf[10]) {
  case RAZER_BLACKSHARK_SUBCMD_POWER_STATE:
    dev->is_connected = buf[13];

    if (buf[13] == 0x01) {
      dev->charge_level = RAZER_BLACKSHARK_BATTERY_UNKNOWN;

      dev_dbg(&urb->dev->dev, "razerblackshark: headset powered ON\n");

			schedule_work(&dev->sidetone_restore_work);
    } else {
      dev->charge_level = RAZER_BLACKSHARK_BATTERY_UNKNOWN;
      dev->charge_status = 0;

			dev->last_unplug_jiffies = RAZER_BLACKSHARK_JIFFIES_INIT;
      dev_dbg(&urb->dev->dev, "razerblackshark: headset powered OFF\n");
    }
    break;

  case RAZER_BLACKSHARK_SUBCMD_BATTERY_LEVEL:
    dev->charge_level = buf[13];

    dev_dbg(&urb->dev->dev, "razerblackshark: battery %d%%\n", buf[13]);
    break;

  case RAZER_BLACKSHARK_SUBCMD_CHARGING_STATUS:
    if (buf[13] == 0x00) {
      dev->charge_status = 0;
      dev->last_unplug_jiffies = jiffies;

      dev_dbg(&urb->dev->dev, "razerblackshark: charging disconnected\n");
    } else {
      if (time_before(jiffies, dev->last_unplug_jiffies + 2 * HZ)) {
        dev_dbg(&urb->dev->dev, "razerblackshark: suppressing stale 0x2a/0x01 "
                                "after recent unplug\n");
        break;
      }

      dev->charge_status = 1;

      dev_dbg(&urb->dev->dev, "razerblackshark: charging connected\n");
    }
    break;

  default:
    break;
  }
resubmit:
  retval = usb_submit_urb(urb, GFP_ATOMIC);
  if (retval && retval != -ENODEV) {
    dev_err(&urb->dev->dev, "razerblackshark: urb re-submit failed (%d)\n",
            retval);
  }
}

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
	dev->last_unplug_jiffies = RAZER_BLACKSHARK_JIFFIES_INIT;

	INIT_WORK(&dev->sidetone_restore_work,
			razer_blackshark_sidetone_restore_work);
}

static int razer_blackshark_setup_irq_urb(struct razer_blackshark_device *dev,
                                          struct hid_device *hdev,
                                          struct usb_interface *intf) {
  struct usb_device *usb_dev = interface_to_usbdev(intf);

  struct usb_host_interface *iface_desc = intf->cur_altsetting;
  struct usb_endpoint_descriptor *ep_desc = NULL;
  int retval;
  int i;

  unsigned int pipe = usb_rcvintpipe(usb_dev, RAZER_BLACKSHARK_EP_INTERRUPT_IN);

  for (i = 0; i < iface_desc->desc.bNumEndpoints; i++) {
    if (iface_desc->endpoint[i].desc.bEndpointAddress ==
        RAZER_BLACKSHARK_EP_INTERRUPT_IN) {
      ep_desc = &iface_desc->endpoint[i].desc;
      break;
    }
  }

  if (!ep_desc) {
    hid_err(hdev, "razerblackshark: ep 0x84 not found on interface 3\n");
    return -ENODEV;
  }

  dev->irq_urb = usb_alloc_urb(0, GFP_KERNEL);
  if (!dev->irq_urb) {
    hid_err(hdev, "razer: failed to allocate irq URB\n");
    return -ENOMEM;
  }

  dev->irq_buf = usb_alloc_coherent(usb_dev, RAZER_BLACKSHARK_REPORT_LEN,
                                    GFP_KERNEL, &dev->irq_buf_dma);

  if (!dev->irq_buf) {
    hid_err(hdev, "razerblackshark: failed to allocate irq buffer\n");
    usb_free_urb(dev->irq_urb);
    dev->irq_urb = NULL;
    return -ENOMEM;
  }

  usb_fill_int_urb(dev->irq_urb, usb_dev, pipe, dev->irq_buf,
                   RAZER_BLACKSHARK_REPORT_LEN, razer_blackshark_irq_callback,
                   dev, ep_desc->bInterval);
  dev->irq_urb->transfer_dma = dev->irq_buf_dma;
  dev->irq_urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

  retval = usb_submit_urb(dev->irq_urb, GFP_KERNEL);

  if (retval) {
    hid_err(hdev, "razerblackshark: failed to submit irq URB (%d)\n", retval);
    usb_free_coherent(usb_dev, RAZER_BLACKSHARK_REPORT_LEN, dev->irq_buf,
                      dev->irq_buf_dma);
    dev->irq_buf = NULL;
    usb_free_urb(dev->irq_urb);
    dev->irq_urb = NULL;
    return retval;
  }

  hid_info(hdev,
           "razerblackshark: interrupt URB submitted on ep 0x84 "
           "(interval %d)\n",
           ep_desc->bInterval);

  return 0;
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
  CREATE_DEVICE_FILE(&hdev->dev, &dev_attr_charge_level);
  CREATE_DEVICE_FILE(&hdev->dev, &dev_attr_charge_status);
  CREATE_DEVICE_FILE(&hdev->dev, &dev_attr_sidetone);

  hid_set_drvdata(hdev, dev);
  dev_set_drvdata(&hdev->dev, dev);

  if (hid_parse(hdev)) {
    hid_err(hdev, "razerblackshark: hid_parse failed\n");
    retval = -ENODEV;
    goto exit_free;
  }

  if (hid_hw_start(hdev, HID_CONNECT_HIDRAW)) {
    hid_err(hdev, "razerblackshark: hid_hw_start failed\n");
    retval = -ENODEV;
    goto exit_free;
  }

  usb_disable_autosuspend(usb_dev);

  retval = razer_blackshark_setup_irq_urb(dev, hdev, intf);
  if (retval) {
    hid_hw_stop(hdev);
    goto exit_free;
  }

  dev_info(&intf->dev, "razerblackshark: Razer BlackShark V2 HS initialised\n");

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
  struct usb_device *usb_dev = interface_to_usbdev(intf);

  if (intf->cur_altsetting->desc.bInterfaceNumber !=
      RAZER_BLACKSHARK_HID_INTERFACE) {
    return;
  }

	cancel_work_sync(&dev->sidetone_restore_work);
  if (dev->irq_urb) {
    usb_kill_urb(dev->irq_urb);
    usb_free_coherent(usb_dev, RAZER_BLACKSHARK_REPORT_LEN, dev->irq_buf,
                      dev->irq_buf_dma);
    usb_free_urb(dev->irq_urb);
  }


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
};

module_hid_driver(razer_blackshark_driver);
