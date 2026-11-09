#include "phy-identity.h"

mepa_rc mx84_phy_identity_read(mepa_miim_read_t read,
    struct mepa_callout_ctx *context, uint32_t *identity)
{
    if (!read || !context || !identity)
        return MEPA_RC_ERROR;
    uint16_t high = 0, low = 0;
    mepa_rc rc = read(context, 2, &high);
    if (rc != MEPA_RC_OK)
        return rc < 0 ? rc : MEPA_RC_ERROR;
    rc = read(context, 3, &low);
    if (rc != MEPA_RC_OK)
        return rc < 0 ? rc : MEPA_RC_ERROR;
    uint32_t result = ((uint32_t)high << 16) | low;
    if (!result || result == UINT32_MAX)
        return MEPA_RC_ERROR;
    *identity = result;
    return MEPA_RC_OK;
}
