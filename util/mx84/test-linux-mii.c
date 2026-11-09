#include "linux-mii.h"
#include "mdio-window.h"
#include <assert.h>
#include <errno.h>
#include <linux/if.h>
#include <linux/mii.h>
#include <linux/sockios.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#ifdef NDEBUG
#error "The Linux MII checks require assertions enabled"
#endif

_Static_assert(SIOCGMIIREG == 0x8948 && SIOCSMIIREG == 0x8949,
               "Linux MII commands must match the recovered transport ABI");

struct operation { unsigned long command; uint16_t reg, value; };
struct fake_ioctl {
    const struct operation *operations;
    unsigned count, index;
    int error, result;
    bool expire, corrupt;
    struct mx84_linux_mii *mii;
};

static int fake_request(void *context, int fd, unsigned long command, struct ifreq *request)
{
    struct fake_ioctl *f = context;
    assert(fd == 7 && f->index < f->count);
    const struct operation *op = &f->operations[f->index++];
    const char name[IFNAMSIZ] = "eth0";
    assert(memcmp(request->ifr_name, name, sizeof(name)) == 0);
    struct mii_ioctl_data data;
    memcpy(&data, &request->ifr_ifru, sizeof(data));
    assert(command == op->command && data.phy_id == 0 && data.reg_num == op->reg);
    assert(data.val_in == (command == SIOCSMIIREG ? op->value : 0));
    assert(data.val_out == 0);
    const unsigned char *bytes = (const void *)&request->ifr_ifru;
    for (unsigned i = sizeof(data); i < sizeof(request->ifr_ifru); ++i)
        assert(bytes[i] == 0);
    if (f->error) { errno = f->error; return -1; }
    if (f->result) return f->result;
    if (f->expire) f->mii->deadline = (struct timespec){0};
    if (f->corrupt) data.phy_id = 1;
    data.val_out = op->value;
    memcpy(&request->ifr_ifru, &data, sizeof(data));
    return 0;
}

static void fresh(struct mx84_linux_mii *mii, struct fake_ioctl *fake)
{
    fake->index = 0;
    fake->mii = mii;
    mii->fd = 7;
    mii->context = fake;
    mii->request = fake_request;
    assert(clock_gettime(CLOCK_MONOTONIC, &mii->deadline) == 0);
    mii->deadline.tv_sec += 60;
}

int main(void)
{
    const struct operation sequence[] = {
        {SIOCSMIIREG, 0, 0x5678}, {SIOCSMIIREG, 1, 0x1234},
        {SIOCGMIIREG, 2, 0xdead}, {SIOCGMIIREG, 3, 0xbeef},
        {SIOCGMIIREG, 2, 0xcdef}, {SIOCGMIIREG, 3, 0x89ab},
        {SIOCSMIIREG, 0, 0x3040}, {SIOCSMIIREG, 1, 0x1020},
        {SIOCSMIIREG, 2, 0xba98}, {SIOCSMIIREG, 3, 0xfedc},
    };
    struct fake_ioctl fake = {.operations = sequence, .count = 10};
    struct mx84_linux_mii mii;
    fresh(&mii, &fake);
    struct mx84_mdio_window window;
    assert(mx84_mdio_window_init(&window, &mii, mx84_linux_mii_read16,
                                 mx84_linux_mii_write16) == 0);
    uint32_t value;
    assert(mx84_mdio_read32(&window, 0x12345678, &value) == 0 && value == 0x89abcdef);
    assert(mx84_mdio_write32(&window, 0x10203040, 0xfedcba98) == 0);
    assert(fake.index == 10);

    /* A failed ABI transfer must propagate through the complete window, not
     * become an all-ones read or permit the remaining selector/data writes. */
    fresh(&mii, &fake);
    fake.error = EIO;
    value = 0x11111111;
    assert(mx84_mdio_read32(&window, 0x12345678, &value) == EIO);
    assert(fake.index == 1 && value == 0x11111111);
    fake.error = 0;
    assert(mx84_mdio_window_destroy(&window) == 0);

    const struct operation one = {SIOCGMIIREG, 2, 0xffff};
    fake.operations = &one; fake.count = 1;
    const int errors[] = {EIO, EINTR, ENODEV, EPERM};
    for (unsigned i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        fresh(&mii, &fake); fake.error = errors[i];
        uint16_t v = 0x1234;
        assert(mx84_linux_mii_read16(&mii, 2, &v) == errors[i]);
        assert(v == 0x1234 && fake.index == 1);
    }
    fake.error = 0;
    uint16_t v = 0x1234;
    fresh(&mii, &fake); fake.result = -1;
    assert(mx84_linux_mii_read16(&mii, 2, &v) == EIO && v == 0x1234);
    fresh(&mii, &fake); fake.result = 1;
    assert(mx84_linux_mii_read16(&mii, 2, &v) == EPROTO && v == 0x1234);
    fake.result = 0;
    fresh(&mii, &fake); fake.corrupt = true;
    assert(mx84_linux_mii_read16(&mii, 2, &v) == EPROTO && v == 0x1234);
    fake.corrupt = false;
    fresh(&mii, &fake); fake.expire = true;
    assert(mx84_linux_mii_read16(&mii, 2, &v) == ETIMEDOUT && v == 0x1234);
    fake.expire = false;
    fresh(&mii, &fake); mii.deadline = (struct timespec){0};
    assert(mx84_linux_mii_read16(&mii, 2, &v) == ETIMEDOUT && fake.index == 0);
    fresh(&mii, &fake); mii.deadline.tv_nsec = 1000000000L;
    assert(mx84_linux_mii_read16(&mii, 2, &v) == EINVAL && fake.index == 0);
    fresh(&mii, &fake); mii.fd = -1;
    assert(mx84_linux_mii_read16(&mii, 2, &v) == EINVAL && fake.index == 0);
    fresh(&mii, &fake); mii.request = NULL;
    assert(mx84_linux_mii_read16(&mii, 2, &v) == EINVAL && fake.index == 0);
    fresh(&mii, &fake);
    assert(mx84_linux_mii_read16(NULL, 2, &v) == EINVAL);
    assert(mx84_linux_mii_read16(&mii, 2, NULL) == EINVAL);
    assert(mx84_linux_mii_read16(&mii, 4, &v) == EINVAL);
    assert(mx84_linux_mii_write16(&mii, 31, 0xffff) == EINVAL && fake.index == 0);
    assert(mx84_linux_mii_read16(&mii, 2, &v) == 0 && v == 0xffff);
    puts("PASS: Linux inline MII ABI, PHY/interface restrictions, protocol integration, errors and deadlines; fake I/O only");
    return 0;
}
