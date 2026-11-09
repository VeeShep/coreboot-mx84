#include "mesa-phy-identity.h"
#include "phy-identity.h"
#include "mx84-board-profile.h"

struct mepa_callout_ctx {
    vtss_inst_t inst;
    unsigned port;
};

static mepa_rc read_identity_register(struct mepa_callout_ctx *context,
    uint8_t reg, uint16_t *value)
{
    return vtss_miim_read(context->inst, 0,
        mx84_board_ports[context->port].miim_controller,
        mx84_board_ports[context->port].miim_addr, reg, value);
}

vtss_rc mx84_mesa_phy_identity(vtss_inst_t inst, struct mx84_mdio_window *window,
    unsigned logical_port, uint32_t *identity)
{
    if (!inst || !window || !identity || logical_port > 10)
        return VTSS_RC_ERROR;
    if (mx84_mesa_transport_begin(window))
        return VTSS_RC_ERROR;
    struct mepa_callout_ctx context = {.inst = inst, .port = logical_port};
    uint32_t result;
    vtss_init_conf_t conf;
    mepa_rc rc = MEPA_RC_ERROR;
    if (vtss_init_conf_get(inst, &conf) == VTSS_RC_OK && conf.reg_read && conf.reg_write)
        rc = mx84_phy_identity_read(read_identity_register, &context, &result);
    int unlocked = mx84_mesa_transport_end();
    if (rc != MEPA_RC_OK || unlocked)
        return VTSS_RC_ERROR;
    *identity = result;
    return VTSS_RC_OK;
}
