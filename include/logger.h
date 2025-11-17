#ifndef LOGGER_H
#define LOGGER_H

#include <pulsar/pulsar.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Logs HTTP request information with colored output.
 * @param ctx The Pulsar context containing request/response data.
 * @param total_ns Total request duration in nanoseconds.
 */
void logger(PulsarCtx* ctx, uint64_t total_ns);

#ifdef __cplusplus
}
#endif

#endif  // LOGGER_H
