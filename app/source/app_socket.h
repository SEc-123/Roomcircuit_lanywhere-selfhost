#pragma once

#include <stdbool.h>

// Hold a reference until all of the caller's socket workers have stopped.
bool app_socket_acquire(void);
void app_socket_release(void);
