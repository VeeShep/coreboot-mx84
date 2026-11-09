#ifndef MX84_LINUX_MII_H
#define MX84_LINUX_MII_H

#include <stdint.h>
#include <time.h>

struct ifreq;

/* Initialize all fields; descriptor and callback context remain caller-owned.
 * No descriptor is opened/owned here. The future caller must establish sole
 * bus ownership and recovery gates before providing a socket/syscall callback.
 * Set one absolute CLOCK_MONOTONIC deadline for the encompassing SDK operation;
 * do not renew it in each callback. Callback time itself needs external bounds. */
struct mx84_linux_mii {
    int fd;
    struct timespec deadline;
    void *context;
    int (*request)(void *context, int fd, unsigned long command, struct ifreq *request);
};

/* Signatures match mdio-window.h's callbacks. Only eth0 / PHY0 / registers0..3. */
int mx84_linux_mii_read16(void *context, uint16_t reg, uint16_t *value);
int mx84_linux_mii_write16(void *context, uint16_t reg, uint16_t value);

#endif
