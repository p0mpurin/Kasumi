#include <openssl/crypto.h>
#include <psp2/kernel/threadmgr.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

/* OpenSSL's thread layer (crypto/threads_pthread.c) in place of the one in
 * libcrypto.a. There every lock is a pthread rwlock, and the Vita's pthreads
 * build each one from kernel semaphores: every certificate, key and
 * connection holds a lock, so the 121 trusted roots alone ran Kasumi out
 * of kernel objects (SCE_KERNEL_ERROR_UID_RL_OVERFLOW). After that not even
 * a DNS resolver could be created and nothing could sign in. These locks
 * are a counter in memory instead: readers add one, a writer takes it from
 * 0 to -1, and a thread that finds it busy waits a moment. OpenSSL holds
 * them for microseconds. Thread-local values and run-once stay on pthreads
 * (no kernel objects per call). */

typedef struct {
    volatile int state; /* > 0 readers, -1 a writer, 0 free */
} SpinRwLock;

static void wait_turn(unsigned *spins)
{
    if (++*spins < 64) return;
    /* Someone holds it for longer: let them run. */
    sceKernelDelayThread(50);
}

CRYPTO_RWLOCK *CRYPTO_THREAD_lock_new(void) { return calloc(1, sizeof(SpinRwLock)); }

int CRYPTO_THREAD_read_lock(CRYPTO_RWLOCK *lock)
{
    SpinRwLock *l = lock;
    unsigned spins = 0;
    for (;;) {
        int state = __atomic_load_n(&l->state, __ATOMIC_RELAXED);
        if (state >= 0 && __atomic_compare_exchange_n(&l->state, &state, state + 1, false, __ATOMIC_ACQUIRE,
                                                      __ATOMIC_RELAXED))
            return 1;
        wait_turn(&spins);
    }
}

int CRYPTO_THREAD_write_lock(CRYPTO_RWLOCK *lock)
{
    SpinRwLock *l = lock;
    unsigned spins = 0;
    for (;;) {
        int expected = 0;
        if (__atomic_compare_exchange_n(&l->state, &expected, -1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 1;
        wait_turn(&spins);
    }
}

int CRYPTO_THREAD_unlock(CRYPTO_RWLOCK *lock)
{
    SpinRwLock *l = lock;
    if (__atomic_load_n(&l->state, __ATOMIC_RELAXED) < 0) __atomic_store_n(&l->state, 0, __ATOMIC_RELEASE);
    else __atomic_sub_fetch(&l->state, 1, __ATOMIC_RELEASE);
    return 1;
}

void CRYPTO_THREAD_lock_free(CRYPTO_RWLOCK *lock) { free(lock); }

int CRYPTO_THREAD_run_once(CRYPTO_ONCE *once, void (*init)(void)) { return pthread_once(once, init) == 0; }

int CRYPTO_THREAD_init_local(CRYPTO_THREAD_LOCAL *key, void (*cleanup)(void *))
{
    return pthread_key_create(key, cleanup) == 0;
}

void *CRYPTO_THREAD_get_local(CRYPTO_THREAD_LOCAL *key) { return pthread_getspecific(*key); }

int CRYPTO_THREAD_set_local(CRYPTO_THREAD_LOCAL *key, void *value) { return pthread_setspecific(*key, value) == 0; }

int CRYPTO_THREAD_cleanup_local(CRYPTO_THREAD_LOCAL *key) { return pthread_key_delete(*key) == 0; }

CRYPTO_THREAD_ID CRYPTO_THREAD_get_current_id(void) { return pthread_self(); }

int CRYPTO_THREAD_compare_id(CRYPTO_THREAD_ID a, CRYPTO_THREAD_ID b) { return pthread_equal(a, b); }

int CRYPTO_atomic_add(int *val, int amount, int *ret, CRYPTO_RWLOCK *lock)
{
    (void)lock;
    *ret = __atomic_add_fetch(val, amount, __ATOMIC_ACQ_REL);
    return 1;
}

/* No fork() on the Vita. */
int openssl_init_fork_handlers(void) { return 0; }
int openssl_get_fork_id(void) { return getpid(); }
