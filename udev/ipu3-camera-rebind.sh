#!/bin/sh
# Rebind ov9734 after ipu3_cio2 loads.
#
# Problem: ipu_bridge_init() runs during ipu3_cio2 probe and creates the
# fwnode software node connecting OV9734 to the IPU3 CIO2. But ov9734 probes
# before ipu3_cio2, so ov9734_check_hwcfg() finds no endpoint and fails.
#
# Fix: after ipu3_cio2 binds (triggering ipu_bridge_init), rebind ov9734.
#
# NOTE: Do NOT unbind/rebind INT3472:00. The int3472-discrete driver uses
# acpi_dev_clear_dependencies() which is one-shot — rebind always fails with
# "INT3472 seems to have no dependents." Leave INT3472:00 bound from boot.

I2C_OV9734=/sys/bus/i2c/drivers/ov9734

if [ -e "$I2C_OV9734/i2c-OVTI9734:00" ]; then
    echo "i2c-OVTI9734:00" > "$I2C_OV9734/unbind"
fi
echo "i2c-OVTI9734:00" > "$I2C_OV9734/bind"
