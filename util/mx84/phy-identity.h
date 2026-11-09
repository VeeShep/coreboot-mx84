#ifndef MX84_PHY_IDENTITY_H
#define MX84_PHY_IDENTITY_H

#include <microchip/ethernet/phy/api.h>

/* Read target-PHY Clause22 ID registers, not switch-window MDIO registers.
 * Caller provides serialization, bounded I/O and the selected PHY context.
 * No PHY creation/reset, model selection or retries. Output changes only on success. */
mepa_rc mx84_phy_identity_read(mepa_miim_read_t read,
    struct mepa_callout_ctx *context, uint32_t *identity);

#endif
