#include "wtf.h"

#include <errno.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
    #include <windows.h>
#endif

#define BENCH_DEFAULT_PORT 45443
#define BENCH_DEFAULT_ITERATIONS 10000
#define BENCH_DEFAULT_RUNS 5
#define BENCH_TIMEOUT_MS 30000
#define BENCH_STREAM_MAX_OUTSTANDING 1024
#define BENCH_DATAGRAM_MAX_OUTSTANDING 64

typedef enum {
    BENCH_CLIENT,
    BENCH_SERVER,
} bench_endpoint_t;

typedef struct {
    atomic_int client_connected;
    atomic_int server_connected;
    atomic_int client_disconnected;
    atomic_int server_disconnected;
    atomic_uintptr_t server_session;
    atomic_uintptr_t server_stream;
    atomic_uint_fast64_t stream_bytes_received;
    atomic_uint_fast64_t stream_sends_completed;
    atomic_uint_fast64_t datagrams_received;
    atomic_uint_fast64_t datagram_sends_final;
} bench_state_t;

typedef struct {
    bench_state_t* state;
    bench_endpoint_t endpoint;
} bench_callback_context_t;

static void sleep_ms(unsigned milliseconds)
{
#ifdef _WIN32
    Sleep(milliseconds);
#else
    struct timespec delay = {
        .tv_sec = (time_t)(milliseconds / 1000),
        .tv_nsec = (long)((milliseconds % 1000) * 1000000),
    };
    nanosleep(&delay, NULL);
#endif
}

static double monotonic_seconds(void)
{
#ifdef _WIN32
    LARGE_INTEGER frequency;
    LARGE_INTEGER counter;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)frequency.QuadPart;
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
#endif
}

static int wait_for_counter(atomic_uint_fast64_t* counter, uint64_t target, unsigned timeout_ms)
{
    for (unsigned elapsed = 0; elapsed < timeout_ms; elapsed++) {
        if (atomic_load_explicit(counter, memory_order_acquire) >= target) {
            return 0;
        }
        sleep_ms(1);
    }
    return 1;
}

static int wait_for_flag(atomic_int* flag, unsigned timeout_ms)
{
    for (unsigned elapsed = 0; elapsed < timeout_ms; elapsed++) {
        if (atomic_load_explicit(flag, memory_order_acquire) != 0) {
            return 0;
        }
        sleep_ms(1);
    }
    return 1;
}

static int wait_for_pointer(atomic_uintptr_t* pointer, unsigned timeout_ms)
{
    for (unsigned elapsed = 0; elapsed < timeout_ms; elapsed++) {
        if (atomic_load_explicit(pointer, memory_order_acquire) != 0) {
            return 0;
        }
        sleep_ms(1);
    }
    return 1;
}

static void bench_stream_callback(const wtf_stream_event_t* event)
{
    if (!event || !event->user_context) {
        return;
    }

    bench_state_t* state = event->user_context;
    switch (event->type) {
        case WTF_STREAM_EVENT_DATA_RECEIVED: {
            uint64_t bytes = 0;
            for (uint32_t i = 0; i < event->data_received.buffer_count; i++) {
                bytes += event->data_received.buffers[i].length;
            }
            atomic_fetch_add_explicit(&state->stream_bytes_received, bytes,
                                      memory_order_release);
            break;
        }
        case WTF_STREAM_EVENT_SEND_COMPLETE:
            atomic_fetch_add_explicit(&state->stream_sends_completed, 1,
                                      memory_order_release);
            break;
        default:
            break;
    }
}

static void bench_session_callback(const wtf_session_event_t* event)
{
    if (!event || !event->user_context) {
        return;
    }

    bench_callback_context_t* callback_context = event->user_context;
    bench_state_t* state = callback_context->state;
    switch (event->type) {
        case WTF_SESSION_EVENT_CONNECTED:
            if (callback_context->endpoint == BENCH_CLIENT) {
                atomic_store_explicit(&state->client_connected, 1, memory_order_release);
            } else {
                wtf_session_ref(event->session);
                uintptr_t expected = 0;
                if (!atomic_compare_exchange_strong_explicit(
                        &state->server_session, &expected, (uintptr_t)event->session,
                        memory_order_release, memory_order_relaxed)) {
                    wtf_session_unref(event->session);
                }
                atomic_store_explicit(&state->server_connected, 1, memory_order_release);
            }
            break;

        case WTF_SESSION_EVENT_STREAM_OPENED:
            wtf_stream_set_context(event->stream_opened.stream, state);
            wtf_stream_set_callback(event->stream_opened.stream, bench_stream_callback);
            if (callback_context->endpoint == BENCH_SERVER) {
                wtf_stream_ref(event->stream_opened.stream);
                uintptr_t expected = 0;
                if (!atomic_compare_exchange_strong_explicit(
                        &state->server_stream, &expected,
                        (uintptr_t)event->stream_opened.stream, memory_order_release,
                        memory_order_relaxed)) {
                    wtf_stream_unref(event->stream_opened.stream);
                }
            }
            break;

        case WTF_SESSION_EVENT_DATAGRAM_RECEIVED:
            if (callback_context->endpoint == BENCH_SERVER) {
                atomic_fetch_add_explicit(&state->datagrams_received, 1,
                                          memory_order_release);
            }
            break;

        case WTF_SESSION_EVENT_DATAGRAM_SEND_STATE_CHANGE:
            if (callback_context->endpoint == BENCH_CLIENT
                && WTF_DATAGRAM_SEND_STATE_IS_FINAL(
                    event->datagram_send_state_changed.state)) {
                atomic_fetch_add_explicit(&state->datagram_sends_final, 1,
                                          memory_order_release);
            }
            break;

        case WTF_SESSION_EVENT_DISCONNECTED:
            atomic_store_explicit(callback_context->endpoint == BENCH_CLIENT
                                      ? &state->client_disconnected
                                      : &state->server_disconnected,
                                  1, memory_order_release);
            break;

        default:
            break;
    }
}

static wtf_connection_decision_t bench_connection_validator(
    const wtf_connection_request_t* request, wtf_connection_response_t* response,
    void* user_context)
{
    (void)request;
    (void)response;
    (void)user_context;
    return WTF_CONNECTION_ACCEPT;
}

static int parse_positive_uint(const char* value, unsigned* result)
{
    char* end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(value, &end, 10);
    if (errno != 0 || !end || *end != '\0' || parsed == 0 || parsed > UINT32_MAX) {
        return 1;
    }
    *result = (unsigned)parsed;
    return 0;
}

static int run_stream_case(bench_state_t* state, wtf_stream_t* stream, const char* label,
                           unsigned run, size_t payload_size, unsigned iterations, bool emit)
{
    uint8_t* payload = malloc(payload_size);
    if (!payload) {
        return 1;
    }
    memset(payload, 0xa5, payload_size);

    uint64_t initial_completed = atomic_load_explicit(&state->stream_sends_completed,
                                                       memory_order_acquire);
    uint64_t initial_received = atomic_load_explicit(&state->stream_bytes_received,
                                                      memory_order_acquire);
    uint64_t completed_target = initial_completed + iterations;
    uint64_t received_target = initial_received + (uint64_t)iterations * payload_size;
    double started = monotonic_seconds();

    for (unsigned sent = 0; sent < iterations;) {
        uint64_t completed = atomic_load_explicit(&state->stream_sends_completed,
                                                  memory_order_acquire);
        if ((uint64_t)sent + initial_completed - completed
            >= BENCH_STREAM_MAX_OUTSTANDING) {
            sleep_ms(1);
            continue;
        }

        wtf_result_t result = wtf_stream_send_copy(stream, payload, payload_size, false);
        if (result == WTF_SUCCESS) {
            sent++;
        } else if (result == WTF_ERROR_FLOW_CONTROL) {
            sleep_ms(1);
        } else {
            fprintf(stderr, "benchmark: stream send failed: %s\n",
                    wtf_result_to_string(result));
            free(payload);
            return 1;
        }
    }

    if (wait_for_counter(&state->stream_sends_completed, completed_target,
                         BENCH_TIMEOUT_MS)
        || wait_for_counter(&state->stream_bytes_received, received_target,
                            BENCH_TIMEOUT_MS)) {
        fprintf(stderr, "benchmark: timed out waiting for stream completion\n");
        free(payload);
        return 1;
    }

    double elapsed = monotonic_seconds() - started;
    if (emit) {
        double operations_per_second = (double)iterations / elapsed;
        double mib_per_second = ((double)iterations * (double)payload_size)
            / (elapsed * 1024.0 * 1024.0);
        printf("%s,%u,stream-copy,%zu,%u,%.9f,%.3f,%.3f,%u\n", label, run,
               payload_size, iterations, elapsed, operations_per_second, mib_per_second,
               iterations);
        fflush(stdout);
    }
    free(payload);
    return 0;
}

static int run_datagram_case(bench_state_t* state, wtf_session_t* session,
                             const char* label, unsigned run, size_t payload_size,
                             unsigned iterations, bool emit)
{
    uint8_t* payload = malloc(payload_size);
    if (!payload) {
        return 1;
    }
    memset(payload, 0x5a, payload_size);

    uint64_t initial_final = atomic_load_explicit(&state->datagram_sends_final,
                                                   memory_order_acquire);
    uint64_t initial_received = atomic_load_explicit(&state->datagrams_received,
                                                      memory_order_acquire);
    uint64_t final_target = initial_final + iterations;
    uint64_t received_target = initial_received + iterations;
    double started = monotonic_seconds();

    for (unsigned sent = 0; sent < iterations;) {
        uint64_t finalized = atomic_load_explicit(&state->datagram_sends_final,
                                                   memory_order_acquire);
        if ((uint64_t)sent + initial_final - finalized
            >= BENCH_DATAGRAM_MAX_OUTSTANDING) {
            sleep_ms(1);
            continue;
        }

        wtf_result_t result = wtf_session_send_datagram_copy(session, payload, payload_size);
        if (result != WTF_SUCCESS) {
            fprintf(stderr, "benchmark: datagram send failed: %s\n",
                    wtf_result_to_string(result));
            free(payload);
            return 1;
        }
        sent++;
    }

    if (wait_for_counter(&state->datagram_sends_final, final_target, BENCH_TIMEOUT_MS)
        || wait_for_counter(&state->datagrams_received, received_target,
                            BENCH_TIMEOUT_MS)) {
        fprintf(stderr,
                "benchmark: timed out waiting for datagrams (final=%" PRIuFAST64
                ", received=%" PRIuFAST64 ")\n",
                atomic_load(&state->datagram_sends_final),
                atomic_load(&state->datagrams_received));
        free(payload);
        return 1;
    }

    double elapsed = monotonic_seconds() - started;
    if (emit) {
        double operations_per_second = (double)iterations / elapsed;
        double mib_per_second = ((double)iterations * (double)payload_size)
            / (elapsed * 1024.0 * 1024.0);
        printf("%s,%u,datagram-copy,%zu,%u,%.9f,%.3f,%.3f,%u\n", label, run,
               payload_size, iterations, elapsed, operations_per_second, mib_per_second,
               iterations);
        fflush(stdout);
    }
    free(payload);
    return 0;
}

static void print_usage(const char* executable)
{
    fprintf(stderr,
            "Usage: %s --cert-dir DIR [--label NAME] [--port PORT] "
            "[--iterations N] [--runs N]\n",
            executable);
}

int main(int argc, char** argv)
{
    const char* cert_dir = NULL;
    const char* label = "working-tree";
    unsigned port = BENCH_DEFAULT_PORT;
    unsigned iterations = BENCH_DEFAULT_ITERATIONS;
    unsigned runs = BENCH_DEFAULT_RUNS;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--cert-dir") == 0 && i + 1 < argc) {
            cert_dir = argv[++i];
        } else if (strcmp(argv[i], "--label") == 0 && i + 1 < argc) {
            label = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            if (parse_positive_uint(argv[++i], &port) || port > UINT16_MAX) {
                print_usage(argv[0]);
                return 2;
            }
        } else if (strcmp(argv[i], "--iterations") == 0 && i + 1 < argc) {
            if (parse_positive_uint(argv[++i], &iterations)) {
                print_usage(argv[0]);
                return 2;
            }
        } else if (strcmp(argv[i], "--runs") == 0 && i + 1 < argc) {
            if (parse_positive_uint(argv[++i], &runs)) {
                print_usage(argv[0]);
                return 2;
            }
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            print_usage(argv[0]);
            return 2;
        }
    }

    if (!cert_dir) {
        print_usage(argv[0]);
        return 2;
    }

    char cert_path[1024];
    char key_path[1024];
    int cert_length = snprintf(cert_path, sizeof(cert_path), "%s/localhost.crt", cert_dir);
    int key_length = snprintf(key_path, sizeof(key_path), "%s/localhost.key", cert_dir);
    if (cert_length < 0 || cert_length >= (int)sizeof(cert_path) || key_length < 0
        || key_length >= (int)sizeof(key_path)) {
        fprintf(stderr, "benchmark: certificate path is too long\n");
        return 1;
    }

    bench_state_t state;
    atomic_init(&state.client_connected, 0);
    atomic_init(&state.server_connected, 0);
    atomic_init(&state.client_disconnected, 0);
    atomic_init(&state.server_disconnected, 0);
    atomic_init(&state.server_session, 0);
    atomic_init(&state.server_stream, 0);
    atomic_init(&state.stream_bytes_received, 0);
    atomic_init(&state.stream_sends_completed, 0);
    atomic_init(&state.datagrams_received, 0);
    atomic_init(&state.datagram_sends_final, 0);

    bench_callback_context_t client_events = {.state = &state, .endpoint = BENCH_CLIENT};
    bench_callback_context_t server_events = {.state = &state, .endpoint = BENCH_SERVER};
    wtf_context_config_t context_config = {
        .log_level = WTF_LOG_LEVEL_NONE,
        .worker_thread_count = 2,
        .execution_profile = WTF_EXECUTION_PROFILE_MAX_THROUGHPUT,
    };
    wtf_context_t* context = NULL;
    wtf_server_t* server = NULL;
    wtf_client_t* client = NULL;
    wtf_session_t* client_session = NULL;
    wtf_stream_t* client_stream = NULL;
    int failure = 1;

    wtf_result_t result = wtf_context_create(&context_config, &context);
    if (result != WTF_SUCCESS) {
        fprintf(stderr, "benchmark: context create failed: %s\n",
                wtf_result_to_string(result));
        goto cleanup;
    }

    wtf_certificate_config_t certificate_config = {
        .cert_type = WTF_CERT_TYPE_FILE,
        .cert_data.file = {.cert_path = cert_path, .key_path = key_path},
    };
    wtf_server_config_t server_config = {
        .host = "127.0.0.1",
        .port = (uint16_t)port,
        .cert_config = &certificate_config,
        .draft = WTF_WEBTRANSPORT_DRAFT_AUTO,
        .max_sessions_per_connection = 1,
        .max_streams_per_session = 8,
        .max_data_per_session = 1024ULL * 1024ULL * 1024ULL,
        .connection_validator = bench_connection_validator,
        .session_callback = bench_session_callback,
        .user_context = &server_events,
    };
    result = wtf_server_create(context, &server_config, &server);
    if (result != WTF_SUCCESS || wtf_server_start(server) != WTF_SUCCESS) {
        fprintf(stderr, "benchmark: server setup failed\n");
        goto cleanup;
    }

    char url[128];
    snprintf(url, sizeof(url), "https://127.0.0.1:%u/benchmark", port);
    wtf_client_config_t client_config = {
        .url = url,
        .draft = WTF_WEBTRANSPORT_DRAFT_AUTO,
        .allow_pooling = false,
        .require_unreliable = true,
        .pinned_server_certificate_file = cert_path,
        .max_streams_per_session = 8,
        .max_data_per_session = 1024ULL * 1024ULL * 1024ULL,
        .session_callback = bench_session_callback,
        .user_context = &client_events,
    };
    result = wtf_client_create(context, &client_config, &client);
    if (result != WTF_SUCCESS || wtf_client_open(client, &client_session) != WTF_SUCCESS) {
        fprintf(stderr, "benchmark: client setup failed\n");
        goto cleanup;
    }

    if (wait_for_flag(&state.client_connected, BENCH_TIMEOUT_MS)
        || wait_for_flag(&state.server_connected, BENCH_TIMEOUT_MS)) {
        fprintf(stderr, "benchmark: session connection timed out\n");
        goto cleanup;
    }

    result = wtf_session_create_stream(client_session, WTF_STREAM_BIDIRECTIONAL,
                                       &client_stream);
    if (result != WTF_SUCCESS) {
        fprintf(stderr, "benchmark: stream creation failed: %s\n",
                wtf_result_to_string(result));
        goto cleanup;
    }
    wtf_stream_set_context(client_stream, &state);
    wtf_stream_set_callback(client_stream, bench_stream_callback);
    if (wait_for_pointer(&state.server_stream, BENCH_TIMEOUT_MS)) {
        fprintf(stderr, "benchmark: peer stream creation timed out\n");
        goto cleanup;
    }

    unsigned warmup_iterations = iterations / 10;
    if (warmup_iterations < 100) {
        warmup_iterations = 100;
    } else if (warmup_iterations > 1000) {
        warmup_iterations = 1000;
    }
    if (run_stream_case(&state, client_stream, label, 0, 64, warmup_iterations, false)
        || run_datagram_case(&state, client_session, label, 0, 64,
                             warmup_iterations, false)) {
        goto cleanup;
    }

    printf("label,run,case,payload_bytes,iterations,seconds,ops_per_second,mib_per_second,received\n");
    for (unsigned run = 1; run <= runs; run++) {
        if (run_stream_case(&state, client_stream, label, run, 64, iterations, true)
            || run_stream_case(&state, client_stream, label, run, 4096, iterations,
                               true)
            || run_datagram_case(&state, client_session, label, run, 64, iterations,
                                 true)
            || run_datagram_case(&state, client_session, label, run, 1024,
                                 iterations, true)) {
            goto cleanup;
        }
    }
    failure = 0;

cleanup:
    if (client_stream) {
        wtf_stream_close(client_stream);
        wtf_stream_unref(client_stream);
    }
    wtf_stream_t* server_stream = (wtf_stream_t*)atomic_exchange(&state.server_stream, 0);
    if (server_stream) {
        wtf_stream_unref(server_stream);
    }
    if (client_session) {
        wtf_session_close(client_session, 0, "benchmark complete");
        wait_for_flag(&state.client_disconnected, 2000);
        wtf_session_unref(client_session);
    }
    wtf_session_t* server_session = (wtf_session_t*)atomic_exchange(&state.server_session, 0);
    if (server_session) {
        wtf_session_unref(server_session);
    }
    if (client) {
        wtf_client_disconnect(client, 0, "benchmark complete");
        wtf_client_destroy(client);
    }
    if (server) {
        wtf_server_stop(server);
        wtf_server_destroy(server);
    }
    if (context) {
        wtf_context_destroy(context);
    }
    return failure;
}
