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
    puts("PASS app uses the module's immutable storage snapshot; legacy is the default");
}
