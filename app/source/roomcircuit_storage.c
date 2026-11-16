#include "roomcircuit_storage.h"
static unsigned storage_generation;
static bool configured;

void roomcircuit_storage_init(void)
{
    storage_generation = 0;
    configured = false;
}

void roomcircuit_storage_set_authoritative(bool current)
{
    roomcircuit_storage_set_generation(current ? 1 : 0);
}

void roomcircuit_storage_set_generation(unsigned generation)
{
    if (!configured && (generation == 0 || generation == 1 || generation == 3)) {
        storage_generation = generation;
        configured = true;
    }
}

unsigned roomcircuit_storage_generation(void) { return storage_generation; }

bool roomcircuit_storage_current(void)
{
    return storage_generation != 0;
}

bool roomcircuit_storage_available(void)
{
    return configured;
}
