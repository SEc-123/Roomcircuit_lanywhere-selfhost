#define _POSIX_C_SOURCE 200809L
#include "node_selection_trace.h"
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef LANYWHERE_APP_VERSION
#define LANYWHERE_APP_VERSION "development"
#endif
#ifndef NODE_SELECTION_TRACE_DIRECTORY
#define NODE_SELECTION_TRACE_DIRECTORY "sdmc:/lanywhere"
#endif

#define TRACE_PATH NODE_SELECTION_TRACE_DIRECTORY "/node-selection-app.log"
#define TRACE_LIMIT 2048L
#define TRACE_BUILD "node-select-20261116"

// Only the UI thread records selections. Each record is closed before the next
// operation. Flush stdio and the SD file before IPC so the last completed stage
// has reached the filesystem even when the console subsequently needs a reboot.
static bool trace_ready;
static uint32_t trace_node;

static void write_record(const char *text, size_t size, bool first)
{
    FILE *file = fopen(TRACE_PATH, first ? "wb" : "r+b");
    if (!file) {
        trace_ready = false;
        return;
    }
    bool valid = fseek(file, 0, SEEK_END) == 0;
    const long offset = valid ? ftell(file) : -1;
    valid = offset >= 0 && size <= (size_t)TRACE_LIMIT && offset <= TRACE_LIMIT - (long)size;
    if (valid)
        valid = fwrite(text, 1, size, file) == size;
    if (valid)
        valid = fflush(file) == 0;
    if (valid) {
        const int descriptor = fileno(file);
        valid = descriptor >= 0 && fsync(descriptor) == 0;
    }
    if (fclose(file) != 0)
        valid = false;
    trace_ready = valid;
}

void node_selection_trace_begin(uint32_t node_id)
{
    const int saved_errno = errno;
    trace_ready = false;
    trace_node = node_id;
    if (mkdir(NODE_SELECTION_TRACE_DIRECTORY, 0700) == 0 || errno == EEXIST) {
        char line[160];
        const int length = snprintf(line, sizeof(line), "app=%s build=%s node=%u\n",
                                    LANYWHERE_APP_VERSION, TRACE_BUILD, (unsigned)node_id);
        if (length > 0 && (size_t)length < sizeof(line))
            write_record(line, (size_t)length, true);
    }
    errno = saved_errno;
}

void node_selection_trace_phase(NodeSelectionTracePhase phase, uint32_t result)
{
    static const char *const names[] = {
        "before_ipc", "after_ipc", "before_cancel_probes", "after_cancel_probes",
    };
    if (!trace_ready || (unsigned)phase >= sizeof(names) / sizeof(names[0]))
        return;
    const int saved_errno = errno;
    char line[112];
    int length;
    if (phase == NodeSelectionBeforeIpc || phase == NodeSelectionBeforeCancelProbes)
        length = snprintf(line, sizeof(line), "node=%u phase=%s result=pending\n",
                          (unsigned)trace_node, names[phase]);
    else
        length = snprintf(line, sizeof(line), "node=%u phase=%s result=0x%08x\n",
                          (unsigned)trace_node, names[phase], (unsigned)result);
    if (length > 0 && (size_t)length < sizeof(line))
        write_record(line, (size_t)length, false);
    errno = saved_errno;
}
