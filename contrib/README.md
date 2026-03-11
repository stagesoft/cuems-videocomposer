# Contrib: systemd service (for cuems-commons)

The systemd unit **cuems-videocomposer.service** is intended to be shipped and installed by **cuems-commons**, not by the cuems-videocomposer Debian package.

## DRI / GPU

The service is set up for DRI:

- Runs **After=graphical.target** (display and GPU available).
- Runs as **User=cuems** with **SupplementaryGroups=video render** for `/dev/dri` and GPU access.
- **PrivateDevices=no** so the process can see `/dev/dri/*` and `/dev/snd/*`.

## Install (from cuems-common)

1. Create the service user if needed:
   ```bash
   sudo useradd -r -s /usr/sbin/nologin -d /var/lib/cuems -m cuems
   sudo usermod -aG video,render cuems
   ```
2. Install the unit (e.g. from cuems-commons package):
   ```bash
   install -m 644 cuems-videocomposer.service /lib/systemd/system/
   systemctl daemon-reload
   ```
3. Enable at boot:
   ```bash
   sudo systemctl enable cuems-videocomposer
   sudo systemctl start cuems-videocomposer
   ```

## Options

- To disable the startup splash when running at boot, use in the unit:
  `ExecStart=/usr/bin/cuems-videocomposer --no-splash`
- Adjust **User**/ **Group** and **HOME** if your distro uses a different service user.
