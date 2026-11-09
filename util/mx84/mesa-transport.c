#include "mesa-transport.h"
#include <errno.h>

static pthread_mutex_t operation = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local struct mx84_mdio_window *current;

int mx84_mesa_transport_begin(struct mx84_mdio_window *window)
{
    if (!window)
        return EINVAL;
    if (current)
        return EBUSY;
    int rc = pthread_mutex_trylock(&operation);
    if (!rc)
        current = window;
    return rc;
}

int mx84_mesa_transport_end(void)
{
    if (!current)
        return EINVAL;
    current = NULL;
    return pthread_mutex_unlock(&operation);
}

vtss_rc mx84_mesa_reg_read(const vtss_chip_no_t chip, const u32 address, u32 *const value)
{
    if (!current || chip != 0 || !value)
        return VTSS_RC_ERROR;
    uint32_t result;
    if (mx84_mdio_read32(current, address, &result))
        return VTSS_RC_ERROR;
    *value = result;
    return VTSS_RC_OK;
}

vtss_rc mx84_mesa_reg_write(const vtss_chip_no_t chip, const u32 address, const u32 value)
{
    if (!current || chip != 0)
        return VTSS_RC_ERROR;
    return mx84_mdio_write32(current, address, value) ? VTSS_RC_ERROR : VTSS_RC_OK;
}
