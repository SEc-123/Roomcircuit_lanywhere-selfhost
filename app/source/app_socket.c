#include "app_socket.h"
#include <switch.h>

static Mutex socket_lock;
static unsigned references;

bool app_socket_acquire(void)
{
    mutexLock(&socket_lock);
    if (!references && R_FAILED(socketInitializeDefault())) {
        mutexUnlock(&socket_lock);
        return false;
    }
    references++;
    mutexUnlock(&socket_lock);
    return true;
}

void app_socket_release(void)
{
    mutexLock(&socket_lock);
    if (references && --references == 0)
        socketExit();
    mutexUnlock(&socket_lock);
}
