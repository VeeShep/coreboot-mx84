#include "mdio-window.h"
#include <errno.h>

int mx84_mdio_window_init(struct mx84_mdio_window *w, void *context,
    int (*read16)(void *, uint16_t, uint16_t *),
    int (*write16)(void *, uint16_t, uint16_t))
{
    if (!w || !read16 || !write16)
        return EINVAL;
    int rc = pthread_mutex_init(&w->mutex, NULL);
    if (rc)
        return rc;
    w->context = context;
    w->read16 = read16;
    w->write16 = write16;
    return 0;
}

int mx84_mdio_window_destroy(struct mx84_mdio_window *w)
{
    return w ? pthread_mutex_destroy(&w->mutex) : EINVAL;
}

static int select_address(struct mx84_mdio_window *w, uint32_t address)
{
    int rc = w->write16(w->context, 0, (uint16_t)address);
    return rc ? rc : w->write16(w->context, 1, (uint16_t)(address >> 16));
}

int mx84_mdio_read32(struct mx84_mdio_window *w, uint32_t address, uint32_t *value)
{
    if (!w || !value)
        return EINVAL;
    int rc = pthread_mutex_lock(&w->mutex);
    if (rc)
        return rc;
    uint16_t low, high, discard;
    rc = select_address(w, address);
    /* Both dummy reads are part of the observed switch-window protocol. */
    if (!rc) rc = w->read16(w->context, 2, &discard);
    if (!rc) rc = w->read16(w->context, 3, &discard);
    if (!rc) rc = w->read16(w->context, 2, &low);
    if (!rc) rc = w->read16(w->context, 3, &high);
    if (!rc) *value = ((uint32_t)high << 16) | low;
    int unlocked = pthread_mutex_unlock(&w->mutex);
    return rc ? rc : unlocked;
}

int mx84_mdio_write32(struct mx84_mdio_window *w, uint32_t address, uint32_t value)
{
    if (!w)
        return EINVAL;
    int rc = pthread_mutex_lock(&w->mutex);
    if (rc)
        return rc;
    rc = select_address(w, address);
    if (!rc) rc = w->write16(w->context, 2, (uint16_t)value);
    if (!rc) rc = w->write16(w->context, 3, (uint16_t)(value >> 16));
    int unlocked = pthread_mutex_unlock(&w->mutex);
    return rc ? rc : unlocked;
}
