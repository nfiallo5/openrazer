#!/usr/bin/env python3
"""
Configure a Razer BlackShark V2 HS headset (equalizer, sidetone, do-not-disturb,
auto power-off, status-indicator LED) via the OpenRazer client library. Works on
both the wired connection and the 2.4GHz dongle.

Examples:
    # Show the headset, its battery level and which features it supports
    python3 blackshark_v2_hs.py

    # Equalizer on/off (presets and the curve only apply while it is on)
    python3 blackshark_v2_hs.py --eq-enable on

    # Built-in EQ preset
    python3 blackshark_v2_hs.py --eq-preset game

    # Custom 10-band EQ curve (dB gains, -9..+6: subbass..treble)
    python3 blackshark_v2_hs.py --eq 4 3 2 0 0 -1 -2 0 2 3

    # Microphone sidetone (0 = off, 1-15 = level)
    python3 blackshark_v2_hs.py --sidetone 8

    # Do Not Disturb, auto power-off (minutes, 0 disables), status LED mode
    python3 blackshark_v2_hs.py --dnd on
    python3 blackshark_v2_hs.py --auto-off 15
    python3 blackshark_v2_hs.py --status-indicator battery
"""
import argparse
import sys

from openrazer.client import DeviceManager

EQ_PRESETS = {'game': 0x07, 'music': 0x08, 'movie': 0x09, 'custom': 0xff}
EQ_PRESET_NAMES = {v: k for k, v in EQ_PRESETS.items()}
STATUS_MODES = {'connection': 1, 'battery': 2, 'warning': 3}
STATUS_MODE_NAMES = {v: k for k, v in STATUS_MODES.items()}

FEATURES = ['equalizer_enable', 'equalizer_preset', 'equalizer', 'sidetone', 'mic_mute', 'dnd',
            'idle_time', 'status_indicator', 'battery']


def find_headset():
    """Return the first BlackShark-style headset (has our vendor features)."""
    for device in DeviceManager().devices:
        if device.type == 'headset' and device.has('sidetone'):
            return device
    return None


def main():
    parser = argparse.ArgumentParser(description="Configure a Razer BlackShark V2 HS headset")
    parser.add_argument('--eq-enable', dest='eq_on', choices=['on', 'off'], metavar='on|off',
                        help="turn the equalizer engine on or off")
    parser.add_argument('--eq-preset', choices=EQ_PRESETS, help="select a built-in EQ preset")
    parser.add_argument('--eq', nargs=10, type=int, metavar='B',
                        help="10 signed dB band values for a custom EQ curve (-9..+6)")
    parser.add_argument('--sidetone', type=int, metavar='0-15', help="mic sidetone level (0 = off)")
    parser.add_argument('--dnd', choices=['on', 'off'], help="Do Not Disturb")
    parser.add_argument('--auto-off', type=int, metavar='MIN',
                        help="auto power-off after N idle minutes (0 = disabled)")
    parser.add_argument('--status-indicator', choices=STATUS_MODES,
                        help="on-headset status LED mode")
    args = parser.parse_args()

    headset = find_headset()
    if headset is None:
        print("No BlackShark V2 HS headset found (is it connected and the daemon running?)")
        sys.exit(1)

    print(f"Headset: {headset.name} (serial {headset.serial}, firmware {headset.firmware_version})")

    # With no options, report battery, current state and supported features.
    if not any([args.eq_on, args.eq_preset, args.eq, args.sidetone is not None, args.dnd,
                args.auto_off is not None, args.status_indicator]):
        if headset.has('hardware_model'):
            print(f"Hardware model: {headset.get_hardware_model()}")
        if headset.has('edition_id'):
            print(f"Edition id: {headset.get_edition_id()}")
        if headset.has('hardware_id'):
            print(f"Hardware id: {headset.get_hardware_id()}")
        if headset.has('battery'):
            charging = " (charging)" if headset.is_charging else ""
            print(f"Battery: {headset.battery_level}%{charging}")
        if headset.has('equalizer_enable'):
            print(f"Equalizer: {'on' if headset.get_equalizer_enable() else 'off'}")
        if headset.has('equalizer_preset'):
            preset = headset.get_equalizer_preset()
            print(f"EQ preset: {EQ_PRESET_NAMES.get(preset, hex(preset))}")
        if headset.has('equalizer'):
            print(f"Custom EQ curve: {headset.get_equalizer()}")
        if headset.has('sidetone'):
            print(f"Sidetone: {headset.get_sidetone()}")
        if headset.has('dnd'):
            print(f"DND: {'on' if headset.get_dnd() else 'off'}")
        if headset.has('mic_mute'):
            print(f"Mic: {'muted' if headset.get_mic_mute() else 'live'}")
        if headset.has('idle_time'):
            print(f"Auto power-off: {headset.get_idle_time() // 60} min")
        if headset.has('status_indicator'):
            mode = headset.get_status_indicator()
            print(f"Status LED: {STATUS_MODE_NAMES.get(mode, mode)}")
        print("\nSupported features:")
        for feature in FEATURES:
            print(f"  {feature:18s} {'yes' if headset.has(feature) else 'no'}")
        print("\nRun with --help to see how to change them.")
        return

    if args.eq_on:
        headset.set_equalizer_enable(args.eq_on == 'on')
        print(f"Equalizer -> {args.eq_on}")

    if args.eq_preset:
        headset.set_equalizer_preset(EQ_PRESETS[args.eq_preset])
        print(f"EQ preset -> {args.eq_preset}")

    if args.eq:
        headset.set_equalizer(args.eq)
        print(f"EQ curve  -> {args.eq}")

    if args.sidetone is not None:
        headset.set_sidetone(args.sidetone)
        print(f"Sidetone  -> {args.sidetone}")

    if args.dnd:
        headset.set_dnd(args.dnd == 'on')
        print(f"DND       -> {args.dnd}")

    if args.auto_off is not None:
        headset.set_idle_time(args.auto_off * 60)
        print(f"Auto power-off -> {args.auto_off} min")

    if args.status_indicator:
        headset.set_status_indicator(STATUS_MODES[args.status_indicator])
        print(f"Status LED -> {args.status_indicator}")


if __name__ == '__main__':
    main()
