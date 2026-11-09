#include "mdio-window.h"
#include <assert.h>
#include <errno.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>

#ifdef NDEBUG
#error "The protocol checks require assertions enabled"
#endif

/* Individual transfers are atomic here, as in the driver; transactions are not.
 * Yielding after each transfer exposes missing whole-transaction protection. */
struct fake_bus {
    pthread_mutex_t mutex;
    unsigned phase, calls, fail_at, writes;
    pthread_t owner;
    uint32_t address, written;
    uint16_t low;
    bool writing;
};

static int transfer(struct fake_bus *b, bool write, uint16_t reg, uint16_t *value)
{
    assert(pthread_mutex_lock(&b->mutex) == 0);
    ++b->calls;
    if (b->calls == b->fail_at) {
        b->phase = 0;
        assert(pthread_mutex_unlock(&b->mutex) == 0);
        return EIO;
    }
    if (b->phase == 0)
        b->owner = pthread_self();
    else
        assert(pthread_equal(b->owner, pthread_self()));
    switch (b->phase) {
    case 0:
        assert(write && reg == 0);
        b->address = *value;
        break;
    case 1:
        assert(write && reg == 1);
        b->address |= (uint32_t)*value << 16;
        break;
    case 2:
        assert(reg == 2);
        b->writing = write;
        if (write) b->low = *value;
        else *value = 0xdead;
        break;
    case 3:
        assert(reg == 3 && write == b->writing);
        if (write) {
            b->written = (uint32_t)*value << 16 | b->low;
            ++b->writes;
        } else *value = 0xbeef;
        break;
    case 4:
        assert(!write && reg == 2);
        *value = (uint16_t)(b->address ^ UINT32_C(0xa5a5a5a5));
        break;
    case 5:
        assert(!write && reg == 3);
        *value = (uint16_t)((b->address ^ UINT32_C(0xa5a5a5a5)) >> 16);
        break;
    default: assert(false);
    }
    b->phase = b->phase == 5 || (b->phase == 3 && write) ? 0 : b->phase + 1;
    assert(pthread_mutex_unlock(&b->mutex) == 0);
    sched_yield();
    return 0;
}

static int read16(void *context, uint16_t reg, uint16_t *value)
{
    return transfer(context, false, reg, value);
}

static int write16(void *context, uint16_t reg, uint16_t value)
{
    return transfer(context, true, reg, &value);
}

static atomic_bool go;
static atomic_uint ready;
struct worker { struct mx84_mdio_window *window; unsigned id; };

static void *concurrent_worker(void *arg)
{
    struct worker *worker = arg;
    atomic_fetch_add(&ready, 1);
    while (!atomic_load(&go)) sched_yield();
    for (unsigned i = 0; i < 1000; ++i) {
        uint32_t address = (worker->id << 24) | i, value;
        if (i & 1)
            assert(mx84_mdio_write32(worker->window, address, ~address) == 0);
        else {
            assert(mx84_mdio_read32(worker->window, address, &value) == 0);
            assert(value == (address ^ UINT32_C(0xa5a5a5a5)));
        }
    }
    return NULL;
}

int main(void)
{
    struct fake_bus bus = {.mutex = PTHREAD_MUTEX_INITIALIZER};
    struct mx84_mdio_window window;
    uint32_t value;
    assert(mx84_mdio_window_init(NULL, &bus, read16, write16) == EINVAL);
    assert(mx84_mdio_window_init(&window, &bus, NULL, write16) == EINVAL);
    assert(mx84_mdio_window_init(&window, &bus, read16, NULL) == EINVAL);
    assert(mx84_mdio_window_init(&window, &bus, read16, write16) == 0);
    assert(mx84_mdio_read32(NULL, 0, &value) == EINVAL);
    assert(mx84_mdio_read32(&window, 0, NULL) == EINVAL);
    assert(mx84_mdio_write32(NULL, 0, 0) == EINVAL);
    const uint32_t addresses[] = {0, UINT32_MAX, 0x12345678, 0xa5a5a5a5, 0x5a5a5a5a};
    for (unsigned i = 0; i < sizeof(addresses) / sizeof(addresses[0]); ++i) {
        assert(mx84_mdio_read32(&window, addresses[i], &value) == 0);
        assert(value == (addresses[i] ^ UINT32_C(0xa5a5a5a5)));
        assert(bus.address == addresses[i]);
        assert(mx84_mdio_write32(&window, addresses[i], ~addresses[i]) == 0);
        assert(bus.written == ~addresses[i]);
    }
    for (unsigned write = 0; write < 2; ++write) {
        for (unsigned fail = 1; fail <= (write ? 4U : 6U); ++fail) {
            bus.calls = bus.writes = 0;
            bus.fail_at = fail;
            value = 0x12345678;
            int rc = write ? mx84_mdio_write32(&window, 0xffffffff, 0xffffffff) :
                             mx84_mdio_read32(&window, 0xffffffff, &value);
            assert(rc == EIO && bus.calls == fail && bus.writes == 0);
            assert(value == 0x12345678);
            assert(pthread_mutex_trylock(&window.mutex) == 0);
            assert(pthread_mutex_unlock(&window.mutex) == 0);
            bus.fail_at = 0;
            assert(mx84_mdio_read32(&window, 0, &value) == 0);
            assert(value == UINT32_C(0xa5a5a5a5));
        }
    }
    bus.calls = bus.writes = 0;
    pthread_t threads[2];
    struct worker workers[2] = {{&window, 1}, {&window, 2}};
    for (unsigned i = 0; i < 2; ++i)
        assert(pthread_create(&threads[i], NULL, concurrent_worker, &workers[i]) == 0);
    while (atomic_load(&ready) != 2) sched_yield();
    atomic_store(&go, true);
    for (unsigned i = 0; i < 2; ++i) assert(pthread_join(threads[i], NULL) == 0);
    assert(bus.phase == 0 && bus.calls == 10000 && bus.writes == 1000);
    assert(mx84_mdio_window_destroy(&window) == 0);
    assert(pthread_mutex_destroy(&bus.mutex) == 0);
    puts("PASS: MDIO word ordering, dummy reads, all transfer errors, boundaries, and 2000 concurrent transactions");
    return 0;
}
