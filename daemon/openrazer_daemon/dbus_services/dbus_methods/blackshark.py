# SPDX-License-Identifier: GPL-2.0-or-later

"""
Module for Razer BlackShark V2 HS headset methods

The functions carry a blackshark_ prefix because the daemon resolves METHODS by
function name across every dbus_methods module, and headset_audio.py already
defines the unprefixed names, with a different wire format, for the BlackShark
V2 Pro 2.4.
"""
from openrazer_daemon.dbus_services import endpoint


@endpoint('razer.device.misc', 'getHardwareModel', out_sig='s')
def blackshark_get_hardware_model(self):
    """
    Get the headset's own product id as a 4-digit hex string. Over the 2.4GHz
    dongle this identifies the paired headset rather than the dongle.

    :return: product id (e.g. "056e")
    :rtype: str
    """
    self.logger.debug("DBus call blackshark_get_hardware_model")

    driver_path = self.get_driver_path('hw_model')
    with open(driver_path, 'r') as driver_file:
        return driver_file.read().strip()


@endpoint('razer.device.misc', 'getEditionId', out_sig='y')
def blackshark_get_edition_id(self):
    """
    Get the headset's edition id, identifying the product variant. Special
    editions and colourways share a product id but differ here.

    :return: edition id (0 on a standard-edition unit)
    :rtype: int
    """
    self.logger.debug("DBus call blackshark_get_edition_id")

    driver_path = self.get_driver_path('edition_id')
    with open(driver_path, 'r') as driver_file:
        return int(driver_file.read().strip())


@endpoint('razer.device.misc', 'getHardwareId', out_sig='y')
def blackshark_get_hardware_id(self):
    """
    Get the headset's board revision, distinct from the product id returned by
    getHardwareModel.

    :return: hardware id (0 on the initial revision)
    :rtype: int
    """
    self.logger.debug("DBus call blackshark_get_hardware_id")

    driver_path = self.get_driver_path('hardware_id')
    with open(driver_path, 'r') as driver_file:
        return int(driver_file.read().strip())


@endpoint('razer.device.misc', 'setSidetone', in_sig='y')
def blackshark_set_sidetone(self, level):
    """
    Set microphone sidetone level.

    :param level: 0 disables sidetone, 1-15 enables it at the given level
    :type level: int
    """
    self.logger.debug("DBus call blackshark_set_sidetone")

    if level > 15:
        level = 15

    driver_path = self.get_driver_path('sidetone')
    with open(driver_path, 'wb') as driver_file:
        driver_file.write(bytes([level]))


@endpoint('razer.device.misc', 'getSidetone', out_sig='y')
def blackshark_get_sidetone(self):
    """
    Get the current sidetone level (0 = off, 1-15 = active level).

    :return: sidetone level
    :rtype: int
    """
    self.logger.debug("DBus call blackshark_get_sidetone")

    driver_path = self.get_driver_path('sidetone')
    with open(driver_path, 'rb') as driver_file:
        return driver_file.read(1)[0]


@endpoint('razer.device.misc', 'setEqualizerEnable', in_sig='b')
def blackshark_set_equalizer_enable(self, enable):
    """
    Enable/disable the equalizer engine. The preset and custom curve stay stored
    while it is off, but are not applied to the audio.

    :param enable: True to enable
    :type enable: bool
    """
    self.logger.debug("DBus call blackshark_set_equalizer_enable")

    driver_path = self.get_driver_path('equalizer_enable')
    with open(driver_path, 'wb') as driver_file:
        driver_file.write(bytes([1 if enable else 0]))


@endpoint('razer.device.misc', 'getEqualizerEnable', out_sig='b')
def blackshark_get_equalizer_enable(self):
    """
    Get whether the equalizer engine is active.

    :return: True if enabled
    :rtype: bool
    """
    self.logger.debug("DBus call blackshark_get_equalizer_enable")

    driver_path = self.get_driver_path('equalizer_enable')
    with open(driver_path, 'rb') as driver_file:
        return bool(driver_file.read(1)[0])


@endpoint('razer.device.misc', 'setEqualizerPreset', in_sig='y')
def blackshark_set_equalizer_preset(self, preset):
    """
    Select a built-in equalizer preset.

    :param preset: preset code (0x07 game, 0x08 music, 0x09 movie, 0xff custom)
    :type preset: int
    """
    self.logger.debug("DBus call blackshark_set_equalizer_preset")

    driver_path = self.get_driver_path('equalizer_preset')
    with open(driver_path, 'wb') as driver_file:
        driver_file.write(bytes([preset & 0xff]))


@endpoint('razer.device.misc', 'getEqualizerPreset', out_sig='y')
def blackshark_get_equalizer_preset(self):
    """
    Get the current equalizer preset code (0x07 game, 0x08 music, 0x09 movie,
    0xff custom).

    :return: preset code
    :rtype: int
    """
    self.logger.debug("DBus call blackshark_get_equalizer_preset")

    driver_path = self.get_driver_path('equalizer_preset')
    with open(driver_path, 'rb') as driver_file:
        return driver_file.read(1)[0]


@endpoint('razer.device.misc', 'setEqualizer', in_sig='ai')
def blackshark_set_equalizer(self, bands):
    """
    Set the 10-band custom equalizer curve. Each band is a signed gain in dB
    (hardware range -9..+6, values outside it saturate). Implicitly selects the
    custom preset.

    :param bands: exactly 10 signed band values
    :type bands: list of int
    """
    self.logger.debug("DBus call blackshark_set_equalizer")

    if len(bands) != 10:
        raise ValueError("Equalizer requires exactly 10 bands")

    curve = []
    for band in bands:
        band = int(band)
        if band < -128:
            band = -128
        elif band > 127:
            band = 127
        curve.append(band & 0xff)

    driver_path = self.get_driver_path('equalizer')
    with open(driver_path, 'wb') as driver_file:
        driver_file.write(bytes(curve))


@endpoint('razer.device.misc', 'getEqualizer', out_sig='ai')
def blackshark_get_equalizer(self):
    """
    Get the stored custom 10-band equalizer curve as signed dB gains. Reflects
    the custom curve regardless of the currently selected preset.

    :return: 10 signed band values
    :rtype: list of int
    """
    self.logger.debug("DBus call blackshark_get_equalizer")

    driver_path = self.get_driver_path('equalizer')
    with open(driver_path, 'rb') as driver_file:
        data = driver_file.read(10)
    return [b - 256 if b > 127 else b for b in data]


@endpoint('razer.device.misc', 'getMicMute', out_sig='b')
def blackshark_get_mic_mute(self):
    """
    Get the microphone mute state (hardware mute button). Read-only.

    :return: True if the mic is muted
    :rtype: bool
    """
    self.logger.debug("DBus call blackshark_get_mic_mute")

    driver_path = self.get_driver_path('mic_mute')
    with open(driver_path, 'rb') as driver_file:
        return bool(driver_file.read(1)[0])


@endpoint('razer.device.misc', 'setDnd', in_sig='y')
def blackshark_set_dnd(self, enable):
    """
    Enable/disable Do Not Disturb.

    :param enable: 0 = off, non-zero = on
    :type enable: int
    """
    self.logger.debug("DBus call blackshark_set_dnd")

    driver_path = self.get_driver_path('dnd')
    with open(driver_path, 'wb') as driver_file:
        driver_file.write(bytes([1 if enable else 0]))


@endpoint('razer.device.misc', 'getDnd', out_sig='b')
def blackshark_get_dnd(self):
    """
    Get the Do Not Disturb state.

    :return: True if enabled
    :rtype: bool
    """
    self.logger.debug("DBus call blackshark_get_dnd")

    driver_path = self.get_driver_path('dnd')
    with open(driver_path, 'rb') as driver_file:
        return bool(driver_file.read(1)[0])


# Auto power-off is exposed through the standard OpenRazer power API
# (getIdleTime/setIdleTime -> device_idle_time), not a bespoke method.


@endpoint('razer.device.misc', 'setStatusIndicator', in_sig='y')
def blackshark_set_status_indicator(self, mode):
    """
    Set the on-headset status LED mode.

    :param mode: 1 = connection status, 2 = battery status,
                 3 = battery warning only
    :type mode: int
    """
    self.logger.debug("DBus call blackshark_set_status_indicator")

    driver_path = self.get_driver_path('status_indicator')
    with open(driver_path, 'wb') as driver_file:
        driver_file.write(bytes([mode & 0xff]))


@endpoint('razer.device.misc', 'getStatusIndicator', out_sig='y')
def blackshark_get_status_indicator(self):
    """
    Get the current on-headset status LED mode (0 = off, 1 = connection,
    2 = battery status, 3 = battery warning only).

    :return: status indicator mode
    :rtype: int
    """
    self.logger.debug("DBus call blackshark_get_status_indicator")

    driver_path = self.get_driver_path('status_indicator')
    with open(driver_path, 'rb') as driver_file:
        return driver_file.read(1)[0]
