#ifndef MX84_MDIO_WINDOW_H
#define MX84_MDIO_WINDOW_H

#include <pthread.h>
#include <stdint.h>

/* One shared instance per bus, exclusively owned by the future daemon.
 * Initialize before use; do not copy it; join callers before destruction.
 * No device backend is supplied; this cannot arbitrate with vtss_poca_d. */
struct mx84_mdio_window {
    pthread_mutex_t mutex;
    void *context;
    int (*read16)(void *context, uint16_t reg, uint16_t *value);
    int (*write16)(void *context, uint16_t reg, uint16_t value);
};

/* Callbacks return zero on success, nonzero on error; they must bound their waits. */
int mx84_mdio_window_init(struct mx84_mdio_window *window, void *context,
    int (*read16)(void *, uint16_t, uint16_t *),
    int (*write16)(void *, uint16_t, uint16_t));
int mx84_mdio_window_destroy(struct mx84_mdio_window *window);
int mx84_mdio_read32(struct mx84_mdio_window *window, uint32_t address, uint32_t *value);
int mx84_mdio_write32(struct mx84_mdio_window *window, uint32_t address, uint32_t value);

#endif
