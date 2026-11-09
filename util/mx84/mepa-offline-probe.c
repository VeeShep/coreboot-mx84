/* Characterize the pinned PHY identity helper with fake MIIM; no device creation. */
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <microchip/ethernet/phy/api.h>
#include "phy-identity.h"

#ifdef NDEBUG
#error "The offline PHY probe requires assertions enabled"
#endif

struct mepa_callout_ctx {
    uint32_t id;
    unsigned calls, fail;
    mepa_rc error;
};

static mepa_rc read_id(struct mepa_callout_ctx *context, uint8_t reg, uint16_t *value)
{
    assert(context && value && reg == 2 + context->calls);
    if (++context->calls == context->fail)
        return context->error ? context->error : MEPA_RC_ERROR;
    *value = reg == 2 ? context->id >> 16 : context->id & 0xffff;
    return MEPA_RC_OK;
}

int main(void)
{
    const mepa_callout_t callout = {.miim_read = read_id};
    const uint32_t ids[] = {0, 0xffffffff, 0x12345678};
    for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i) {
        struct mepa_callout_ctx context = {.id = ids[i]};
        assert(mepa_phy_id_get(&callout, &context, 0) == ids[i]);
        assert(context.calls == 2);
    }
    for (unsigned fail = 1; fail <= 2; ++fail) {
        struct mepa_callout_ctx context = {.id = 0x12345678, .fail = fail};
        /* Pin-specific hazard: upstream ignores errors and returns a partial ID. */
        assert(mepa_phy_id_get(&callout, &context, 0) ==
               (fail == 1 ? 0x00005678U : 0x12340000U));
        assert(context.calls == 2);
    }
    const mepa_rc errors[] = {MEPA_RC_ERROR, -1234, 1};
    for (unsigned fail = 1; fail <= 2; ++fail) {
        for (unsigned i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
            struct mepa_callout_ctx context = {.id = 0x12345678, .fail = fail, .error = errors[i]};
            uint32_t identity = 42;
            assert(mx84_phy_identity_read(read_id, &context, &identity) ==
                   (errors[i] < 0 ? errors[i] : MEPA_RC_ERROR));
            assert(context.calls == fail && identity == 42);
        }
    }
    const uint32_t checked_ids[] = {0, 0xffffffff, 0x12345678, 0x00005678, 0x12340000};
    for (unsigned i = 0; i < sizeof(checked_ids) / sizeof(checked_ids[0]); ++i) {
        struct mepa_callout_ctx context = {.id = checked_ids[i]};
        uint32_t identity = 42;
        bool invalid = i < 2;
        assert(mx84_phy_identity_read(read_id, &context, &identity) ==
               (invalid ? MEPA_RC_ERROR : MEPA_RC_OK));
        assert(context.calls == 2 && identity == (invalid ? 42 : checked_ids[i]));
    }
    struct mepa_callout_ctx context = {.id = 0x12345678};
    uint32_t identity = 42;
    assert(mx84_phy_identity_read(NULL, &context, &identity) == MEPA_RC_ERROR);
    assert(mx84_phy_identity_read(read_id, NULL, &identity) == MEPA_RC_ERROR);
    assert(mx84_phy_identity_read(read_id, &context, NULL) == MEPA_RC_ERROR);
    assert(context.calls == 0 && identity == 42);
    puts("PASS: linked PHY identity helper, fake Clause22 reads and reproduced partial-ID hazard; no device access");
    puts("PASS: checked PHY ID reader stops on errors, preserves output and rejects zero/all-one IDs; fake I/O only");
    return 0;
}
