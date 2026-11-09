#ifndef MX84_MESA_TRANSPORT_H
#define MX84_MESA_TRANSPORT_H

#include "mdio-window.h"
#include <vtss_api.h>

/* Wrap the entire SDK operation, not each callback. Begin is nonblocking;
 * end must run on the same thread on every exit. The window stays caller-owned.
 * This serializes only cooperating callers, not the vendor daemon or raw I/O.
 * Set the backend's operation deadline before begin; never renew it in callbacks. */
int mx84_mesa_transport_begin(struct mx84_mdio_window *window);
int mx84_mesa_transport_end(void);
vtss_rc mx84_mesa_reg_read(const vtss_chip_no_t chip, const u32 address, u32 *const value);
vtss_rc mx84_mesa_reg_write(const vtss_chip_no_t chip, const u32 address, const u32 value);

#endif
