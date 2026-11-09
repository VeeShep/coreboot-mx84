/* Software-only MESA link/lifecycle check. No device access or initializer. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <vtss_api.h>
#include <vtss_luton26_cil.h>
#include "mesa-transport.h"
#include "mx84-board-profile.h"
#include "mesa-phy-identity.h"
#include <errno.h>

#ifdef NDEBUG
#error "The offline probe requires assertions enabled"
#endif

void mx84_check_phy_stack(vtss_inst_t inst);

/* Single-thread probe: these callouts must become real locks in a daemon. */
void vtss_callout_lock(const vtss_api_lock_t *const lock) {}
void vtss_callout_unlock(const vtss_api_lock_t *const lock) {}

void vtss_callout_trace_printf(const vtss_trace_layer_t layer,
    const vtss_trace_group_t group, const vtss_trace_level_t level,
    const char *file, const int line, const char *function, const char *format, ...)
{
    if (level == VTSS_TRACE_LEVEL_ERROR) {
        va_list args;
        va_start(args, format);
        vfprintf(stderr, format, args);
        va_end(args);
        fputc('\n', stderr);
    }
}

void vtss_callout_trace_hex_dump(const vtss_trace_layer_t layer,
    const vtss_trace_group_t group, const vtss_trace_level_t level,
    const char *file, const int line, const char *function,
    const u8 *bytes, const int count) {}

static unsigned transfers, fail_at, polls;
static BOOL stuck_busy;

static vtss_rc fake_read(const vtss_chip_no_t chip, const u32 addr, u32 *const value)
{
    assert(chip == 0 && value != NULL);
    if (++transfers == fail_at)
        return VTSS_RC_ERROR;
    if (addr == VTSS_DEVCPU_GCB_VCORE_ACCESS_VA_CTRL) {
        ++polls;
        *value = stuck_busy || polls < 3 ? VTSS_F_DEVCPU_GCB_VCORE_ACCESS_VA_CTRL_VA_BUSY : 0;
    } else {
        assert(addr == VTSS_DEVCPU_GCB_VCORE_ACCESS_VA_DATA);
        *value = 0x89abcdef;
    }
    return VTSS_RC_OK;
}

static vtss_rc fake_write(const vtss_chip_no_t chip, const u32 addr, const u32 value)
{
    assert(chip == 0);
    assert(addr == VTSS_DEVCPU_GCB_VCORE_ACCESS_VA_ADDR ||
           addr == VTSS_DEVCPU_GCB_VCORE_ACCESS_VA_DATA);
    if (addr == VTSS_DEVCPU_GCB_VCORE_ACCESS_VA_ADDR)
        assert(value == VTSS_IO_ORIGIN2_OFFSET);
    else
        assert(value == 0x12345678);
    return ++transfers == fail_at ? VTSS_RC_ERROR : VTSS_RC_OK;
}

static void check_indirect_errors(vtss_inst_t inst)
{
    vtss_state_t *state = NULL;
    assert(vtss_inst_check(inst, &state) == VTSS_RC_OK);
    vtss_init_conf_t saved = state->init_conf;
    /* White-box test only: bypass init_conf_set, which initializes hardware. */
    state->init_conf.reg_read = fake_read;
    state->init_conf.reg_write = fake_write;
    state->init_conf.spi_bus = TRUE;
    const u32 address = (VTSS_IO_ORIGIN2_OFFSET - VTSS_IO_ORIGIN1_OFFSET) >> 2;
    for (unsigned write = 0; write < 2; ++write) {
        const unsigned count = write ? 5 : 6;
        for (unsigned failure = 0; failure <= count; ++failure) {
            transfers = polls = 0;
            fail_at = failure;
            stuck_busy = FALSE;
            u32 value = 0x12345678;
            vtss_rc rc = write ? vtss_l26_wr(state, address, value)
                               : vtss_l26_rd(state, address, &value);
            assert(rc == (failure ? VTSS_RC_ERROR : VTSS_RC_OK));
            assert(transfers == (failure ? failure : count));
            if (!failure && !write)
                assert(value == 0x89abcdef);
        }
        /* A transport deadline must return an error even while BUSY persists. */
        transfers = polls = 0;
        fail_at = 9;
        stuck_busy = TRUE;
        u32 value = 0x12345678;
        assert((write ? vtss_l26_wr(state, address, value)
                      : vtss_l26_rd(state, address, &value)) == VTSS_RC_ERROR);
        assert(transfers == fail_at && polls == 6);
    }
    state->init_conf = saved;
    puts("PASS: SDK indirect read/write errors at every stage and stuck-BUSY callback escape; fake I/O only");
}

struct fake_window {
    unsigned step, fail;
    BOOL write;
};

static int window_write(void *context, uint16_t reg, uint16_t value)
{
    struct fake_window *bus = context;
    const uint16_t expected[] = {0x5678, 0x1234, 0xcdef, 0x89ab};
    assert(bus->step < (bus->write ? 4U : 2U));
    assert(reg == bus->step && value == expected[bus->step]);
    return ++bus->step == bus->fail ? EIO : 0;
}

static int window_read(void *context, uint16_t reg, uint16_t *value)
{
    struct fake_window *bus = context;
    assert(!bus->write && bus->step >= 2 && bus->step < 6);
    assert(reg == 2 + bus->step % 2);
    if (++bus->step == bus->fail)
        return EIO;
    *value = bus->step <= 4 ? 0x5555 : (reg == 2 ? 0xcdef : 0x89ab);
    return 0;
}

static void *check_other_thread(void *context)
{
    u32 value = 42;
    assert(mx84_mesa_transport_begin(context) == EBUSY);
    assert(mx84_mesa_reg_read(0, 0, &value) == VTSS_RC_ERROR && value == 42);
    assert(mx84_mesa_reg_write(0, 0, 0) == VTSS_RC_ERROR);
    assert(mx84_mesa_transport_end() == EINVAL);
    return NULL;
}

static void check_transport(vtss_inst_t inst)
{
    struct fake_window bus = {0};
    struct mx84_mdio_window window;
    assert(mx84_mdio_window_init(&window, &bus, window_read, window_write) == 0);
    vtss_state_t *state = NULL;
    assert(vtss_inst_check(inst, &state) == VTSS_RC_OK);
    vtss_init_conf_t saved = state->init_conf;
    state->init_conf.reg_read = mx84_mesa_reg_read;
    state->init_conf.reg_write = mx84_mesa_reg_write;
    state->init_conf.spi_bus = FALSE;
    u32 value = 42;
    assert(mx84_mesa_transport_begin(NULL) == EINVAL);
    assert(mx84_mesa_reg_read(0, 0, &value) == VTSS_RC_ERROR && value == 42);
    assert(mx84_mesa_reg_write(0, 0, 0) == VTSS_RC_ERROR);
    assert(mx84_mesa_transport_end() == EINVAL);
    assert(mx84_mesa_transport_begin(&window) == 0);
    assert(mx84_mesa_transport_begin(&window) == EBUSY);
    assert(mx84_mesa_reg_read(1, 0, &value) == VTSS_RC_ERROR && value == 42);
    assert(mx84_mesa_reg_read(0, 0, NULL) == VTSS_RC_ERROR);
    assert(mx84_mesa_reg_write(1, 0, 0) == VTSS_RC_ERROR);
    pthread_t other;
    assert(pthread_create(&other, NULL, check_other_thread, &window) == 0);
    assert(pthread_join(other, NULL) == 0 && bus.step == 0);
    assert(mx84_mesa_transport_end() == 0);
    for (unsigned write = 0; write < 2; ++write) {
        unsigned count = write ? 4 : 6;
        for (unsigned failure = 0; failure <= count; ++failure) {
            bus = (struct fake_window){.fail = failure, .write = write};
            value = 42;
            assert(mx84_mesa_transport_begin(&window) == 0);
            vtss_rc rc = write ? vtss_l26_wr(state, 0x12345678, 0x89abcdef)
                               : vtss_l26_rd(state, 0x12345678, &value);
            assert(rc == (failure ? VTSS_RC_ERROR : VTSS_RC_OK));
            assert(bus.step == (failure ? failure : count));
            if (!write)
                assert(value == (failure ? 42U : 0x89abcdefU));
            assert(mx84_mesa_transport_end() == 0);
        }
    }
    state->init_conf = saved;
    assert(mx84_mdio_window_destroy(&window) == 0);
    puts("PASS: SDK/MDIO integration, unchanged word addresses, all transfer faults, operation ownership and reuse; fake I/O only");
}

static u8 miim_phy, miim_register;
static BOOL miim_bad_response;
static BOOL miim_identity_mode;
static unsigned miim_commands;
static struct mx84_mdio_window *miim_window;
static uint32_t miim_identity_value;

static vtss_rc fake_miim_write(const vtss_chip_no_t chip, const u32 addr, const u32 value)
{
    assert(chip == 0 && addr == VTSS_DEVCPU_GCB_MIIM_MII_CMD(0));
    if (miim_identity_mode) {
        assert(mx84_mesa_transport_begin(miim_window) == EBUSY);
        miim_register = 2 + miim_commands++;
        assert(miim_register <= 3);
        polls = 0;
    }
    assert(value == (VTSS_F_DEVCPU_GCB_MIIM_MII_CMD_MIIM_CMD_OPR_FIELD(2) |
                     VTSS_F_DEVCPU_GCB_MIIM_MII_CMD_MIIM_CMD_VLD |
                     VTSS_F_DEVCPU_GCB_MIIM_MII_CMD_MIIM_CMD_REGAD(miim_register) |
                     VTSS_F_DEVCPU_GCB_MIIM_MII_CMD_MIIM_CMD_PHYAD(miim_phy)));
    return ++transfers == fail_at ? VTSS_RC_ERROR : VTSS_RC_OK;
}

static vtss_rc fake_miim_read(const vtss_chip_no_t chip, const u32 addr, u32 *const value)
{
    assert(chip == 0 && value);
    if (++transfers == fail_at)
        return VTSS_RC_ERROR;
    if (addr == VTSS_DEVCPU_GCB_MIIM_MII_STATUS(0)) {
        ++polls;
        *value = stuck_busy || polls < 3 ? VTSS_F_DEVCPU_GCB_MIIM_MII_STATUS_MIIM_STAT_BUSY : 0;
    } else {
        assert(addr == VTSS_DEVCPU_GCB_MIIM_MII_DATA(0));
        u16 data = miim_identity_mode ? (miim_register == 2 ? miim_identity_value >> 16
                                                           : miim_identity_value & 0xffff)
                                     : 0x1234;
        *value = VTSS_F_DEVCPU_GCB_MIIM_MII_DATA_MIIM_DATA_RDDATA(data) |
                 (miim_bad_response ? VTSS_F_DEVCPU_GCB_MIIM_MII_DATA_MIIM_DATA_SUCCESS(3) : 0);
    }
    return VTSS_RC_OK;
}

static void check_miim(vtss_inst_t inst)
{
    vtss_state_t *state = NULL;
    assert(vtss_inst_check(inst, &state) == VTSS_RC_OK);
    vtss_init_conf_t saved = state->init_conf;
    state->init_conf.reg_read = fake_miim_read;
    state->init_conf.reg_write = fake_miim_write;
    state->init_conf.spi_bus = FALSE;
    for (miim_phy = 0; miim_phy <= 10; ++miim_phy) {
        for (miim_register = 2; miim_register <= 3; ++miim_register) {
            for (unsigned failure = 0; failure <= 5; ++failure) {
                transfers = polls = 0;
                fail_at = failure;
                stuck_busy = miim_bad_response = FALSE;
                u16 value = 42;
                assert(vtss_miim_read(inst, 0, VTSS_MIIM_CONTROLLER_0,
                                      miim_phy, miim_register, &value) ==
                       (failure ? VTSS_RC_ERROR : VTSS_RC_OK));
                assert(transfers == (failure ? failure : 5));
                assert(value == (failure ? 42 : 0x1234));
            }
        }
    }
    transfers = polls = 0;
    fail_at = 9;
    stuck_busy = TRUE;
    miim_phy = 10;
    miim_register = 3;
    u16 value = 42;
    assert(vtss_miim_read(inst, 0, VTSS_MIIM_CONTROLLER_0, miim_phy, miim_register, &value) == VTSS_RC_ERROR);
    assert(transfers == 9 && polls == 7 && value == 42);
    transfers = polls = fail_at = 0;
    stuck_busy = FALSE;
    miim_bad_response = TRUE;
    assert(vtss_miim_read(inst, 0, VTSS_MIIM_CONTROLLER_0, miim_phy, miim_register, &value) == VTSS_RC_ERROR);
    assert(transfers == 5 && value == 42);
    state->init_conf = saved;
    puts("PASS: linked SDK MIIM controller/PHY/ID-register encoding, all transfer faults, BUSY/error escape; fake I/O only");
}

static void check_identity_bridge(vtss_inst_t inst)
{
    struct fake_window bus = {0};
    struct mx84_mdio_window window;
    assert(mx84_mdio_window_init(&window, &bus, window_read, window_write) == 0);
    vtss_state_t *state = NULL;
    assert(vtss_inst_check(inst, &state) == VTSS_RC_OK);
    vtss_init_conf_t saved = state->init_conf;
    uint32_t unconfigured = 42;
    assert(mx84_mesa_phy_identity(inst, &window, 0, &unconfigured) == VTSS_RC_ERROR);
    assert(unconfigured == 42 && mx84_mesa_transport_end() == EINVAL);
    state->init_conf.reg_read = fake_miim_read;
    state->init_conf.reg_write = fake_miim_write;
    state->init_conf.spi_bus = FALSE;
    miim_identity_mode = TRUE;
    miim_window = &window;
    stuck_busy = miim_bad_response = FALSE;
    miim_identity_value = 0x12345678;
    for (miim_phy = 0; miim_phy <= 10; ++miim_phy) {
        for (unsigned failure = 0; failure <= 10; ++failure) {
            transfers = polls = miim_commands = 0;
            fail_at = failure;
            uint32_t identity = 42;
            assert(mx84_mesa_phy_identity(inst, &window, miim_phy, &identity) ==
                   (failure ? VTSS_RC_ERROR : VTSS_RC_OK));
            assert(transfers == (failure ? failure : 10));
            assert(identity == (failure ? 42U : miim_identity_value));
            assert(mx84_mesa_transport_end() == EINVAL);
        }
    }
    const uint32_t invalid_ids[] = {0, 0xffffffff};
    miim_phy = 0;
    fail_at = 0;
    for (unsigned i = 0; i < 2; ++i) {
        transfers = polls = miim_commands = 0;
        miim_identity_value = invalid_ids[i];
        uint32_t identity = 42;
        assert(mx84_mesa_phy_identity(inst, &window, 0, &identity) == VTSS_RC_ERROR);
        assert(transfers == 10 && identity == 42);
    }
    transfers = 0;
    uint32_t identity = 42;
    for (unsigned port = 11; port <= 18; ++port)
        assert(mx84_mesa_phy_identity(inst, &window, port, &identity) == VTSS_RC_ERROR);
    assert(mx84_mesa_phy_identity(inst, &window, UINT32_MAX, &identity) == VTSS_RC_ERROR);
    assert(mx84_mesa_phy_identity(NULL, &window, 0, &identity) == VTSS_RC_ERROR);
    assert(mx84_mesa_phy_identity(inst, NULL, 0, &identity) == VTSS_RC_ERROR);
    assert(mx84_mesa_phy_identity(inst, &window, 0, NULL) == VTSS_RC_ERROR);
    assert(mx84_mesa_transport_begin(&window) == 0);
    assert(mx84_mesa_phy_identity(inst, &window, 0, &identity) == VTSS_RC_ERROR);
    assert(mx84_mesa_transport_end() == 0);
    assert(transfers == 0 && identity == 42 && bus.step == 0);
    state->init_conf = saved;
    miim_identity_mode = FALSE;
    miim_window = NULL;
    assert(mx84_mdio_window_destroy(&window) == 0);
    puts("PASS: checked ID -> SDK MIIM bridge, copper-only mapping, all faults, ownership and cleanup; fake register I/O only");
}

int main(void)
{
    vtss_inst_create_t create;
    vtss_inst_t inst = NULL;
    vtss_init_conf_t conf;
    /* API logical -> silicon port, proved by stock port_map; SFP labels unverified. */
    const unsigned chip_ports[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 21, 20, 24, 25, 23, 22};
    unsigned seen = 0;
    assert(sizeof(mx84_board_ports) / sizeof(mx84_board_ports[0]) == 17);
    assert(mx84_mux_mode == VTSS_PORT_MUX_MODE_2);
    assert(VTSS_PORT_ARRAY_SIZE >= sizeof(chip_ports) / sizeof(chip_ports[0]));
    for (unsigned i = 0; i < sizeof(chip_ports) / sizeof(chip_ports[0]); ++i) {
        assert(chip_ports[i] < 26 && !(seen & (1U << chip_ports[i])));
        seen |= 1U << chip_ports[i];
        assert(mx84_board_ports[i].chip_port == (int)chip_ports[i]);
        assert(mx84_board_ports[i].interface ==
               (i <= 10 ? VTSS_PORT_INTERFACE_SGMII : VTSS_PORT_INTERFACE_SERDES));
        assert(mx84_board_ports[i].miim_controller ==
               (i <= 10 ? VTSS_MIIM_CONTROLLER_0 : VTSS_MIIM_CONTROLLER_NONE));
        assert(mx84_board_ports[i].miim_addr == (i <= 10 ? i : 0xff));
    }
    /* Guard against accidentally treating stock numeric enums as current ABI. */
    assert(VTSS_PORT_INTERFACE_SGMII != 8 && VTSS_PORT_INTERFACE_SERDES != 10);
    puts("PASS: symbolic MX84 board profile, mux mode 2, MIIM map and legacy-enum separation; data only");
    assert(vtss_inst_get(VTSS_TARGET_SPARX_III_18, &create) == VTSS_RC_OK);
    assert(create.target == 0x7425);
    assert(vtss_inst_create(&create, &inst) == VTSS_RC_OK && inst != NULL);
    assert(vtss_init_conf_get(inst, &conf) == VTSS_RC_OK);
    assert(conf.reg_read == NULL && conf.reg_write == NULL);
    check_indirect_errors(inst);
    check_transport(inst);
    check_miim(inst);
    check_identity_bridge(inst);
    mx84_check_phy_stack(inst);
    /* Never call init_conf_set/port_map_set: these cross into hardware initialization. */
    assert(vtss_inst_destroy(inst) == VTSS_RC_OK);
    printf("PASS: VSC7425 software lifecycle, capacity %u, 17 unique MX84 chip ports; no hardware backend\n",
           (unsigned)VTSS_PORT_ARRAY_SIZE);
    return 0;
}
