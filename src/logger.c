#include <pulsar/pulsar.h>
#include <stdio.h>   // for snprintf, perror
#include <unistd.h>  // for write, STDOUT_FILENO

#define LOG_BUFFER_SIZE 1024

// ANSI color codes
#define ANSI_RESET  "\033[0m"
#define ANSI_CYAN   "\033[36m"  // Method
#define ANSI_BLUE   "\033[34m"  // Path
#define ANSI_GREEN  "\033[32m"  // 2xx status / fast latency
#define ANSI_YELLOW "\033[33m"  // 3xx status / medium latency
#define ANSI_RED    "\033[31m"  // 4xx/5xx status / slow latency

static thread_local char log_buffer[LOG_BUFFER_SIZE];

/**
 * Logs HTTP request information with colored output.
 * @param ctx The Pulsar context containing request/response data.
 * @param total_ns Total request duration in nanoseconds.
 */
void logger(PulsarCtx* ctx, uint64_t total_ns) {
    PulsarConn* conn        = ctx->conn;
    const char* method      = req_method(conn);
    const char* path        = req_path(conn);
    http_status status_code = res_get_status(conn);

    // Format latency with appropriate unit (right-aligned within 7 chars)
    char latency_str[32];
    if (total_ns < 1000) {
        // Nanoseconds
        snprintf(latency_str, sizeof(latency_str), "%6luns", total_ns);
    } else if (total_ns < 1000000) {
        // Microseconds
        snprintf(latency_str, sizeof(latency_str), "%5luµs", total_ns / 1000);
    } else if (total_ns < 1000000000) {
        // Milliseconds
        snprintf(latency_str, sizeof(latency_str), "%6lums", total_ns / 1000000);
    } else if (total_ns < 60000000000) {
        // Seconds
        snprintf(latency_str, sizeof(latency_str), "%6lus", total_ns / 1000000000);
    } else {
        // Minutes
        snprintf(latency_str, sizeof(latency_str), "%6lum", total_ns / 60000000000);
    }

    // Determine status code color
    const char* status_color = ANSI_GREEN;
    if (status_code >= 500) {
        status_color = ANSI_RED;
    } else if (status_code >= 400) {
        status_color = ANSI_RED;
    } else if (status_code >= 300) {
        status_color = ANSI_YELLOW;
    }

    // Determine latency color based on thresholds
    const char* latency_color = ANSI_GREEN;
    if (total_ns >= 1000000000) {  // >= 1s
        latency_color = ANSI_RED;
    } else if (total_ns >= 100000000) {  // >= 100ms
        latency_color = ANSI_YELLOW;
    }

    // Build the log line in our buffer
    char* ptr       = log_buffer;
    const char* end = log_buffer + LOG_BUFFER_SIZE - 1;

    // [Pulsar]
    ptr += snprintf(ptr, (size_t)(end - ptr), "[Pulsar] ");

    // Method (colored, 4 chars width, left-aligned)
    ptr += snprintf(ptr, (size_t)(end - ptr), "%s%-4s" ANSI_RESET " ", ANSI_CYAN, method);

    // Path (colored, 20 chars width, left-aligned)
    ptr += snprintf(ptr, (size_t)(end - ptr), "%s%-20s" ANSI_RESET " ", ANSI_BLUE, path);

    // Status code (colored, 3 digits)
    ptr += snprintf(ptr, (size_t)(end - ptr), "%s%3d" ANSI_RESET " ", status_color, status_code);

    // Latency (colored, right-aligned)
    ptr += snprintf(ptr, (size_t)(end - ptr), "%s%s" ANSI_RESET "\n", latency_color, latency_str);

    // Single write to stdout
    if (write(STDOUT_FILENO, log_buffer, (size_t)(ptr - log_buffer)) == -1) {
        perror("write");
    }
}
