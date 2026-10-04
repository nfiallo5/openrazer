// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Razer BlackShark V2 HS headset driver
 *
 * Copyright (c) 2026 OpenRazer contributors
 *
 * Supports the Razer BlackShark V2 HS (wired 0x056e and 2.4GHz dongle 0x0565).
 * These are MediaTek-based headsets that use a 64-byte vendor HID report
 * (report id 0x02, sent as an OUTPUT report) rather than the classic Razer
 * 90-byte protocol. See razerblackshark_driver.h for the frame layout.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/usb/input.h>
#include <linux/hid.h>
#include <linux/mutex.h>
#include <linux/completion.h>
#include <linux/workqueue.h>
#include <linux/string.h>

#include "razerblackshark_driver.h"
#include "razercommon.h"

#define DRIVER_DESC "Razer BlackShark V2 HS headset driver"

/*
 * Apply the connection-specific framing to a command frame.
 *
 * The command bytes, layout and reply format are identical on the wired unit and
 * the 2.4GHz dongle; only the framing differs. The wired unit takes the frame as
 * built. The dongle additionally needs the command class at byte[9] and a
 * trailing XOR checksum at byte[62], and only forwards the command to the
 * headset when both are present.
 */
static void razer_bs_apply_framing(struct razer_blackshark_device *device, u8 *buf)
{
    int i;
    u8 crc = 0;

    if (device->usb_pid != USB_DEVICE_ID_RAZER_BLACKSHARK_V2_HS_24)
        return;

    buf[RAZER_BS_BODY_INDEX] = RAZER_BS_CLASS_HEADSET;
    for (i = 0; i < RAZER_BS_CRC_INDEX; i++)
        crc ^= buf[i];
    buf[RAZER_BS_CRC_INDEX] = crc;
}

/*
 * Send a vendor command with an optional payload.
 *
 * The device replies to a write with an input report where reply[10] echoes the
 * command and reply[11] is 0x01. Commands are sent through the HID core with
 * hid_hw_raw_request() rather than a raw usb_control_msg(): this device drops
 * ep0 SET_REPORTs that race with usbhid, so the serialised HID-core path is
 * needed for commands to land reliably.
 */
static int razer_bs_fw_set(struct razer_blackshark_device *device, u8 cmd,
                           const u8 *payload, u8 len)
{
    u8 *buf;
    int ret;

    if (len > RAZER_BS_REPORT_LEN - RAZER_BS_PAYLOAD_INDEX)
        return -EINVAL;

    buf = kzalloc(RAZER_BS_REPORT_LEN, GFP_KERNEL);
    if (!buf)
        return -ENOMEM;

    buf[0]  = RAZER_BS_REPORT_ID;
    buf[2]  = RAZER_BS_CHANNEL;
    buf[6]  = 0x04 + len;           // fixed arg bytes + payload length
    buf[10] = cmd;
    buf[12] = len;
    if (len)
        memcpy(&buf[RAZER_BS_PAYLOAD_INDEX], payload, len);

    razer_bs_apply_framing(device, buf);

    ret = hid_hw_raw_request(device->hdev, RAZER_BS_REPORT_ID, buf,
                             RAZER_BS_REPORT_LEN, HID_OUTPUT_REPORT,
                             HID_REQ_SET_REPORT);
    fsleep(RAZER_BS_WAIT_US);

    if (ret < 0)
        hid_warn(device->hdev, "razerblackshark: command 0x%02x failed: %d\n", cmd, ret);

    kfree(buf);
    return ret < 0 ? ret : 0;
}

/*
 * Send a command whose body sits at byte[9], with its length at byte[6] and the
 * XOR checksum at byte[62]. This is the framing the status indicator uses; the
 * caller supplies the command class as the first body byte, so this carries no
 * channel byte.
 */
static int razer_bs_send_body(struct razer_blackshark_device *device,
                              const u8 *body, u8 body_len)
{
    u8 *buf;
    int ret, i;
    u8 crc = 0;

    if (body_len > RAZER_BS_CRC_INDEX - RAZER_BS_BODY_INDEX)
        return -EINVAL;

    buf = kzalloc(RAZER_BS_REPORT_LEN, GFP_KERNEL);
    if (!buf)
        return -ENOMEM;

    buf[0] = RAZER_BS_REPORT_ID;
    buf[2] = RAZER_BS_CHANNEL;
    buf[RAZER_BS_LEN_INDEX] = body_len;
    memcpy(&buf[RAZER_BS_BODY_INDEX], body, body_len);

    for (i = 0; i < RAZER_BS_CRC_INDEX; i++)
        crc ^= buf[i];
    buf[RAZER_BS_CRC_INDEX] = crc;

    ret = hid_hw_raw_request(device->hdev, RAZER_BS_REPORT_ID, buf,
                             RAZER_BS_REPORT_LEN, HID_OUTPUT_REPORT,
                             HID_REQ_SET_REPORT);
    fsleep(RAZER_BS_WAIT_US);

    if (ret < 0)
        hid_warn(device->hdev, "razerblackshark: command failed: %d\n", ret);

    kfree(buf);
    return ret < 0 ? ret : 0;
}

/*
 * Send a body-framed read and wait for its reply. The reply arrives in the same
 * shape as the channel reads, so it is captured by razer_blackshark_raw_event().
 * Caller must hold device->lock.
 *
 * Unlike razer_bs_fw_query() this does not reject a non-OK status. These reads
 * address the receiver rather than the headset, and the receiver answers an
 * opcode it does not implement by staying silent, which the timeout below already
 * catches, rather than by replying RAZER_BS_STATUS_UNSUPP. A status check would
 * therefore add nothing over the dongle, and the body-framed reply has not been
 * measured on the wired unit, so enforcing one here risks rejecting valid replies
 * for no gain. The status is still captured for callers that want to look.
 */
static int razer_bs_query_body(struct razer_blackshark_device *device,
                               const u8 *body, u8 body_len, u8 cmd)
{
    unsigned long left;
    int ret;

    device->fw_reply_cmd = cmd;
    device->fw_reply_val = 0;
    device->fw_reply_len = 0;
    device->fw_reply_status = 0;
    reinit_completion(&device->fw_reply);
    WRITE_ONCE(device->fw_wait, true);

    ret = razer_bs_send_body(device, body, body_len);
    if (ret < 0) {
        WRITE_ONCE(device->fw_wait, false);
        return ret;
    }

    left = wait_for_completion_timeout(&device->fw_reply,
                                       msecs_to_jiffies(RAZER_BS_REPLY_TIMEOUT_MS));
    WRITE_ONCE(device->fw_wait, false);

    if (!left) {
        hid_dbg(device->hdev, "razerblackshark: body read 0x%02x timed out\n", cmd);
        return -ETIMEDOUT;
    }
    return 0;
}

/*
 * Set the status-indicator LED mode. The set only takes effect when it follows a
 * prepare for the same setting, so both frames are sent together.
 */
static int razer_bs_send_status(struct razer_blackshark_device *device, u8 mode)
{
    u8 prepare[] = { RAZER_BS_CLASS_HEADSET,  RAZER_BS_FW_STATUS_R, 0x00, 0x00 };
    u8 set[]     = { RAZER_BS_CLASS_RECEIVER, RAZER_BS_FW_STATUS,   0x00, 0x01, mode };
    int ret;

    ret = razer_bs_send_body(device, prepare, sizeof(prepare));
    if (ret < 0)
        return ret;

    return razer_bs_send_body(device, set, sizeof(set));
}

/*
 * Send a read command and wait for its reply. The reply arrives asynchronously
 * as an input report handled by razer_blackshark_raw_event(); on success the
 * captured payload is left in device->fw_reply_*. Returns 0 or a negative errno,
 * including -EOPNOTSUPP when the device answered but reported a status other than
 * RAZER_BS_STATUS_OK -- most often RAZER_BS_STATUS_UNSUPP, whose 0x01 payload is
 * otherwise indistinguishable from a register that genuinely reads 1.
 * Caller must hold device->lock (only one read in flight at a time).
 */
static int razer_bs_fw_query(struct razer_blackshark_device *device, u8 cmd)
{
    u8 *buf;
    int ret;
    unsigned long left;

    buf = kzalloc(RAZER_BS_REPORT_LEN, GFP_KERNEL);
    if (!buf)
        return -ENOMEM;

    buf[0]  = RAZER_BS_REPORT_ID;
    buf[2]  = RAZER_BS_CHANNEL;
    buf[6]  = 0x04;                 // fixed arg bytes, no payload for a read
    buf[10] = cmd;
    buf[12] = 0x00;

    razer_bs_apply_framing(device, buf);

    device->fw_reply_cmd = cmd;
    device->fw_reply_val = 0;
    device->fw_reply_len = 0;
    device->fw_reply_status = 0;
    reinit_completion(&device->fw_reply);
    WRITE_ONCE(device->fw_wait, true);

    ret = hid_hw_raw_request(device->hdev, RAZER_BS_REPORT_ID, buf,
                             RAZER_BS_REPORT_LEN, HID_OUTPUT_REPORT,
                             HID_REQ_SET_REPORT);
    if (ret < 0) {
        WRITE_ONCE(device->fw_wait, false);
        hid_warn(device->hdev, "razerblackshark: read 0x%02x send failed: %d\n", cmd, ret);
        kfree(buf);
        return ret;
    }

    left = wait_for_completion_timeout(&device->fw_reply,
                                       msecs_to_jiffies(RAZER_BS_REPLY_TIMEOUT_MS));
    WRITE_ONCE(device->fw_wait, false);
    kfree(buf);

    if (!left) {
        /* Expected when a dongle has no headset linked; keep it out of the log. */
        hid_dbg(device->hdev, "razerblackshark: read 0x%02x timed out\n", cmd);
        return -ETIMEDOUT;
    }

    if (device->fw_reply_status != RAZER_BS_STATUS_OK) {
        hid_dbg(device->hdev, "razerblackshark: read 0x%02x returned status 0x%02x\n",
                cmd, device->fw_reply_status);
        return -EOPNOTSUPP;
    }
    return 0;
}

/* Read a single-byte value; returns the value (0..255) or a negative errno. */
static int razer_bs_fw_get(struct razer_blackshark_device *device, u8 cmd)
{
    int ret = razer_bs_fw_query(device, cmd);

    return ret < 0 ? ret : device->fw_reply_val;
}

/* Read a multi-byte payload into @out; returns the byte count or a negative errno. */
static int razer_bs_fw_get_buf(struct razer_blackshark_device *device, u8 cmd,
                               u8 *out, u8 out_len)
{
    int ret = razer_bs_fw_query(device, cmd);
    u8 n;

    if (ret < 0)
        return ret;

    n = min(device->fw_reply_len, out_len);
    memcpy(out, device->fw_reply_buf, n);
    return n;
}

/*
 * Capture read replies. The device answers a read with an input report id 0x02
 * whose byte[10] echoes the command, byte[12] holds the payload length and
 * byte[13..] the value bytes. This only looks while a read is pending; returning
 * 0 leaves the report for hidraw and other users.
 *
 * The status at byte[1] is captured but not matched on: an unsupported opcode is
 * answered by a report that is well-formed in every other respect, so refusing to
 * complete on it would only turn the read into a RAZER_BS_REPLY_TIMEOUT_MS stall.
 * The waiter inspects the status instead.
 *
 * Unsolicited link pushes (receiver register 0x20) are picked up first, whether
 * or not a read is pending. This runs in interrupt context, so it only records
 * the new state; @link_work does the reporting.
 */
static int razer_blackshark_raw_event(struct hid_device *hdev, struct hid_report *report,
                                      u8 *data, int size)
{
    struct razer_blackshark_device *device = hid_get_drvdata(hdev);
    u8 plen;
    int link;

    if (!device)
        return 0;

    if (device->usb_pid == USB_DEVICE_ID_RAZER_BLACKSHARK_V2_HS_24 &&
        size > RAZER_BS_PAYLOAD_INDEX &&
        data[0] == RAZER_BS_REPORT_ID &&
        data[RAZER_BS_STATUS_INDEX] == RAZER_BS_PUSH_STATUS &&
        data[10] == RAZER_BS_RX_LINK &&
        data[11] == RAZER_BS_PUSH_FLAG &&
        data[12] >= 1) {
        link = data[RAZER_BS_PAYLOAD_INDEX] ? 1 : 0;
        if (READ_ONCE(device->link_state) != link) {
            WRITE_ONCE(device->link_state, link);
            if (!READ_ONCE(device->ids_stop))
                mod_delayed_work(system_wq, &device->link_work, 0);
        }
        return 0;
    }

    if (!READ_ONCE(device->fw_wait))
        return 0;

    if (size > RAZER_BS_PAYLOAD_INDEX &&
        data[0] == RAZER_BS_REPORT_ID &&
        data[10] == device->fw_reply_cmd &&
        data[11] == 0x01) {                     // ACK flag
        plen = data[12];
        if (plen > size - RAZER_BS_PAYLOAD_INDEX)
            plen = size - RAZER_BS_PAYLOAD_INDEX;
        if (plen > sizeof(device->fw_reply_buf))
            plen = sizeof(device->fw_reply_buf);
        device->fw_reply_len = plen;
        memcpy(device->fw_reply_buf, &data[RAZER_BS_PAYLOAD_INDEX], plen);
        device->fw_reply_val = data[RAZER_BS_PAYLOAD_INDEX];
        device->fw_reply_status = data[RAZER_BS_STATUS_INDEX];
        complete(&device->fw_reply);
    }
    return 0;
}

/* ---------------------------------------------------------------- read attrs */

static void razer_blackshark_ids_work(struct work_struct *work);
static void razer_blackshark_link_work(struct work_struct *work);

static ssize_t razer_attr_read_version(struct device *dev, struct device_attribute *attr, char *buf)
{
    return sysfs_emit(buf, "%s\n", DRIVER_VERSION);
}

static ssize_t razer_attr_read_device_type(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct razer_blackshark_device *device = dev_get_drvdata(dev);

    char *device_type;

    switch (device->usb_pid) {
    case USB_DEVICE_ID_RAZER_BLACKSHARK_V2_HS_24:
        device_type = "Razer BlackShark V2 HS 2.4";
        break;

    case USB_DEVICE_ID_RAZER_BLACKSHARK_V2_HS_USB:
        device_type = "Razer BlackShark V2 HS USB";
        break;

    default:
        device_type = "Unknown Device";
    }

    return sysfs_emit(buf, "%s\n", device_type);
}

/*
 * Serial and firmware version are fetched in the background by
 * razer_blackshark_ids_work(); these handlers never touch the device, they only
 * emit what is currently stored (the fallback until a fetch succeeds), under
 * @lock so they cannot race the work updating the buffers.
 */
static ssize_t razer_attr_read_device_serial(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct razer_blackshark_device *device = dev_get_drvdata(dev);
    ssize_t ret;

    mutex_lock(&device->lock);
    ret = sysfs_emit(buf, "%s\n", device->serial);
    mutex_unlock(&device->lock);
    return ret;
}

static ssize_t razer_attr_read_firmware_version(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct razer_blackshark_device *device = dev_get_drvdata(dev);
    ssize_t ret;

    mutex_lock(&device->lock);
    ret = sysfs_emit(buf, "v%u.%u\n", device->firmware_version[1], device->firmware_version[2]);
    mutex_unlock(&device->lock);
    return ret;
}

/*
 * Read device file "hw_model"
 *
 * The headset reports its own product id (two bytes, high byte first) as a 4-digit
 * hex value. Over the 2.4GHz dongle this identifies the paired headset, so it can
 * differ from the USB product id of the interface the driver is bound to.
 */
static ssize_t razer_attr_read_hw_model(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct razer_blackshark_device *device = dev_get_drvdata(dev);
    u8 id[2];
    int n;

    mutex_lock(&device->lock);
    n = razer_bs_fw_get_buf(device, RAZER_BS_FW_HW_MODEL, id, sizeof(id));
    mutex_unlock(&device->lock);

    if (n < 0)
        return n;
    if (n != sizeof(id))
        return -EIO;

    return sysfs_emit(buf, "%04x\n", (id[0] << 8) | id[1]);
}

/*
 * Read device file "edition_id"
 *
 * Identifies the product variant (special editions and colourways share a product
 * id but differ here). Reads 0 on a standard-edition unit.
 */
static ssize_t razer_attr_read_edition_id(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct razer_blackshark_device *device = dev_get_drvdata(dev);
    int id;

    mutex_lock(&device->lock);
    id = razer_bs_fw_get(device, RAZER_BS_FW_EDITION_ID);
    mutex_unlock(&device->lock);

    if (id < 0)
        return id;

    return sysfs_emit(buf, "%d\n", id);
}

/*
 * Read device file "hardware_id"
 *
 * Board revision of the headset, distinct from the product id in "hw_model".
 * Reads 0 on the initial revision.
 */
static ssize_t razer_attr_read_hardware_id(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct razer_blackshark_device *device = dev_get_drvdata(dev);
    int id;

    mutex_lock(&device->lock);
    id = razer_bs_fw_get(device, RAZER_BS_FW_HARDWARE_ID);
    mutex_unlock(&device->lock);

    if (id < 0)
        return id;

    return sysfs_emit(buf, "%d\n", id);
}

static ssize_t razer_attr_read_device_mode(struct device *dev, struct device_attribute *attr, char *buf)
{
    // No device-mode support on these headsets; report normal driver mode.
    buf[0] = 0x00;
    buf[1] = 0x00;
    return 2;
}

static ssize_t razer_attr_read_test(struct device *dev, struct device_attribute *attr, char *buf)
{
    return sysfs_emit(buf, "\n");
}

/*
 * Read device file "charge_level"
 *
 * Battery charge as a 0-255 value (OpenRazer convention). The device reports a
 * percentage, which is scaled to 0-255 here.
 */
static ssize_t razer_attr_read_charge_level(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct razer_blackshark_device *device = dev_get_drvdata(dev);
    int percent;

    mutex_lock(&device->lock);
    percent = razer_bs_fw_get(device, RAZER_BS_FW_BATTERY);
    mutex_unlock(&device->lock);

    /*
     * Headset asleep or out of range while the dongle stays connected: the
     * relayed read just times out. Every OpenRazer battery consumer expects
     * this file to always parse as a number, with -1 meaning "unknown" (the
     * daemon's get_battery does `if battery_255 < 0: return -1.0`). Returning
     * the raw errno instead makes the read() syscall fail and the caller
     * throw, so emit the sentinel.
     */
    if (percent < 0)
        return sysfs_emit(buf, "-1\n");
    if (percent > 100)
        percent = 100;

    /*
     * The headset answered, so it is awake and reachable: a good moment to
     * pick up the real serial / firmware if the probe-time attempts missed
     * them (headset powered on well after boot).
     */
    if (!READ_ONCE(device->ids_stop) &&
        !(READ_ONCE(device->serial_read) && READ_ONCE(device->fw_read)))
        schedule_delayed_work(&device->ids_work, 0);

    return sysfs_emit(buf, "%d\n", DIV_ROUND_CLOSEST(percent * 255, 100));
}

/*
 * Read device file "charge_status"
 *
 * Returns 1 while the headset is charging, 0 otherwise.
 */
static ssize_t razer_attr_read_charge_status(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct razer_blackshark_device *device = dev_get_drvdata(dev);
    int charging;

    mutex_lock(&device->lock);
    charging = razer_bs_fw_get(device, RAZER_BS_FW_CHARGING);
    mutex_unlock(&device->lock);

    /* See razer_attr_read_charge_level: emit a parseable sentinel, not an
     * errno, when the headset is unreachable. */
    if (charging < 0)
        return sysfs_emit(buf, "0\n");

    return sysfs_emit(buf, "%d\n", charging ? 1 : 0);
}

/*
 * Read device file "headset_connected" (dongle only)
 *
 * 1 while the headset is on the 2.4GHz link, 0 while it is off or out of range,
 * -1 until the state is first known. Never touches the device: it emits the state
 * kept current by the receiver's pushes, so it is cheap to read. Each change is
 * signalled with sysfs_notify() on this file and a KOBJ_CHANGE uevent carrying
 * HEADSET_CONNECTED=0|1, so user space can wait for it instead of polling.
 */
static ssize_t razer_attr_read_headset_connected(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct razer_blackshark_device *device = dev_get_drvdata(dev);

    return sysfs_emit(buf, "%d\n", READ_ONCE(device->link_state));
}

/* --------------------------------------------------------------- write attrs */

/*
 * The equalizer engine has its own enable. Without it the preset and the custom
 * curve are stored but never applied to the audio, so it is turned on alongside
 * every equalizer write.
 */
static void razer_bs_eq_enable(struct razer_blackshark_device *device)
{
    u8 on = 0x01;

    razer_bs_fw_set(device, RAZER_BS_FW_EQ_ENABLE, &on, 1);
}

/*
 * Write device file "equalizer_enable"
 *
 * Master enable for the equalizer engine. One raw byte: 0 = off, non-zero = on.
 * The preset and the custom curve stay stored while it is off; they are simply
 * not applied to the audio.
 */
static ssize_t razer_attr_write_equalizer_enable(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
    struct razer_blackshark_device *device = dev_get_drvdata(dev);
    u8 enabled;

    if (count < 1)
        return -EINVAL;

    enabled = buf[0] ? 0x01 : 0x00;

    mutex_lock(&device->lock);
    razer_bs_fw_set(device, RAZER_BS_FW_EQ_ENABLE, &enabled, 1);
    mutex_unlock(&device->lock);

    return count;
}

/*
 * Read device file "equalizer_enable"
 *
 * Returns whether the equalizer engine is active: 0 = off, 1 = on.
 */
static ssize_t razer_attr_read_equalizer_enable(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct razer_blackshark_device *device = dev_get_drvdata(dev);
    int enabled;

    mutex_lock(&device->lock);
    enabled = razer_bs_fw_get(device, RAZER_BS_FW_EQ_ENABLE_R);
    mutex_unlock(&device->lock);

    if (enabled < 0)
        return enabled;

    buf[0] = enabled ? 0x01 : 0x00;
    return 1;
}

/*
 * Write device file "equalizer_preset"
 *
 * Selects a built-in EQ preset from a single raw byte: 0x07 game, 0x08 music,
 * 0x09 movie, 0xff custom. Also enables the equalizer engine, so that selecting a
 * preset is audible without a separate write to "equalizer_enable".
 */
static ssize_t razer_attr_write_equalizer_preset(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
    struct razer_blackshark_device *device = dev_get_drvdata(dev);
    u8 preset;

    if (count < 1)
        return -EINVAL;

    preset = buf[0];

    mutex_lock(&device->lock);
    razer_bs_eq_enable(device);
    razer_bs_fw_set(device, RAZER_BS_FW_EQ_PRESET, &preset, 1);
    mutex_unlock(&device->lock);

    return count;
}

/*
 * Read device file "equalizer_preset"
 *
 * Returns the current preset code as a raw byte (0x07 game, 0x08 music,
 * 0x09 movie, 0xff custom).
 */
static ssize_t razer_attr_read_equalizer_preset(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct razer_blackshark_device *device = dev_get_drvdata(dev);
    int preset;

    mutex_lock(&device->lock);
    preset = razer_bs_fw_get(device, RAZER_BS_FW_EQ_READ);
    mutex_unlock(&device->lock);

    if (preset < 0)
        return preset;

    buf[0] = preset;
    return 1;
}

/*
 * Write device file "equalizer"
 *
 * Exactly 10 signed bytes (int8 dB gains, 31Hz..16kHz) for the custom EQ curve.
 * Selects the custom preset, then uploads the curve. Each gain is clamped to the
 * hardware range and biased by RAZER_BS_EQ_WRITE_OFFSET so the device applies the
 * requested gain (see the equalizer read handler). Also enables the equalizer
 * engine, so an uploaded curve is audible straight away.
 */
static ssize_t razer_attr_write_equalizer(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
    struct razer_blackshark_device *device = dev_get_drvdata(dev);
    u8 custom = RAZER_BS_EQ_CUSTOM;
    u8 curve[RAZER_BS_EQ_BANDS];
    int i;

    if (count != RAZER_BS_EQ_BANDS)
        return -EINVAL;

    for (i = 0; i < RAZER_BS_EQ_BANDS; i++) {
        int gain = clamp((int)(s8)buf[i], RAZER_BS_EQ_GAIN_MIN, RAZER_BS_EQ_GAIN_MAX);

        curve[i] = (u8)(s8)(gain + RAZER_BS_EQ_WRITE_OFFSET);
    }

    mutex_lock(&device->lock);
    razer_bs_eq_enable(device);
    razer_bs_fw_set(device, RAZER_BS_FW_EQ_PRESET, &custom, 1);
    razer_bs_fw_set(device, RAZER_BS_FW_EQ_CURVE, curve, RAZER_BS_EQ_BANDS);
    mutex_unlock(&device->lock);

    return count;
}

/*
 * Read device file "equalizer"
 *
 * Returns the stored custom 10-band curve as signed dB gains. The device applies
 * the stored value directly as the gain, so it is returned as-is (the +5 bias is
 * only applied on write). This is the custom curve, regardless of which preset is
 * currently selected.
 */
static ssize_t razer_attr_read_equalizer(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct razer_blackshark_device *device = dev_get_drvdata(dev);
    u8 curve[RAZER_BS_EQ_BANDS];
    int n;

    mutex_lock(&device->lock);
    n = razer_bs_fw_get_buf(device, RAZER_BS_FW_EQ_CURVE_R, curve, RAZER_BS_EQ_BANDS);
    mutex_unlock(&device->lock);

    if (n < 0)
        return n;
    if (n != RAZER_BS_EQ_BANDS)
        return -EIO;

    memcpy(buf, curve, RAZER_BS_EQ_BANDS);
    return RAZER_BS_EQ_BANDS;
}

/*
 * Write device file "sidetone"
 *
 * A single raw byte 0-15. 0 disables sidetone; 1-15 enables it at that level
 * (the level is a 4-bit field).
 */
static ssize_t razer_attr_write_sidetone(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
    struct razer_blackshark_device *device = dev_get_drvdata(dev);
    u8 level, enabled;

    if (count < 1)
        return -EINVAL;

    level = buf[0] > RAZER_BS_SIDETONE_MAX ? RAZER_BS_SIDETONE_MAX : buf[0];
    enabled = level > 0 ? 0x01 : 0x00;

    mutex_lock(&device->lock);
    razer_bs_fw_set(device, RAZER_BS_FW_SIDETONE_EN, &enabled, 1);
    if (enabled)
        razer_bs_fw_set(device, RAZER_BS_FW_SIDETONE_LV, &level, 1);
    mutex_unlock(&device->lock);

    return count;
}

/*
 * Read device file "sidetone"
 *
 * Returns a single raw byte: 0 = off, 1..15 = active level.
 */
static ssize_t razer_attr_read_sidetone(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct razer_blackshark_device *device = dev_get_drvdata(dev);
    int enabled, level = 0;

    mutex_lock(&device->lock);
    enabled = razer_bs_fw_get(device, RAZER_BS_FW_SIDETONE_EN_R);
    if (enabled > 0)
        level = razer_bs_fw_get(device, RAZER_BS_FW_SIDETONE_LV_R);
    mutex_unlock(&device->lock);

    if (enabled < 0)
        return enabled;

    buf[0] = (enabled && level > 0) ? level : 0;
    return 1;
}

/*
 * Read device file "mic_mute"
 *
 * Microphone mute state: 1 = muted, 0 = live. This is a hardware toggle (the mute
 * button) and is read-only; it cannot be set from the host.
 */
static ssize_t razer_attr_read_mic_mute(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct razer_blackshark_device *device = dev_get_drvdata(dev);
    int muted;

    mutex_lock(&device->lock);
    muted = razer_bs_fw_get(device, RAZER_BS_FW_MIC_MUTE);
    mutex_unlock(&device->lock);

    if (muted < 0)
        return muted;

    buf[0] = muted ? 1 : 0;
    return 1;
}

/*
 * Write device file "dnd"
 *
 * Do Not Disturb (mutes battery/status voice prompts). One raw byte: 0 = off,
 * non-zero = on.
 */
static ssize_t razer_attr_write_dnd(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
    struct razer_blackshark_device *device = dev_get_drvdata(dev);
    u8 enabled;

    if (count < 1)
        return -EINVAL;

    enabled = buf[0] ? 0x01 : 0x00;

    mutex_lock(&device->lock);
    razer_bs_fw_set(device, RAZER_BS_FW_DND, &enabled, 1);
    mutex_unlock(&device->lock);

    return count;
}

/*
 * Read device file "dnd"
 *
 * Returns the Do Not Disturb state: 0 = off, 1 = on.
 */
static ssize_t razer_attr_read_dnd(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct razer_blackshark_device *device = dev_get_drvdata(dev);
    int v;

    mutex_lock(&device->lock);
    v = razer_bs_fw_get(device, RAZER_BS_FW_DND_R);
    mutex_unlock(&device->lock);

    if (v < 0)
        return v;

    buf[0] = v ? 0x01 : 0x00;
    return 1;
}

/*
 * Write device file "device_idle_time"
 *
 * Auto power-off timer, in seconds (standard OpenRazer idle-time attribute). The
 * headset stores the timer in whole minutes, so the value is rounded to the
 * nearest minute on write and expanded back to seconds on read. 0 disables it.
 */
static ssize_t razer_attr_write_device_idle_time(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
    struct razer_blackshark_device *device = dev_get_drvdata(dev);
    unsigned int seconds;
    u8 minutes;

    if (kstrtouint(buf, 10, &seconds))
        return -EINVAL;

    minutes = min(DIV_ROUND_CLOSEST(seconds, 60u), 255u);

    mutex_lock(&device->lock);
    razer_bs_fw_set(device, RAZER_BS_FW_AUTO_OFF, &minutes, 1);
    mutex_unlock(&device->lock);

    return count;
}

/*
 * Read device file "device_idle_time"
 *
 * Returns the auto power-off timer in seconds (0 = disabled).
 */
static ssize_t razer_attr_read_device_idle_time(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct razer_blackshark_device *device = dev_get_drvdata(dev);
    int minutes;

    mutex_lock(&device->lock);
    minutes = razer_bs_fw_get(device, RAZER_BS_FW_AUTO_OFF_R);
    mutex_unlock(&device->lock);

    /* Unreachable headset: emit a parseable sentinel rather than an errno so a
     * plain `cat` and script read still work (see charge_level). */
    if (minutes < 0)
        return sysfs_emit(buf, "-1\n");

    return sysfs_emit(buf, "%d\n", minutes * 60);
}

/*
 * Write device file "status_indicator"
 *
 * On-headset status LED mode, one byte: 1 = connection status, 2 = battery
 * status, 3 = battery warning only, 0 = off.
 */
static ssize_t razer_attr_write_status_indicator(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
    struct razer_blackshark_device *device = dev_get_drvdata(dev);

    if (count < 1)
        return -EINVAL;

    mutex_lock(&device->lock);
    razer_bs_send_status(device, buf[0]);
    mutex_unlock(&device->lock);

    return count;
}

/*
 * Read device file "status_indicator"
 *
 * Returns the current LED mode: 0 = off, 1 = connection, 2 = battery,
 * 3 = battery warning only.
 */
static ssize_t razer_attr_read_status_indicator(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct razer_blackshark_device *device = dev_get_drvdata(dev);
    u8 body[] = { RAZER_BS_CLASS_RECEIVER, RAZER_BS_FW_STATUS_R, 0x00, 0x00 };
    int ret;

    mutex_lock(&device->lock);
    ret = razer_bs_query_body(device, body, sizeof(body), RAZER_BS_FW_STATUS_R);
    mutex_unlock(&device->lock);

    if (ret < 0)
        return ret;

    buf[0] = device->fw_reply_val;
    return 1;
}

/* -------------------------------------------------------------- attr defines */

static DEVICE_ATTR(version,           0440, razer_attr_read_version,          NULL);
static DEVICE_ATTR(test,              0440, razer_attr_read_test,             NULL);
static DEVICE_ATTR(device_type,       0440, razer_attr_read_device_type,      NULL);
static DEVICE_ATTR(device_serial,     0440, razer_attr_read_device_serial,    NULL);
static DEVICE_ATTR(firmware_version,  0440, razer_attr_read_firmware_version, NULL);
static DEVICE_ATTR(device_mode,       0440, razer_attr_read_device_mode,      NULL);
static DEVICE_ATTR(hw_model,          0440, razer_attr_read_hw_model,         NULL);
static DEVICE_ATTR(edition_id,        0440, razer_attr_read_edition_id,       NULL);
static DEVICE_ATTR(hardware_id,       0440, razer_attr_read_hardware_id,      NULL);
static DEVICE_ATTR(charge_level,      0440, razer_attr_read_charge_level,     NULL);
static DEVICE_ATTR(charge_status,     0440, razer_attr_read_charge_status,    NULL);
static DEVICE_ATTR(headset_connected, 0440, razer_attr_read_headset_connected, NULL);
static DEVICE_ATTR(equalizer_enable,  0660, razer_attr_read_equalizer_enable, razer_attr_write_equalizer_enable);
static DEVICE_ATTR(equalizer_preset,  0660, razer_attr_read_equalizer_preset, razer_attr_write_equalizer_preset);
static DEVICE_ATTR(equalizer,         0660, razer_attr_read_equalizer,        razer_attr_write_equalizer);
static DEVICE_ATTR(sidetone,          0660, razer_attr_read_sidetone,         razer_attr_write_sidetone);
static DEVICE_ATTR(mic_mute,          0440, razer_attr_read_mic_mute,         NULL);
static DEVICE_ATTR(dnd,               0660, razer_attr_read_dnd,              razer_attr_write_dnd);
static DEVICE_ATTR(device_idle_time,  0660, razer_attr_read_device_idle_time, razer_attr_write_device_idle_time);
static DEVICE_ATTR(status_indicator,  0660, razer_attr_read_status_indicator, razer_attr_write_status_indicator);

/* --------------------------------------------------------------- probe/remove */

/*
 * A serial we can key persistence off: at least RAZER_BS_SERIAL_MIN printable
 * characters and not all identical. Rejects this dongle's placeholder USB
 * iSerial ("0000000000000000") and an all-zero reply from a flaky link.
 */
static bool bs_serial_usable(const char *s)
{
    size_t i, len = strnlen(s, RAZER_BS_SERIAL_LEN);

    if (len < RAZER_BS_SERIAL_MIN)
        return false;
    for (i = 0; i < len; i++) {
        if (s[i] < 0x20 || s[i] > 0x7e)
            return false;               // not printable ASCII
        if (s[i] != s[0])
            break;                      // has variation, not a degenerate string
    }
    return i != len;                    // i == len => every character identical
}

static void razer_blackshark_init(struct razer_blackshark_device *dev, struct usb_interface *intf, struct hid_device *hdev)
{
    struct usb_device *usb_dev = interface_to_usbdev(intf);

    mutex_init(&dev->lock);
    init_completion(&dev->fw_reply);
    INIT_DELAYED_WORK(&dev->ids_work, razer_blackshark_ids_work);
    INIT_DELAYED_WORK(&dev->link_work, razer_blackshark_link_work);
    dev->link_state = RAZER_BS_LINK_UNKNOWN;
    dev->link_reported = RAZER_BS_LINK_UNKNOWN;
    dev->hdev = hdev;
    dev->usb_dev = usb_dev;
    dev->usb_pid = le16_to_cpu(usb_dev->descriptor.idProduct);

    /*
     * Stable fallbacks until razer_blackshark_ids_work() reads the real values
     * off the device. The dongle's USB iSerial is a placeholder, so fall back
     * to a constant derived from the product id: identical across reboots,
     * which keeps the daemon's persistence key (and D-Bus path) stable even
     * when the headset is never powered on.
     */
    if (usb_dev->serial && bs_serial_usable(usb_dev->serial))
        scnprintf(dev->serial, sizeof(dev->serial), "%s", usb_dev->serial);
    else
        scnprintf(dev->serial, sizeof(dev->serial), "BS-%04x", dev->usb_pid);
    dev->firmware_version[0] = 1;
    dev->firmware_version[1] = 1;
    dev->firmware_version[2] = 0;
}

/*
 * Fetch the real serial number and firmware version in the background.
 *
 * Run off a delayed_work rather than from probe or the sysfs read path: a
 * 2.4GHz dongle needs a moment after enumeration before it relays headset
 * reads, and the headset may be powered off entirely. Each run attempts only
 * the ids not yet obtained, holds @lock only for the ~500ms of an actual query
 * (never across the inter-attempt delay), and reschedules itself with a fixed
 * backoff up to RAZER_BS_IDS_MAX_ATTEMPTS. A successful charge_level /
 * charge_status read also kicks it, so a headset powered on long after boot is
 * still picked up. @ids_attempts is only ever touched here, and the work is
 * never concurrent with itself, so it needs no locking.
 */
static void razer_blackshark_ids_work(struct work_struct *work)
{
    struct razer_blackshark_device *device =
        container_of(to_delayed_work(work), struct razer_blackshark_device, ids_work);
    int n;

    if (READ_ONCE(device->ids_stop))
        return;

    if (!device->serial_read) {
        char tmp[RAZER_BS_SERIAL_LEN];

        mutex_lock(&device->lock);
        n = razer_bs_fw_get_buf(device, RAZER_BS_FW_SERIAL, (u8 *)tmp, sizeof(tmp) - 1);
        if (n > 0) {
            tmp[n] = '\0';
            if (bs_serial_usable(tmp)) {
                scnprintf(device->serial, sizeof(device->serial), "%s", tmp);
                device->serial_read = true;
            }
            /* not usable: leave the fallback in place and retry */
        }
        mutex_unlock(&device->lock);
    }

    if (!device->fw_read) {
        u8 tmp[16];

        mutex_lock(&device->lock);
        n = razer_bs_fw_get_buf(device, RAZER_BS_FW_VERSION, tmp, sizeof(tmp));
        if (n >= 2) {
            device->firmware_version[0] = 1;
            device->firmware_version[1] = tmp[0];   // major
            device->firmware_version[2] = tmp[1];   // minor
            device->fw_read = true;
        }
        mutex_unlock(&device->lock);
    }

    if (device->serial_read && device->fw_read)
        return;
    if (READ_ONCE(device->ids_stop))
        return;
    if (++device->ids_attempts < RAZER_BS_IDS_MAX_ATTEMPTS)
        schedule_delayed_work(&device->ids_work,
                              msecs_to_jiffies(RAZER_BS_IDS_RETRY_MS));
}

/*
 * Track the headset's 2.4GHz link and report each change to user space.
 *
 * Kicked by .raw_event on every 0x20 push, and scheduled once from probe. While
 * the state is still unknown it reads receiver register 0x20 to seed it,
 * retrying up to RAZER_BS_LINK_MAX_ATTEMPTS. The read result is only stored if
 * no push landed meanwhile, as a push is always the newer state. Reporting is
 * done here rather than in .raw_event because kobject_uevent_env() can sleep.
 *
 * A headset that has just joined is also the moment to fetch a serial / firmware
 * the probe-time attempts missed, so ids_work is kicked on every 0 -> 1 change.
 */
static void razer_blackshark_link_work(struct work_struct *work)
{
    struct razer_blackshark_device *device =
        container_of(to_delayed_work(work), struct razer_blackshark_device, link_work);
    u8 body[] = { RAZER_BS_CLASS_RECEIVER, RAZER_BS_RX_LINK, 0x00, 0x00 };
    char env[] = "HEADSET_CONNECTED=0";
    char *envp[] = { env, NULL };
    int ret, val = 0, state;

    if (READ_ONCE(device->ids_stop))
        return;

    if (READ_ONCE(device->link_state) == RAZER_BS_LINK_UNKNOWN) {
        mutex_lock(&device->lock);
        ret = razer_bs_query_body(device, body, sizeof(body), RAZER_BS_RX_LINK);
        if (!ret)
            val = device->fw_reply_val ? 1 : 0;
        mutex_unlock(&device->lock);

        if (!ret)
            cmpxchg(&device->link_state, RAZER_BS_LINK_UNKNOWN, val);
    }

    state = READ_ONCE(device->link_state);
    if (state == RAZER_BS_LINK_UNKNOWN) {
        if (++device->link_attempts < RAZER_BS_LINK_MAX_ATTEMPTS &&
            !READ_ONCE(device->ids_stop))
            schedule_delayed_work(&device->link_work,
                                  msecs_to_jiffies(RAZER_BS_LINK_RETRY_MS));
        return;
    }

    if (state == device->link_reported)
        return;
    device->link_reported = state;

    hid_dbg(device->hdev, "razerblackshark: headset %s\n",
            state ? "connected" : "disconnected");
    sysfs_notify(&device->hdev->dev.kobj, NULL, "headset_connected");
    env[sizeof(env) - 2] = state ? '1' : '0';
    kobject_uevent_env(&device->hdev->dev.kobj, KOBJ_CHANGE, envp);

    if (state && !READ_ONCE(device->ids_stop) &&
        !(READ_ONCE(device->serial_read) && READ_ONCE(device->fw_read)))
        schedule_delayed_work(&device->ids_work,
                              msecs_to_jiffies(RAZER_BS_IDS_FIRST_DELAY_MS));
}

static void razer_blackshark_remove_files(struct hid_device *hdev)
{
    device_remove_file(&hdev->dev, &dev_attr_version);
    device_remove_file(&hdev->dev, &dev_attr_test);
    device_remove_file(&hdev->dev, &dev_attr_device_type);
    device_remove_file(&hdev->dev, &dev_attr_device_serial);
    device_remove_file(&hdev->dev, &dev_attr_firmware_version);
    device_remove_file(&hdev->dev, &dev_attr_device_mode);
    device_remove_file(&hdev->dev, &dev_attr_hw_model);
    device_remove_file(&hdev->dev, &dev_attr_edition_id);
    device_remove_file(&hdev->dev, &dev_attr_hardware_id);
    device_remove_file(&hdev->dev, &dev_attr_charge_level);
    device_remove_file(&hdev->dev, &dev_attr_charge_status);
    device_remove_file(&hdev->dev, &dev_attr_headset_connected);
    device_remove_file(&hdev->dev, &dev_attr_equalizer_enable);
    device_remove_file(&hdev->dev, &dev_attr_equalizer_preset);
    device_remove_file(&hdev->dev, &dev_attr_equalizer);
    device_remove_file(&hdev->dev, &dev_attr_sidetone);
    device_remove_file(&hdev->dev, &dev_attr_mic_mute);
    device_remove_file(&hdev->dev, &dev_attr_dnd);
    device_remove_file(&hdev->dev, &dev_attr_device_idle_time);
    device_remove_file(&hdev->dev, &dev_attr_status_indicator);
}

static int razer_blackshark_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
    int retval = -EIO;
    struct usb_interface *intf = to_usb_interface(hdev->dev.parent);
    struct usb_device *usb_dev = interface_to_usbdev(intf);
    struct razer_blackshark_device *dev;

    dev = kzalloc(sizeof(*dev), GFP_KERNEL);
    if (!dev) {
        hid_err(hdev, "out of memory\n");
        return -ENOMEM;
    }

    razer_blackshark_init(dev, intf, hdev);
    dev_set_drvdata(&hdev->dev, dev);

    CREATE_DEVICE_FILE(&hdev->dev, &dev_attr_version);
    CREATE_DEVICE_FILE(&hdev->dev, &dev_attr_test);
    CREATE_DEVICE_FILE(&hdev->dev, &dev_attr_device_type);
    CREATE_DEVICE_FILE(&hdev->dev, &dev_attr_device_serial);
    CREATE_DEVICE_FILE(&hdev->dev, &dev_attr_firmware_version);
    CREATE_DEVICE_FILE(&hdev->dev, &dev_attr_device_mode);

    switch (dev->usb_pid) {
    case USB_DEVICE_ID_RAZER_BLACKSHARK_V2_HS_24:
    case USB_DEVICE_ID_RAZER_BLACKSHARK_V2_HS_USB:
        CREATE_DEVICE_FILE(&hdev->dev, &dev_attr_hw_model);
        CREATE_DEVICE_FILE(&hdev->dev, &dev_attr_edition_id);
        CREATE_DEVICE_FILE(&hdev->dev, &dev_attr_hardware_id);
        CREATE_DEVICE_FILE(&hdev->dev, &dev_attr_charge_level);
        CREATE_DEVICE_FILE(&hdev->dev, &dev_attr_charge_status);
        CREATE_DEVICE_FILE(&hdev->dev, &dev_attr_equalizer_enable);
        CREATE_DEVICE_FILE(&hdev->dev, &dev_attr_equalizer_preset);
        CREATE_DEVICE_FILE(&hdev->dev, &dev_attr_equalizer);
        CREATE_DEVICE_FILE(&hdev->dev, &dev_attr_sidetone);
        CREATE_DEVICE_FILE(&hdev->dev, &dev_attr_mic_mute);
        CREATE_DEVICE_FILE(&hdev->dev, &dev_attr_dnd);
        CREATE_DEVICE_FILE(&hdev->dev, &dev_attr_device_idle_time);
        CREATE_DEVICE_FILE(&hdev->dev, &dev_attr_status_indicator);
        break;
    }

    /* Only the dongle has a link to track: the wired unit is the headset. */
    if (dev->usb_pid == USB_DEVICE_ID_RAZER_BLACKSHARK_V2_HS_24)
        CREATE_DEVICE_FILE(&hdev->dev, &dev_attr_headset_connected);

    retval = hid_parse(hdev);
    if (retval) {
        hid_err(hdev, "parse failed\n");
        goto exit_free;
    }

    retval = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
    if (retval) {
        hid_err(hdev, "hw start failed\n");
        goto exit_free;
    }

    /*
     * Keep the interrupt-IN endpoint polled so GET reply reports reach
     * .raw_event even when nothing has the hidraw node open.
     */
    retval = hid_hw_open(hdev);
    if (retval) {
        hid_err(hdev, "hw open failed\n");
        goto exit_stop;
    }

    usb_disable_autosuspend(usb_dev);

    /*
     * Kick off the background serial / firmware fetch. Scheduled last: the
     * error paths below never reach here, so they need no cancel.
     */
    schedule_delayed_work(&dev->ids_work,
                          msecs_to_jiffies(RAZER_BS_IDS_FIRST_DELAY_MS));
    if (dev->usb_pid == USB_DEVICE_ID_RAZER_BLACKSHARK_V2_HS_24)
        schedule_delayed_work(&dev->link_work, 0);
    return 0;

exit_stop:
    /* A push seen through hidraw may already have queued link_work. */
    WRITE_ONCE(dev->ids_stop, true);
    hid_hw_stop(hdev);
    cancel_delayed_work_sync(&dev->link_work);
exit_free:
    razer_blackshark_remove_files(hdev);
    kfree(dev);
    return retval;
}

static void razer_blackshark_disconnect(struct hid_device *hdev)
{
    struct razer_blackshark_device *dev = hid_get_drvdata(hdev);

    /*
     * Order matters. Drop the sysfs files first so no read handler can kick
     * ids_work again. Then stop and flush both works (ids_stop makes an
     * in-flight run not reschedule and .raw_event not queue link_work;
     * cancel_delayed_work_sync waits a run out -- up to one reply timeout)
     * while the HID transport they use is still up. Once HID is stopped no
     * report can arrive, so a final cancel catches a link_work queued by a push
     * that raced the ids_stop store. Only then free dev.
     */
    razer_blackshark_remove_files(hdev);
    WRITE_ONCE(dev->ids_stop, true);
    cancel_delayed_work_sync(&dev->ids_work);
    cancel_delayed_work_sync(&dev->link_work);

    hid_hw_close(hdev);
    hid_hw_stop(hdev);
    cancel_delayed_work_sync(&dev->link_work);
    kfree(dev);
    hid_info(hdev, "Razer BlackShark disconnected\n");
}

static const struct hid_device_id razer_devices[] = {
    { HID_USB_DEVICE(USB_VENDOR_ID_RAZER, USB_DEVICE_ID_RAZER_BLACKSHARK_V2_HS_24) },
    { HID_USB_DEVICE(USB_VENDOR_ID_RAZER, USB_DEVICE_ID_RAZER_BLACKSHARK_V2_HS_USB) },
    { 0 }
};

MODULE_DEVICE_TABLE(hid, razer_devices);

static struct hid_driver razer_blackshark_driver = {
    .name = "razerblackshark",
    .id_table = razer_devices,
    .probe = razer_blackshark_probe,
    .remove = razer_blackshark_disconnect,
    .raw_event = razer_blackshark_raw_event,
};

module_hid_driver(razer_blackshark_driver);

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE(DRIVER_LICENSE);
