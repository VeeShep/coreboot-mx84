# mx84-kmod

This directory keeps the MX84-specific DKMS metadata and board driver.  The
large `igb`, `gpio-ich`, and LP5562 sources remain derived from the exact
patched Linux tree so they are not maintained as duplicate source copies.

Create a new, complete DKMS source directory with:

```sh
./prepare-source.sh /path/to/patched/linux /usr/src/mx84-kmod-6.12.94.2
```

The Linux tree must already contain this repository's MX84 Linux patches,
especially the restricted I354 MDIO change and GPIO-ICH LED-enable change.
`prepare-source.sh` refuses to overwrite an existing or case-conflicting
directory.  Build each installed kernel with DKMS before installing anything,
and keep the prior DKMS version plus the older kernel as rollback paths.

The board driver also reconstructs the factory reset input: gpio_ich hardware
offset 0, active low, `KEY_RESTART`, polled every 60 ms.  The user-space reboot
handler must remain unconfigured until a live press/release trace confirms the
input event on the running kernel.
