#include <assert.h>
#include <stdio.h>
#include "../source/roomcircuit_storage.h"
#include "../source/roomcircuit_storage.c"

int main(void) {
    roomcircuit_storage_init(); assert(!roomcircuit_storage_current());
    roomcircuit_storage_set_authoritative(true); assert(roomcircuit_storage_current());
    roomcircuit_storage_set_authoritative(false); assert(roomcircuit_storage_current());
    roomcircuit_storage_init(); assert(!roomcircuit_storage_current());
    roomcircuit_storage_set_authoritative(false); assert(!roomcircuit_storage_current());
    roomcircuit_storage_set_authoritative(true); assert(!roomcircuit_storage_current());
    roomcircuit_storage_init(); roomcircuit_storage_set_generation(3);
    assert(roomcircuit_storage_available() && roomcircuit_storage_generation() == 3);
    roomcircuit_storage_set_generation(1); assert(roomcircuit_storage_generation() == 3);
    roomcircuit_storage_init(); roomcircuit_storage_set_generation(2);
    assert(!roomcircuit_storage_available());
    roomcircuit_storage_set_generation(1); assert(roomcircuit_storage_available() && roomcircuit_storage_generation() == 1);
    puts("PASS app uses the module's immutable storage snapshot; legacy is the default");
}
