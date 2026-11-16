#pragma once
#include <stdbool.h>

/* Default to legacy until the running module provides its authoritative
 * storage selection. Set once before opening any RoomCircuit user data. */
void roomcircuit_storage_init(void);
void roomcircuit_storage_set_authoritative(bool current);
bool roomcircuit_storage_current(void);
bool roomcircuit_storage_available(void);
