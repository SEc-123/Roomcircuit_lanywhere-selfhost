#pragma once
#include "../../common/node_lobby.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NODE_PROBE_SAMPLES 5u
#define NODE_PROBE_UNKNOWN UINT16_MAX

typedef enum { NodeMeasurePending, NodeMeasureMeasuring, NodeMeasureReady, NodeMeasureFailed } NodeProbeState;
typedef enum NodeProbeKind NodeProbeKind;
typedef struct {
    uint32_t node_id;
    uint16_t median_ms, jitter_ms;
    uint8_t sent, received, kind, state;
    uint64_t measured_at_ms;
} NodeMeasurement;

typedef struct {
    atomic_uint handle;
    atomic_uint_fast64_t deadline;
} NodeProbeGuard;

uint64_t node_probe_now_ms(void);
void node_probe_guard_poll(NodeProbeGuard *guard, bool cancel);
bool node_probe(const char *host, uint16_t port, NodeProbeKind kind, uint32_t node_id,
                const atomic_bool *cancel, NodeProbeGuard *guard, NodeMeasurement *result);
/* Negative means a ranks ahead of b. Failed/partial measurements remain visible. */
int node_measurement_compare(const NodeMeasurement *a, const NodeMeasurement *b);
bool node_measurement_recommended(const NodeMeasurement *result);
bool node_measurement_weak(const NodeMeasurement *result);

bool node_protocol_exchange(const char *host, uint16_t port, uint8_t command, const void *request,
                            size_t request_size, uint8_t expected, void *response, size_t response_size,
                            const atomic_bool *cancel);
