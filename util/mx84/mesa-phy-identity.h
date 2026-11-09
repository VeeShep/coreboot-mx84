#ifndef MX84_MESA_PHY_IDENTITY_H
#define MX84_MESA_PHY_IDENTITY_H

#include "mesa-transport.h"

/* Caller must establish sole bus ownership/recovery and set one backend deadline.
 * Holds the SDK operation scope across both target-PHY ID reads. No reset/probe
 * creation or raw opener. Only mapped copper logical ports 0–10 are accepted. */
vtss_rc mx84_mesa_phy_identity(vtss_inst_t inst, struct mx84_mdio_window *window,
    unsigned logical_port, uint32_t *identity);

#endif
