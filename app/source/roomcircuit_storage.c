#include "roomcircuit_storage.h"
static bool current_storage;
static bool configured;

void roomcircuit_storage_init(void)
{
    current_storage = false;
    configured = false;
}

void roomcircuit_storage_set_authoritative(bool current)
{
    if (!configured) { current_storage = current; configured = true; }
}

bool roomcircuit_storage_current(void)
{
    return current_storage;
}

bool roomcircuit_storage_available(void)
{
    return configured;
}
