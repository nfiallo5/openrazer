/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Razer BlackShark V2 HS headset driver
 *
 * These MediaTek-based headsets do not use the classic 90-byte Razer report.
 * They accept a 64-byte vendor report (HID report id 0x02, sent as an OUTPUT
 * report) and answer reads with an input report on the same report id.
 */

#ifndef DRIVER_RAZERBLACKSHARK_H_
#define DRIVER_RAZERBLACKSHARK_H_

#define USB_DEVICE_ID_RAZER_BLACKSHARK_V2_HS_24 0x0565 // 2.4GHz wireless dongle
#define USB_DEVICE_ID_RAZER_BLACKSHARK_V2_HS_USB 0x056e // wired / charging mode

/*
 * 64-byte vendor report layout:
 *   [0]=0x02  [2]=0x60  [6]=0x04+len  [10]=cmd  [12]=len  [13..]=payload (zero-pad).
 * The wired unit takes this frame as-is. The 2.4GHz dongle additionally needs a
 * command class at byte[9] and a trailing XOR checksum at byte[62] before it acts
 * on the frame (see razer_bs_apply_framing()).
 *
 * byte[9] selects which device the command addresses, not a transfer direction:
 * RAZER_BS_CLASS_HEADSET has the receiver relay the command to the headset, while
 * RAZER_BS_CLASS_RECEIVER addresses the receiver itself. The two have separate
 * register spaces, so the same opcode means different things under each class.
 */
#define RAZER_BS_REPORT_LEN     64
#define RAZER_BS_REPORT_ID      0x02
#define RAZER_BS_CRC_INDEX      62   // XOR of bytes [0..61] (dongle framing only)
#define RAZER_BS_CHANNEL        0x60 // fixed channel constant (byte 2)
#define RAZER_BS_LEN_INDEX      6    // body length
#define RAZER_BS_BODY_INDEX     9    // command class, then the body
#define RAZER_BS_CLASS_HEADSET  0x80 // relay the command to the headset
#define RAZER_BS_CLASS_RECEIVER 0x00 // address the receiver itself
#define RAZER_BS_PAYLOAD_INDEX  13   // payload starts here; length at byte 12

/*
 * The status-indicator LED takes a different frame from the channel above: the
 * body length at byte[6] and the body {class, opcode, 0x00, count, arg} starting
 * at byte[9], with the XOR checksum. The mode only takes effect when the set is
 * preceded by a prepare (the matching read opcode with a zero count), the same
 * pattern the device expects for its other stored settings. The LED lives on the
 * receiver, so both the set and the read back use RAZER_BS_CLASS_RECEIVER.
 */
#define RAZER_BS_FW_STATUS_R    0x66 // status-indicator read opcode (used as prepare)

/*
 * Command bytes. A read command is the matching write command with the high bit
 * cleared (read = write & 0x7f), e.g. sidetone enable write 0x98 / read 0x18.
 */
#define RAZER_BS_FW_SERIAL        0x00 // read serial number (ASCII string)
#define RAZER_BS_FW_EDITION_ID    0x01 // read edition id (product variant)
#define RAZER_BS_FW_VERSION       0x02 // read firmware version ([major, minor, ...])
#define RAZER_BS_FW_HW_MODEL      0x03 // read product id (2 bytes, high byte first)
#define RAZER_BS_FW_HARDWARE_ID   0x04 // read hardware id (board revision)
#define RAZER_BS_FW_EQ_READ       0x13 // read current EQ preset code
#define RAZER_BS_FW_EQ_ENABLE_R   0x1e // read equalizer engine enable
#define RAZER_BS_FW_EQ_ENABLE     0x9e // write equalizer engine enable (1 = on)
#define RAZER_BS_FW_EQ_CURVE_R    0x15 // read stored custom 10-band curve
#define RAZER_BS_FW_SIDETONE_EN_R 0x18 // read sidetone enable
#define RAZER_BS_FW_SIDETONE_LV_R 0x19 // read sidetone level
#define RAZER_BS_FW_BATTERY       0x21 // read battery level (percent)
#define RAZER_BS_FW_CHARGING      0x2a // read charging state (1 = charging)
#define RAZER_BS_FW_DND_R         0x27 // read do-not-disturb state
#define RAZER_BS_FW_AUTO_OFF_R    0x2c // read auto-power-off timer (minutes)
#define RAZER_BS_FW_MIC_MUTE      0x55 // read mic-mute state (1 = muted)
#define RAZER_BS_FW_EQ_PRESET     0x93 // write EQ preset (payload = preset code)
#define RAZER_BS_FW_EQ_CURVE      0x95 // write custom 10-band curve
#define RAZER_BS_FW_SIDETONE_EN   0x98 // write sidetone enable (payload 0/1)
#define RAZER_BS_FW_SIDETONE_LV   0x99 // write sidetone level (payload 0..15)
#define RAZER_BS_FW_DND           0xa7 // write do-not-disturb (payload 0/1)
#define RAZER_BS_FW_AUTO_OFF      0xac // write auto-power-off timer (minutes)
#define RAZER_BS_FW_STATUS        0xe6 // write status-indicator LED mode

/*
 * Identified but not exposed:
 *   0x20 reads 1 while the headset is on the 2.4GHz link and 0 once it leaves,
 *   including while a Bluetooth connection is up at the same time, so it tracks
 *   the receiver link rather than naming a transport;
 *   0x40 basestation version, which only answers under RAZER_BS_CLASS_HEADSET.
 * Off the 2.4GHz link the headset stops answering relayed commands entirely,
 * so only the receiver-class registers still respond.
 * The receiver also answers a separate command channel used only for firmware
 * updates; it stays silent during normal operation and is deliberately untouched.
 */

#define RAZER_BS_SIDETONE_MAX   15 // sidetone level is a 4-bit field

/* EQ presets (payload of RAZER_BS_FW_EQ_PRESET) */
#define RAZER_BS_EQ_GAME        0x07
#define RAZER_BS_EQ_MUSIC       0x08
#define RAZER_BS_EQ_MOVIE       0x09
#define RAZER_BS_EQ_CUSTOM      0xff
#define RAZER_BS_EQ_BANDS       10 // custom EQ curve = 10 signed bytes

/*
 * The device applies each stored band value directly as the gain in dB, but a
 * curve write is biased by +5 (it stores written - 5). So to apply a gain the
 * driver writes gain + RAZER_BS_EQ_WRITE_OFFSET, and reads the stored gain back
 * as-is. Hardware gain range is RAZER_BS_EQ_GAIN_MIN..RAZER_BS_EQ_GAIN_MAX dB;
 * writes outside it saturate.
 */
#define RAZER_BS_EQ_WRITE_OFFSET 5
#define RAZER_BS_EQ_GAIN_MIN     (-9)
#define RAZER_BS_EQ_GAIN_MAX     6

#define RAZER_BS_WAIT_US          3000 // fsleep after each control message
#define RAZER_BS_REPLY_TIMEOUT_MS 500  // how long to wait for a read reply report

struct razer_blackshark_device {
    struct usb_device *usb_dev;
    struct hid_device *hdev;
    struct mutex lock;
    unsigned int usb_pid;
    char serial[23];
    unsigned char firmware_version[3];

    /*
     * Read replies arrive asynchronously as an input report handled by
     * .raw_event; the requester waits on this completion. Only one read is in
     * flight at a time (serialised by @lock).
     */
    struct completion fw_reply;
    bool fw_wait;                   // a read is awaiting its reply
    __u8 fw_reply_cmd;              // command byte expected echoed at reply[10]
    __u8 fw_reply_val;              // captured reply value (reply[13])
    __u8 fw_reply_len;              // captured reply payload length (reply[12])
    __u8 fw_reply_buf[16];          // captured reply payload (reply[13..])
    bool ids_read;                  // real serial + firmware fetched
};

#endif /* DRIVER_RAZERBLACKSHARK_H_ */
