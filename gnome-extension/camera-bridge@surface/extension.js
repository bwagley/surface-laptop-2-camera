import GLib from 'gi://GLib';
import Gio from 'gi://Gio';
import GObject from 'gi://GObject';
import St from 'gi://St';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as PanelMenu from 'resource:///org/gnome/shell/ui/panelMenu.js';
import * as PopupMenu from 'resource:///org/gnome/shell/ui/popupMenu.js';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

const DBUS_NAME = 'com.surface.CameraBridge';
const DBUS_PATH = '/com/surface/CameraBridge';
const DBUS_IFACE = 'com.surface.CameraBridge';

const SYSTEMD_DBUS = 'org.freedesktop.systemd1';
const SYSTEMD_PATH = '/org/freedesktop/systemd1';
const SYSTEMD_MANAGER_IFACE = 'org.freedesktop.systemd1.Manager';
const UNIT_NAME = 'camera-bridge.service';

const CameraBridgeIndicator = GObject.registerClass(
class CameraBridgeIndicator extends PanelMenu.Button {
    _init(extension) {
        super._init(0.0, 'Camera Bridge');
        this._extension = extension;

        // Panel icon
        this._icon = new St.Icon({
            icon_name: 'camera-video-symbolic',
            style_class: 'system-status-icon',
        });
        this.add_child(this._icon);

        // Track service state
        this._active = false;
        this._brightness = 1.0;

        // Toggle item
        this._toggleItem = new PopupMenu.PopupSwitchMenuItem('Camera Bridge', false);
        this._toggleItem.connect('toggled', (_item, state) => {
            this._toggleService(state);
        });
        this.menu.addMenuItem(this._toggleItem);

        // Separator
        this.menu.addMenuItem(new PopupMenu.PopupSeparatorMenuItem());

        // Brightness control: two rows stacked vertically
        //   "Camera Brightness"
        //   [v]  100%  [^]
        this._brightnessStep = 0.1;
        this._brightnessItem = new PopupMenu.PopupBaseMenuItem({
            activate: false,
            reactive: false,
        });

        const outerBox = new St.BoxLayout({
            vertical: true,
            x_expand: true,
        });

        const titleLabel = new St.Label({
            text: 'Camera Brightness',
            x_align: 1, // CENTER
            style: 'font-weight: bold;',
        });

        const controlsBox = new St.BoxLayout({
            x_expand: true,
            x_align: 2, // FILL
        });

        this._btnDown = new St.Button({
            child: new St.Icon({
                icon_name: 'go-down-symbolic',
                icon_size: 16,
            }),
            style_class: 'button',
            can_focus: true,
        });
        this._btnDown.connect('clicked', () => {
            this._adjustBrightness(-this._brightnessStep);
        });

        this._brightnessLabel = new St.Label({
            text: '100%',
            x_expand: true,
            x_align: 1, // CENTER
            y_align: 1, // CENTER
        });

        this._btnUp = new St.Button({
            child: new St.Icon({
                icon_name: 'go-up-symbolic',
                icon_size: 16,
            }),
            style_class: 'button',
            can_focus: true,
        });
        this._btnUp.connect('clicked', () => {
            this._adjustBrightness(this._brightnessStep);
        });

        controlsBox.add_child(this._btnDown);
        controlsBox.add_child(this._brightnessLabel);
        controlsBox.add_child(this._btnUp);
        outerBox.add_child(titleLabel);
        outerBox.add_child(controlsBox);
        this._brightnessItem.add_child(outerBox);
        this.menu.addMenuItem(this._brightnessItem);

        // Status label
        this._statusItem = new PopupMenu.PopupMenuItem('Status: unknown', {
            reactive: false,
            style_class: 'popup-inactive-menu-item',
        });
        this.menu.addMenuItem(this._statusItem);

        // Poll service status periodically
        this._pollId = GLib.timeout_add_seconds(GLib.PRIORITY_DEFAULT, 3,
            () => {
                this._pollStatus();
                return GLib.SOURCE_CONTINUE;
            });

        // Subscribe to BrightnessChanged signal from camera-bridge
        this._dbusSignalId = null;
        this._subscribeSignals();

        // Initial poll
        this._pollStatus();
    }

    _subscribeSignals() {
        try {
            const bus = Gio.bus_get_sync(Gio.BusType.SESSION, null);
            this._dbusSignalId = bus.signal_subscribe(
                DBUS_NAME, DBUS_IFACE, 'BrightnessChanged',
                DBUS_PATH, null, Gio.DBusSignalFlags.NONE,
                (_conn, _sender, _path, _iface, _signal, params) => {
                    const [val] = params.deep_unpack();
                    this._brightness = val;
                    const pct = Math.round(val * 100);
                    this._brightnessLabel.text = `${pct}%`;
                }
            );
        } catch (e) {
            // D-Bus not available yet, will retry on poll
        }
    }

    _pollStatus() {
        // Check systemd unit state
        try {
            const bus = Gio.bus_get_sync(Gio.BusType.SESSION, null);
            const result = bus.call_sync(
                SYSTEMD_DBUS, SYSTEMD_PATH, SYSTEMD_MANAGER_IFACE,
                'GetUnit',
                new GLib.Variant('(s)', [UNIT_NAME]),
                new GLib.VariantType('(o)'),
                Gio.DBusCallFlags.NONE, 1000, null
            );
            const [unitPath] = result.deep_unpack();

            // Read ActiveState property
            const propResult = bus.call_sync(
                SYSTEMD_DBUS, unitPath,
                'org.freedesktop.DBus.Properties', 'Get',
                new GLib.Variant('(ss)', [
                    'org.freedesktop.systemd1.Unit', 'ActiveState',
                ]),
                new GLib.VariantType('(v)'),
                Gio.DBusCallFlags.NONE, 1000, null
            );
            const activeState = propResult.deep_unpack()[0].deep_unpack();
            this._active = activeState === 'active';
            this._toggleItem.setToggleState(this._active);

            if (this._active) {
                this._icon.icon_name = 'camera-video-symbolic';
                this._statusItem.label.text = 'Status: running';
                this._syncBrightness();
            } else {
                this._icon.icon_name = 'camera-disabled-symbolic';
                this._statusItem.label.text = `Status: ${activeState}`;
            }
        } catch (e) {
            // Unit might not be loaded yet
            this._active = false;
            this._toggleItem.setToggleState(false);
            this._icon.icon_name = 'camera-disabled-symbolic';
            this._statusItem.label.text = 'Status: not loaded';
        }
    }

    _syncBrightness() {
        try {
            const bus = Gio.bus_get_sync(Gio.BusType.SESSION, null);
            const result = bus.call_sync(
                DBUS_NAME, DBUS_PATH, DBUS_IFACE,
                'GetBrightness', null,
                new GLib.VariantType('(d)'),
                Gio.DBusCallFlags.NONE, 1000, null
            );
            const [val] = result.deep_unpack();
            this._brightness = val;
            const pct = Math.round(val * 100);
            this._brightnessLabel.text = `${pct}%`;
        } catch (e) {
            // Service might not be ready yet
        }
    }

    _toggleService(start) {
        try {
            const bus = Gio.bus_get_sync(Gio.BusType.SESSION, null);
            const method = start ? 'StartUnit' : 'StopUnit';
            bus.call_sync(
                SYSTEMD_DBUS, SYSTEMD_PATH, SYSTEMD_MANAGER_IFACE,
                method,
                new GLib.Variant('(ss)', [UNIT_NAME, 'replace']),
                new GLib.VariantType('(o)'),
                Gio.DBusCallFlags.NONE, 5000, null
            );

            // Poll status after a short delay to let service start/stop
            GLib.timeout_add(GLib.PRIORITY_DEFAULT, 1500, () => {
                this._pollStatus();
                return GLib.SOURCE_REMOVE;
            });
        } catch (e) {
            Main.notifyError('Camera Bridge',
                `Failed to ${start ? 'start' : 'stop'} service: ${e.message}`);
            // Revert toggle
            this._toggleItem.setToggleState(!start);
        }
    }

    _adjustBrightness(delta) {
        const newVal = Math.round(Math.max(0.0, Math.min(3.0, this._brightness + delta)) * 100) / 100;
        this._brightness = newVal;
        const pct = Math.round(newVal * 100);
        this._brightnessLabel.text = `${pct}%`;
        this._setBrightness(newVal);
    }

    _setBrightness(value) {
        if (!this._active)
            return;

        try {
            const bus = Gio.bus_get_sync(Gio.BusType.SESSION, null);
            bus.call(
                DBUS_NAME, DBUS_PATH, DBUS_IFACE,
                'SetBrightness',
                new GLib.Variant('(d)', [value]),
                null, Gio.DBusCallFlags.NONE, 1000, null, null
            );
        } catch (e) {
            // Silently fail — service may have just stopped
        }
    }

    destroy() {
        if (this._pollId) {
            GLib.source_remove(this._pollId);
            this._pollId = null;
        }

        if (this._dbusSignalId !== null) {
            try {
                const bus = Gio.bus_get_sync(Gio.BusType.SESSION, null);
                bus.signal_unsubscribe(this._dbusSignalId);
            } catch (e) {
                // Ignore
            }
            this._dbusSignalId = null;
        }

        super.destroy();
    }
});

export default class CameraBridgeExtension extends Extension {
    enable() {
        this._indicator = new CameraBridgeIndicator(this);
        Main.panel.addToStatusArea(this.metadata.uuid, this._indicator);
    }

    disable() {
        if (this._indicator) {
            this._indicator.destroy();
            this._indicator = null;
        }
    }
}
