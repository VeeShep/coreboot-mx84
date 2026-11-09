/* Linux-only whole-stack fixture. No socket opener or real ioctl callback. */
#include <assert.h>
#include <errno.h>
#include <linux/if.h>
#include <linux/mii.h>
#include <linux/sockios.h>
#include <string.h>
#include <stdio.h>
#include <vtss_luton26_cil.h>
#include "linux-mii.h"
#include "mesa-phy-identity.h"

struct fake_controller {
    unsigned calls, fail_at, reads, polls, phy, reg;
    uint32_t address, data, latched, identity;
};

static int fake_ioctl(void *context, int fd, unsigned long command, struct ifreq *request)
{
    struct fake_controller *bus = context;
    struct mii_ioctl_data data;
    assert(fd == 7 && strcmp(request->ifr_name, "eth0") == 0);
    memcpy(&data, &request->ifr_ifru, sizeof(data));
    assert(data.phy_id == 0 && data.reg_num <= 3);
    if (++bus->calls == bus->fail_at) {
        errno = EIO;
        return -1;
    }
    if (command == SIOCSMIIREG) {
        switch (data.reg_num) {
        case 0: bus->address = (bus->address & 0xffff0000) | data.val_in; break;
        case 1:
            bus->address = (bus->address & 0xffff) | ((uint32_t)data.val_in << 16);
            bus->reads = 0;
            break;
        case 2: bus->data = data.val_in; break;
        case 3:
            bus->data |= (uint32_t)data.val_in << 16;
            assert(bus->address == VTSS_DEVCPU_GCB_MIIM_MII_CMD(0));
            assert(VTSS_X_DEVCPU_GCB_MIIM_MII_CMD_MIIM_CMD_OPR_FIELD(bus->data) == 2);
            assert(bus->data & VTSS_F_DEVCPU_GCB_MIIM_MII_CMD_MIIM_CMD_VLD);
            assert(VTSS_X_DEVCPU_GCB_MIIM_MII_CMD_MIIM_CMD_PHYAD(bus->data) == bus->phy);
            bus->reg = VTSS_X_DEVCPU_GCB_MIIM_MII_CMD_MIIM_CMD_REGAD(bus->data);
            assert(bus->reg == 2 || bus->reg == 3);
            bus->polls = 0;
            break;
        }
    } else {
        assert(command == SIOCGMIIREG && bus->reads < 4);
        assert(data.reg_num == 2 + bus->reads % 2);
        if (!bus->reads) {
            if (bus->address == VTSS_DEVCPU_GCB_MIIM_MII_STATUS(0))
                bus->latched = bus->polls++ < 2 ? VTSS_F_DEVCPU_GCB_MIIM_MII_STATUS_MIIM_STAT_BUSY : 0;
            else {
                assert(bus->address == VTSS_DEVCPU_GCB_MIIM_MII_DATA(0));
                unsigned half = bus->reg == 2 ? bus->identity >> 16 : bus->identity & 0xffff;
                bus->latched = VTSS_F_DEVCPU_GCB_MIIM_MII_DATA_MIIM_DATA_RDDATA(half);
            }
        }
        data.val_out = bus->reads < 2 ? 0xaaaa :
                       (data.reg_num == 2 ? bus->latched & 0xffff : bus->latched >> 16);
        ++bus->reads;
        memcpy(&request->ifr_ifru, &data, sizeof(data));
    }
    return 0;
}

void mx84_check_phy_stack(vtss_inst_t inst)
{
    struct fake_controller bus = {0};
    struct mx84_linux_mii mii = {.fd = 7, .context = &bus, .request = fake_ioctl};
    assert(clock_gettime(CLOCK_MONOTONIC, &mii.deadline) == 0);
    mii.deadline.tv_sec += 30;
    struct mx84_mdio_window window;
    assert(mx84_mdio_window_init(&window, &mii, mx84_linux_mii_read16, mx84_linux_mii_write16) == 0);
    vtss_state_t *state = NULL;
    assert(vtss_inst_check(inst, &state) == VTSS_RC_OK);
    vtss_init_conf_t saved = state->init_conf;
    state->init_conf.reg_read = mx84_mesa_reg_read;
    state->init_conf.reg_write = mx84_mesa_reg_write;
    state->init_conf.spi_bus = FALSE;
    for (unsigned phy = 0; phy <= 10; ++phy) {
        bus = (struct fake_controller){.phy = phy, .identity = 0x12345678};
        uint32_t identity = 42;
        assert(mx84_mesa_phy_identity(inst, &window, phy, &identity) == VTSS_RC_OK);
        assert(identity == bus.identity && bus.calls == 56);
    }
    for (unsigned failure = 1; failure <= 56; ++failure) {
        bus = (struct fake_controller){.phy = 10, .identity = 0x12345678, .fail_at = failure};
        uint32_t identity = 42;
        assert(mx84_mesa_phy_identity(inst, &window, 10, &identity) == VTSS_RC_ERROR);
        assert(identity == 42 && bus.calls == failure);
        assert(mx84_mesa_transport_end() == EINVAL);
    }
    bus = (struct fake_controller){.identity = 0x12345678};
    mii.deadline = (struct timespec){0};
    uint32_t identity = 42;
    assert(mx84_mesa_phy_identity(inst, &window, 0, &identity) == VTSS_RC_ERROR);
    assert(identity == 42 && bus.calls == 0);
    assert(mx84_mesa_transport_end() == EINVAL);
    state->init_conf = saved;
    assert(mx84_mdio_window_destroy(&window) == 0);
    puts("PASS: complete PHY/SDK/MDIO/Linux stack, 11 copper addresses, 56 ioctl faults and expired deadline; simulated I/O only");
}
