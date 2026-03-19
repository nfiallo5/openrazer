#ifndef __HID_RAZER_BLACKSHARK_H
#define __HID_RAZER_BLACKSHARK_H

#include <linux/hid.h>
#include <linux/mutex.h>
#include <linux/usb.h>

#define USB_VENDOR_ID_RAZER 0x1532
#define USB_DEVICE_ID_RAZER_BLACKSHARK_V2_HS 0x0565

#define RAZER_BLACKSHARK_JIFFIES_INIT (INITIAL_JIFFIES - 10 * HZ)	

#define RAZER_BLACKSHARK_REPORT_LEN 64

#define RAZER_BLACKSHARK_HID_INTERFACE 3 

#define RAZER_BLACKSHARK_PUSH_STATUS 0x0a
#define RAZER_BLACKSHARK_PUSH_REPORT_ID 0x02
#define RAZER_BLACKSHARK_PUSH_EVENT_ID 0x60

#define RAZER_BLACKSHARK_EP_INTERRUPT_IN 0x84

#define RAZER_BLACKSHARK_SUBCMD_POWER_STATE 0x20
#define RAZER_BLACKSHARK_SUBCMD_BATTERY_LEVEL 0x21
#define RAZER_BLACKSHARK_SUBCMD_CHARGING_STATUS 0x2a

#define RAZER_BLACKSHARK_SUBCMD_IGNORE_RF 0x27
#define RAZER_BLACKSHARK_SUBCMD_IGNORE_AUDIO 0x55

#define RAZER_BLACKSHARK_SUBCMD_SIDETONE_A 0x98
#define RAZER_BLACKSHARK_SUBCMD_SIDETONE_B 0x99

#define RAZER_BLACKSHARK_SIDETONE_MAX_HW 15

#define RAZER_BLACKSHARK_WAIT_MIN_US 600
#define RAZER_BLACKSHARK_WAIT_MAX_US 1000

#define RAZER_BLACKSHARK_BATTERY_UNKNOWN 0xFF

struct razer_blackshark_device {
	struct hid_device *hdev;
	struct usb_device *usb_dev;
  struct mutex lock;

  unsigned char charge_level;
  unsigned char charge_status;
  unsigned char is_connected;
  unsigned char sidetone_level;
	unsigned char transaction_id;

  unsigned short usb_vid;
  unsigned short usb_pid;

  unsigned long last_unplug_jiffies;

	struct work_struct sidetone_restore_work;
};


#endif // !__HID_RAZER_BLACKSHARK_H
