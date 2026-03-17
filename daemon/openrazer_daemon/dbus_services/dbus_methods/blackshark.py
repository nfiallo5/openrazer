import os

from openrazer_daemon.dbus_services import endpoint


@endpoint('razer.device.power', 'getBattery', out_sig='d')
def get_battery(self):
    """
    Get the cached battery from the headset
    """
    self.logger.debug("DBus call get_battery")

    driver_path = self.get_driver_path('charge_level')
    try:
        with open(driver_path, 'r') as f:
            value = int(f.read().strip())

        if value == 0xFF:
            return -1.0
        return float(value)
    except (OSError, ValueError) as e:
        self.logger.error("get_battery: failed to read charge_level: %s", e)
        return -1.0


@endpoint('razer.device.power', 'isCharging', out_sig='b')
def is_charging(self):
    self.logger.debug("DBus call is_charging")

    driver_path = self.get_driver_path('charge_status')

    try:
        with open(driver_path, 'r') as f:
            return int(f.read().strip()) == 1
    except (OSError, ValueError) as e:
        self.logger.error('is_charging: failed to read charge_status: %s', e)
        return False




@endpoint('razer.device.audio', 'setSidetone', in_sig='y')
def set_sidetone(self, level):
    self.logger.debug('DBus call set_sidetone %d', level)

    if not 0 <= level <= 100:
        self.logger.error("set_sidetone: level %d out of range 0-100", level)
        return 
    
    driver_path = self.get_driver_path('sidetone')
    try:
        with open(driver_path, 'w') as f:
            f.write(str(level) + '\n')
    except OSError as e:
        self.logger.error('set_sidetone: failed to write sidetone: %s', e)




