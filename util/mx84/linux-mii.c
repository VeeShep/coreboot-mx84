#include "linux-mii.h"
#include <errno.h>
#include <linux/if.h>
#include <linux/mii.h>
#include <linux/sockios.h>
#include <stdbool.h>
#include <string.h>

_Static_assert(sizeof(struct mii_ioctl_data) <= sizeof(((struct ifreq *)0)->ifr_ifru),
               "MII data must fit the inline ifreq union");

static int check_deadline(struct mx84_linux_mii *mii)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return errno ? errno : EIO;
    return now.tv_sec > mii->deadline.tv_sec ||
           (now.tv_sec == mii->deadline.tv_sec && now.tv_nsec >= mii->deadline.tv_nsec)
           ? ETIMEDOUT : 0;
}

static int transfer(struct mx84_linux_mii *mii, uint16_t reg, uint16_t *value, bool write)
{
    if (!mii || !value || !mii->request || mii->fd < 0 || reg > 3 ||
        mii->deadline.tv_sec < 0 || mii->deadline.tv_nsec < 0 ||
        mii->deadline.tv_nsec >= 1000000000L)
        return EINVAL;
    int rc = check_deadline(mii);
    if (rc)
        return rc;
    struct ifreq request = {0};
    struct mii_ioctl_data data = {.phy_id = 0, .reg_num = reg, .val_in = write ? *value : 0};
    memcpy(request.ifr_name, "eth0", sizeof("eth0"));
    /* if_mii() uses the union inline, not memory pointed to by ifr_data. */
    memcpy(&request.ifr_ifru, &data, sizeof(data));
    errno = 0;
    rc = mii->request(mii->context, mii->fd,
                      write ? SIOCSMIIREG : SIOCGMIIREG, &request);
    if (rc != 0)
        return rc < 0 ? (errno ? errno : EIO) : EPROTO;
    rc = check_deadline(mii);
    if (rc)
        return rc;
    memcpy(&data, &request.ifr_ifru, sizeof(data));
    if (memcmp(request.ifr_name, "eth0", sizeof("eth0")) != 0 ||
        data.phy_id != 0 || data.reg_num != reg)
        return EPROTO;
    if (!write)
        *value = data.val_out;
    return 0;
}

int mx84_linux_mii_read16(void *context, uint16_t reg, uint16_t *value)
{
    return transfer(context, reg, value, false);
}

int mx84_linux_mii_write16(void *context, uint16_t reg, uint16_t value)
{
    return transfer(context, reg, &value, true);
}
