#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <sodium.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <switch.h>
#include "../../common/update_format.h"

static pthread_mutex_t simulated_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_uint registrations, initializations, entropy_calls;
static atomic_bool start_parsing;
static uint8_t test_key[crypto_sign_PUBLICKEYBYTES];

void mutexLock(Mutex *mutex) {
    (void)mutex;
    assert(pthread_mutex_lock(&simulated_lock) == 0);
}

void mutexUnlock(Mutex *mutex) {
    (void)mutex;
    assert(pthread_mutex_unlock(&simulated_lock) == 0);
}

void randomGet(void *buffer, size_t size) {
    atomic_fetch_add(&entropy_calls, 1);
    memset(buffer, 0x63, size);
}

int fsdevCommitDevice(const char *device) {
    assert(strcmp(device, "sdmc") == 0);
    return 0;
}

static int checked_set_implementation(randombytes_implementation *implementation) {
    assert(atomic_fetch_add(&registrations, 1) == 0);
    return randombytes_set_implementation(implementation);
}

static int checked_sodium_init(void) {
    assert(atomic_load(&registrations) == 1);
    assert(atomic_fetch_add(&initializations, 1) == 0);
    assert(strcmp(randombytes_implementation_name(), "switch-libnx") == 0);
    return sodium_init();
}

#define UPDATE_VERIFY_PUBLIC_KEY test_key
#define randombytes_set_implementation checked_set_implementation
#define sodium_init checked_sodium_init
#include "../source/update_core.c"
#undef sodium_init
#undef randombytes_set_implementation

typedef struct {
    uint8_t bytes[UPDATE_MANIFEST_SIZE];
    uint8_t public_key[crypto_sign_PUBLICKEYBYTES];
} SignedManifest;

static SignedManifest make_fixture(void) {
    int ends[2];
    assert(pipe(ends) == 0);
    const pid_t signer = fork();
    assert(signer >= 0);
    if (signer == 0) {
        // Keep updater-side Sodium state pristine by signing in a child process.
        close(ends[0]);
        assert(sodium_init() >= 0);
        SignedManifest fixture = {0};
        uint8_t seed[crypto_sign_SEEDBYTES] = {7};
        uint8_t secret[crypto_sign_SECRETKEYBYTES];
        assert(crypto_sign_seed_keypair(fixture.public_key, secret, seed) == 0);
        uint8_t *raw = fixture.bytes;
        memcpy(raw, "LNU1", 4);
        write32(raw + 4, UPDATE_FORMAT_VERSION);
        write32(raw + 12, 8);
        write32(raw + 16, 1);
        write32(raw + 20, UPDATE_FORMAT_VERSION);
        write32(raw + 24, 256);
        write32(raw + 28, 4096);
        write32(raw + 32, UPDATE_FILE_COUNT);
        assert(crypto_sign_detached(raw + UPDATE_MANIFEST_BODY_SIZE, NULL,
                                    raw, UPDATE_MANIFEST_BODY_SIZE, secret) == 0);
        assert(write(ends[1], &fixture, sizeof(fixture)) == sizeof(fixture));
        close(ends[1]);
        _exit(0);
    }
    close(ends[1]);
    FILE *pipe_read = fdopen(ends[0], "rb");
    SignedManifest fixture;
    assert(pipe_read && fread(&fixture, sizeof(fixture), 1, pipe_read) == 1);
    assert(fclose(pipe_read) == 0);
    int status;
    assert(waitpid(signer, &status, 0) == signer && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    return fixture;
}

static void *parse_at_startup(void *context) {
    const SignedManifest *fixture = context;
    while (!atomic_load(&start_parsing)) sched_yield();
    UpdateManifest manifest;
    assert(update_manifest_parse(fixture->bytes, "0.8.1", &manifest) == UPDATE_NO_UPDATE);
    assert(strcmp(manifest.version, "0.8.1") == 0);
    return NULL;
}

int main(void) {
    SignedManifest fixture = make_fixture();
    memcpy(test_key, fixture.public_key, sizeof(test_key));

    pthread_t checks[4];
    for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); i++)
        assert(pthread_create(&checks[i], NULL, parse_at_startup, &fixture) == 0);
    atomic_store(&start_parsing, true);
    for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); i++)
        assert(pthread_join(checks[i], NULL) == 0);

    UpdateManifest manifest;
    assert(update_manifest_parse(fixture.bytes, "0.7.0", &manifest) == UPDATE_OK);
    fixture.bytes[100] ^= 1;
    assert(update_manifest_parse(fixture.bytes, "0.7.0", &manifest) == UPDATE_SIGNATURE);
    assert(atomic_load(&registrations) == 1 && atomic_load(&initializations) == 1);

    unsigned before = atomic_load(&entropy_calls);
    uint8_t bytes[16];
    randombytes_buf(bytes, sizeof(bytes));
    for (size_t i = 0; i < sizeof(bytes); i++) assert(bytes[i] == 0x63);
    assert(randombytes_random() == UINT32_C(0x63636363));
    assert(atomic_load(&entropy_calls) == before + 2);
    puts("Switch updater crypto: one native entropy registration, concurrent signed startup checks, tamper rejection");
}
