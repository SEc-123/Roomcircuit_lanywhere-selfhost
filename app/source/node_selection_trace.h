#pragma once
#include <stdint.h>

typedef enum {
    NodeSelectionBeforeIpc,
    NodeSelectionAfterIpc,
    NodeSelectionBeforeCancelProbes,
    NodeSelectionAfterCancelProbes,
} NodeSelectionTracePhase;

void node_selection_trace_begin(uint32_t node_id);
void node_selection_trace_phase(NodeSelectionTracePhase phase, uint32_t result);
