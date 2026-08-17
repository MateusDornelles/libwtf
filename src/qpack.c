#include "qpack.h"

#include "log.h"
#include "lsxpack_header.h"
#include "utils.h"

#define WTF_MAX_CONNECT_HEADERS 256
#define WTF_HEADER_STORAGE_BLOCK_SIZE 4096

struct wtf_header_storage_block {
    struct wtf_header_storage_block* next;
    size_t capacity;
    size_t used;
    char data[];
};

static char* wtf_header_storage_copy(wtf_header_storage* storage, const char* value, size_t length)
{
    if (!storage || !value || length == SIZE_MAX) {
        return NULL;
    }

    size_t required = length + 1;
    char* destination = NULL;
    if (required <= sizeof(storage->inline_data) - storage->inline_used) {
        destination = storage->inline_data + storage->inline_used;
        storage->inline_used += required;
    } else {
        wtf_header_storage_block* block = storage->blocks;
        if (!block || required > block->capacity - block->used) {
            size_t capacity = max(required, (size_t)WTF_HEADER_STORAGE_BLOCK_SIZE);
            if (capacity > SIZE_MAX - sizeof(*block)) {
                return NULL;
            }
            block = malloc(sizeof(*block) + capacity);
            if (!block) {
                return NULL;
            }
            block->next = storage->blocks;
            block->capacity = capacity;
            block->used = 0;
            storage->blocks = block;
        }
        destination = block->data + block->used;
        block->used += required;
    }

    memcpy(destination, value, length);
    destination[length] = '\0';
    return destination;
}

static void wtf_header_storage_cleanup(wtf_header_storage* storage)
{
    if (!storage) {
        return;
    }

    wtf_header_storage_block* block = storage->blocks;
    while (block) {
        wtf_header_storage_block* next = block->next;
        free(block);
        block = next;
    }
    storage->blocks = NULL;
    storage->inline_used = 0;
}

static void wtf_qpack_unblocked(void* context WTF_MAYBE_UNUSED)
{
}

static struct lsxpack_header* wtf_qpack_prepare_decode(void* context, struct lsxpack_header* header,
                                                       size_t space)
{
    wtf_header_decode_context* ctx = (wtf_header_decode_context*)context;

    if (!ctx)
        return NULL;

    if (space > sizeof(ctx->decode_buffer)) {
        if (ctx->connection && ctx->connection->context) {
            WTF_LOG_ERROR(ctx->connection->context, "qpack", "Header too large: %zu bytes",
                          space);
        }
        return NULL;
    }

    if (header) {
        header->buf = ctx->decode_buffer;
        header->val_len = (lsxpack_strlen_t)space;
    } else {
        header = &ctx->current_header;
        lsxpack_header_prepare_decode(header, ctx->decode_buffer, 0, space);
    }

    return header;
}

static wtf_context* get_log_context(wtf_header_decode_context* ctx)
{
    return (ctx && ctx->connection) ? ctx->connection->context : NULL;
}

static bool validate_header_size(wtf_header_decode_context* ctx, size_t name_len, size_t value_len)
{
    if (name_len > 256 || value_len > 4096) {
        wtf_context* log_ctx = get_log_context(ctx);
        if (log_ctx) {
            WTF_LOG_ERROR(log_ctx, "qpack", "Header too large: name=%zu, value=%zu", name_len,
                          value_len);
        }
        return false;
    }
    return true;
}

static bool update_request_field(wtf_connect_request* request, char** field, const char* value,
                                 size_t value_len)
{
    char* copy = wtf_header_storage_copy(&request->storage, value, value_len);
    if (!copy) {
        return false;
    }

    *field = copy;
    return true;
}

static bool wtf_connect_request_add_header(wtf_connect_request* request, const char* name,
                                           size_t name_len, const char* value, size_t value_len,
                                           wtf_context* log_ctx);

static bool update_pseudo_field(wtf_header_decode_context* ctx, char** field, bool* seen,
                                const char* field_name, const char* value, size_t value_len,
                                wtf_context* log_ctx)
{
    if (*seen) {
        if (log_ctx) {
            WTF_LOG_ERROR(log_ctx, "qpack", "Duplicate pseudo-header: %s", field_name);
        }
        ctx->malformed_header_block = true;
        return false;
    }

    *seen = true;
    return update_request_field(ctx->request, field, value, value_len);
}

static bool process_pseudo_header(wtf_header_decode_context* ctx, const char* name,
                                  size_t name_len, const char* value, size_t value_len,
                                  wtf_context* log_ctx)
{
    wtf_connect_request* request = ctx->request;

    if (name_len == 7 && strncmp(name, ":method", 7) == 0) {
        return update_pseudo_field(ctx, &request->method, &ctx->seen_method, ":method", value,
                                   value_len, log_ctx);
    } else if (name_len == 7 && strncmp(name, ":scheme", 7) == 0) {
        return update_pseudo_field(ctx, &request->scheme, &ctx->seen_scheme, ":scheme", value,
                                   value_len, log_ctx);
    } else if (name_len == 10 && strncmp(name, ":authority", 10) == 0) {
        return update_pseudo_field(ctx, &request->authority, &ctx->seen_authority, ":authority",
                                   value, value_len, log_ctx);
    } else if (name_len == 5 && strncmp(name, ":path", 5) == 0) {
        return update_pseudo_field(ctx, &request->path, &ctx->seen_path, ":path", value,
                                   value_len, log_ctx);
    } else if (name_len == 9 && strncmp(name, ":protocol", 9) == 0) {
        return update_pseudo_field(ctx, &request->protocol, &ctx->seen_protocol, ":protocol",
                                   value, value_len, log_ctx);
    }

    if (log_ctx) {
        WTF_LOG_ERROR(log_ctx, "qpack", "Unknown pseudo-header: %.*s", (int)name_len, name);
    }
    ctx->malformed_header_block = true;
    return false;
}

static bool validate_no_late_pseudo_header(wtf_header_decode_context* ctx, const char* name,
                                           size_t name_len, wtf_context* log_ctx)
{
    if (ctx->seen_regular_header) {
        if (log_ctx) {
            WTF_LOG_ERROR(log_ctx, "qpack", "Pseudo-header after regular header: %.*s",
                          (int)name_len, name);
        }
        ctx->malformed_header_block = true;
        return false;
    }
    return true;
}

static bool validate_regular_header_name(const char* name, size_t name_len, wtf_context* log_ctx)
{
    for (size_t i = 0; i < name_len; i++) {
        if (name[i] == ':') {
            if (log_ctx) {
                WTF_LOG_ERROR(log_ctx, "qpack", "Invalid regular header name: %.*s",
                              (int)name_len, name);
            }
            return false;
        }
        if (name[i] >= 'A' && name[i] <= 'Z') {
            if (log_ctx) {
                WTF_LOG_ERROR(log_ctx, "qpack", "Uppercase field name is malformed: %.*s",
                              (int)name_len, name);
            }
            return false;
        }
    }
    return true;
}

static bool process_regular_header(wtf_header_decode_context* ctx, const char* name,
                                   size_t name_len, const char* value, size_t value_len,
                                   wtf_context* log_ctx)
{
    wtf_connect_request* request = ctx->request;

    if (!validate_regular_header_name(name, name_len, log_ctx)) {
        ctx->malformed_header_block = true;
        return false;
    }

    ctx->seen_regular_header = true;

    if (!wtf_connect_request_add_header(request, name, name_len, value, value_len, log_ctx)) {
        return false;
    }

    if (name_len == 6 && strncmp(name, "origin", 6) == 0) {
        if (request->origin) {
            if (log_ctx) {
                WTF_LOG_ERROR(log_ctx, "qpack", "Duplicate origin header");
            }
            ctx->malformed_header_block = true;
            return false;
        }
        request->origin = (char*)request->headers[request->header_count - 1].value;
        return true;
    }

    return true;
}

static bool process_header_name(wtf_header_decode_context* ctx, const char* name, size_t name_len,
                                const char* value, size_t value_len, wtf_context* log_ctx)
{
    if (name[0] == ':') {
        if (!validate_no_late_pseudo_header(ctx, name, name_len, log_ctx)) {
            return false;
        }
        return process_pseudo_header(ctx, name, name_len, value, value_len, log_ctx);
    }

    return process_regular_header(ctx, name, name_len, value, value_len, log_ctx);
}

static bool validate_required_connect_headers(wtf_context* ctx, wtf_connect_request* request)
{
    if (!request->method || strcmp(request->method, WTF_CONNECT_METHOD) != 0) {
        WTF_LOG_ERROR(ctx, "qpack", "Invalid or missing :method header");
        return false;
    }
    if (!request->protocol || request->protocol[0] == '\0') {
        WTF_LOG_ERROR(ctx, "qpack", "Missing :protocol header");
        return false;
    }
    if (!request->scheme || request->scheme[0] == '\0') {
        WTF_LOG_ERROR(ctx, "qpack", "Missing :scheme header");
        return false;
    }
    if (!request->authority || request->authority[0] == '\0') {
        WTF_LOG_ERROR(ctx, "qpack", "Missing :authority header");
        return false;
    }
    if (!request->path || request->path[0] == '\0') {
        WTF_LOG_ERROR(ctx, "qpack", "Missing :path header");
        return false;
    }
    return true;
}

static bool wtf_connect_request_add_header(wtf_connect_request* request, const char* name,
                                           size_t name_len, const char* value, size_t value_len,
                                           wtf_context* log_ctx)
{
    if (request->header_count >= WTF_MAX_CONNECT_HEADERS) {
        if (log_ctx) {
            WTF_LOG_ERROR(log_ctx, "qpack", "Too many CONNECT request headers");
        }
        return false;
    }

    if (request->header_count == request->header_capacity) {
        size_t new_capacity = request->header_capacity == 0 ? 8 : request->header_capacity * 2;
        if (new_capacity > WTF_MAX_CONNECT_HEADERS) {
            new_capacity = WTF_MAX_CONNECT_HEADERS;
        }

        wtf_http_header_t* headers = realloc(request->headers, new_capacity * sizeof(*headers));
        if (!headers) {
            if (log_ctx) {
                WTF_LOG_ERROR(log_ctx, "qpack", "Failed to grow CONNECT header list");
            }
            return false;
        }

        request->headers = headers;
        request->header_capacity = new_capacity;
    }

    char* header_name = wtf_header_storage_copy(&request->storage, name, name_len);
    char* header_value = wtf_header_storage_copy(&request->storage, value, value_len);
    if (!header_name || !header_value) {
        if (log_ctx) {
            WTF_LOG_ERROR(log_ctx, "qpack", "Failed to copy CONNECT header");
        }
        return false;
    }

    request->headers[request->header_count++] = (wtf_http_header_t){
        .name = header_name,
        .value = header_value,
    };
    return true;
}

static bool wtf_connect_response_add_header(wtf_connect_response* response, const char* name,
                                            size_t name_len, const char* value, size_t value_len,
                                            wtf_context* log_ctx)
{
    if (response->header_count >= WTF_MAX_CONNECT_HEADERS) {
        if (log_ctx) {
            WTF_LOG_ERROR(log_ctx, "qpack", "Too many CONNECT response headers");
        }
        return false;
    }

    if (response->header_count == response->header_capacity) {
        size_t new_capacity = response->header_capacity == 0 ? 8 : response->header_capacity * 2;
        if (new_capacity > WTF_MAX_CONNECT_HEADERS) {
            new_capacity = WTF_MAX_CONNECT_HEADERS;
        }

        wtf_http_header_t* headers = realloc(response->headers, new_capacity * sizeof(*headers));
        if (!headers) {
            if (log_ctx) {
                WTF_LOG_ERROR(log_ctx, "qpack", "Failed to grow CONNECT response header list");
            }
            return false;
        }

        response->headers = headers;
        response->header_capacity = new_capacity;
    }

    char* header_name = wtf_header_storage_copy(&response->storage, name, name_len);
    char* header_value = wtf_header_storage_copy(&response->storage, value, value_len);
    if (!header_name || !header_value) {
        if (log_ctx) {
            WTF_LOG_ERROR(log_ctx, "qpack", "Failed to copy CONNECT response header");
        }
        return false;
    }

    response->headers[response->header_count++] = (wtf_http_header_t){
        .name = header_name,
        .value = header_value,
    };
    return true;
}

static bool parse_status_code(const char* value, size_t value_len, uint16_t* status_code)
{
    if (!value || !status_code || value_len != 3) {
        return false;
    }

    uint16_t code = 0;
    for (size_t i = 0; i < value_len; i++) {
        if (value[i] < '0' || value[i] > '9') {
            return false;
        }
        code = (uint16_t)(code * 10 + (uint16_t)(value[i] - '0'));
    }

    if (code < 100 || code > 599) {
        return false;
    }

    *status_code = code;
    return true;
}

static bool process_response_header(wtf_header_decode_context* ctx, const char* name,
                                    size_t name_len, const char* value, size_t value_len,
                                    wtf_context* log_ctx)
{
    wtf_connect_response* response = ctx->response;

    if (name[0] == ':') {
        if (!validate_no_late_pseudo_header(ctx, name, name_len, log_ctx)) {
            return false;
        }

        if (name_len == 7 && strncmp(name, ":status", 7) == 0) {
            if (ctx->seen_status) {
                if (log_ctx) {
                    WTF_LOG_ERROR(log_ctx, "qpack", "Duplicate pseudo-header: :status");
                }
                ctx->malformed_header_block = true;
                return false;
            }
            ctx->seen_status = true;
            if (!parse_status_code(value, value_len, &response->status_code)) {
                if (log_ctx) {
                    WTF_LOG_ERROR(log_ctx, "qpack", "Invalid :status value");
                }
                ctx->malformed_header_block = true;
                return false;
            }
            return true;
        }

        if (log_ctx) {
            WTF_LOG_ERROR(log_ctx, "qpack", "Unknown response pseudo-header: %.*s",
                          (int)name_len, name);
        }
        ctx->malformed_header_block = true;
        return false;
    }

    if (!validate_regular_header_name(name, name_len, log_ctx)) {
        ctx->malformed_header_block = true;
        return false;
    }

    ctx->seen_regular_header = true;

    if (!wtf_connect_response_add_header(response, name, name_len, value, value_len, log_ctx)) {
        return false;
    }

    if (name_len == 28 && strncmp(name, "sec-webtransport-http3-draft", 28) == 0) {
        if (response->draft_header) {
            if (log_ctx) {
                WTF_LOG_ERROR(log_ctx, "qpack", "Duplicate draft response header");
            }
            ctx->malformed_header_block = true;
            return false;
        }
        response->draft_header = (char*)response->headers[response->header_count - 1].value;
        return true;
    }

    return true;
}

static int wtf_qpack_process_header(void* context, struct lsxpack_header* header)
{
    // Validate input parameters
    wtf_header_decode_context* ctx = (wtf_header_decode_context*)context;
    if (!ctx || !header || !header->buf) {
        return -1;
    }

    if (ctx->decode_response) {
        if (!ctx->response) {
            return -1;
        }
    } else if (!ctx->request) {
        return -1;
    }

    const char* name = (header->name_len > 0)
        ? (const char*)(header->buf + header->name_offset)
        : NULL;
    const char* value = (header->val_len > 0)
        ? (const char*)(header->buf + header->val_offset)
        : "";
    size_t name_len = header->name_len;
    size_t value_len = header->val_len;

    if (!name || name_len == 0) {
        wtf_context* log_ctx = get_log_context(ctx);
        if (log_ctx) {
            WTF_LOG_ERROR(log_ctx, "qpack", "Header missing name");
        }
        return -1;
    }

    if (!validate_header_size(ctx, name_len, value_len)) {
        return -1;
    }

    wtf_context* log_ctx = get_log_context(ctx);
    if (log_ctx) {
        WTF_LOG_TRACE(log_ctx, "qpack", "Processing header: %.*s = %.*s", (int)name_len, name,
                      (int)value_len, value);
    }

    bool success = ctx->decode_response
        ? process_response_header(ctx, name, name_len, value, value_len, log_ctx)
        : process_header_name(ctx, name, name_len, value, value_len, log_ctx);
    if (!success) {
        return -1;
    }

    ctx->header_count++;
    return 0;
}


static const struct lsqpack_dec_hset_if wtf_qpack_decoder_interface = {
    .dhi_unblocked = wtf_qpack_unblocked,
    .dhi_prepare_decode = wtf_qpack_prepare_decode,
    .dhi_process_header = wtf_qpack_process_header,
};

bool wtf_qpack_preinit(wtf_qpack_context* qpack, uint32_t max_table_capacity,
                       uint32_t max_blocked_streams)
{
    if (!qpack)
        return false;
    memset(qpack, 0, sizeof(*qpack));
    mtx_init(&qpack->mutex, mtx_plain);
    qpack->max_table_capacity = max_table_capacity;
    qpack->max_blocked_streams = max_blocked_streams;
    qpack->peer_max_table_capacity = 0;
    qpack->peer_blocked_streams = 0;
    qpack->initialized = false;
    qpack->tsu_buf_sz = sizeof(qpack->tsu_buf);
    lsqpack_enc_preinit(&qpack->encoder, NULL);
    lsqpack_dec_init(&qpack->decoder, NULL, 0, 0, &wtf_qpack_decoder_interface, 0);
    return true;
}

void wtf_qpack_cleanup(wtf_qpack_context* qpack)
{
    if (!qpack)
        return;

    mtx_lock(&qpack->mutex);

    if (qpack->initialized) {
        lsqpack_enc_cleanup(&qpack->encoder);
        lsqpack_dec_cleanup(&qpack->decoder);
        qpack->initialized = false;
    }

    mtx_unlock(&qpack->mutex);
    mtx_destroy(&qpack->mutex);
}

bool wtf_qpack_init_encoder(wtf_context* ctx, wtf_qpack_context* qpack)
{
    bool success = false;

    if (!qpack) {
        return false;
    }

    mtx_lock(&qpack->mutex);

    if (qpack->initialized) {
        WTF_LOG_ERROR(ctx, "qpack", "QPACK encoder already initialized");
        goto cleanup_unlock;
    }

    uint32_t table_capacity = min(qpack->max_table_capacity, qpack->peer_max_table_capacity);
    uint32_t blocked_streams = (uint32_t)min(qpack->max_blocked_streams,
                                             qpack->peer_blocked_streams);

    if (table_capacity == 0) {
        table_capacity = WTF_QPACK_DYNAMIC_TABLE_SIZE;
    }
    if (blocked_streams == 0) {
        blocked_streams = WTF_QPACK_MAX_BLOCKED_STREAMS;
    }

    qpack->tsu_buf_sz = sizeof(qpack->tsu_buf);
    int result = lsqpack_enc_init(&qpack->encoder, NULL, table_capacity, table_capacity,
                                  blocked_streams, LSQPACK_ENC_OPT_STAGE_2, qpack->tsu_buf,
                                  &qpack->tsu_buf_sz);

    if (result != 0) {
        WTF_LOG_ERROR(ctx, "qpack", "Failed to initialize QPACK encoder: %d", result);
        goto cleanup_unlock;
    }

    lsqpack_dec_cleanup(&qpack->decoder);
    lsqpack_dec_init(&qpack->decoder, NULL, table_capacity, blocked_streams,
                     &wtf_qpack_decoder_interface, (enum lsqpack_dec_opts)0);

    qpack->initialized = true;
    success = true;

cleanup_unlock:
    mtx_unlock(&qpack->mutex);
    return success;
}

bool wtf_qpack_process_decoder(wtf_context* ctx, wtf_qpack_context* qpack, const uint8_t* data,
                               size_t length)
{
    bool success = false;

    if (!qpack || !data || length == 0) {
        goto cleanup;
    }

    mtx_lock(&qpack->mutex);

    if (!qpack->initialized) {
        WTF_LOG_ERROR(ctx, "qpack", "QPACK not initialized");
        goto cleanup_unlock;
    }

    int result = lsqpack_enc_decoder_in(&qpack->encoder, data, length);
    if (result != 0) {
        WTF_LOG_ERROR(ctx, "qpack", "Failed to process QPACK decoder input: %d", result);
        goto cleanup_unlock;
    }

    success = true;

cleanup_unlock:
    mtx_unlock(&qpack->mutex);
cleanup:
    return success;
}

bool wtf_qpack_process_encoder(wtf_context* ctx, wtf_qpack_context* qpack, const uint8_t* data,
                               size_t length)
{
    bool success = false;

    if (!qpack || !data || length == 0) {
        goto cleanup;
    }

    mtx_lock(&qpack->mutex);

    if (!qpack->initialized) {
        WTF_LOG_ERROR(ctx, "qpack", "QPACK not initialized");
        goto cleanup_unlock;
    }

    int result = lsqpack_dec_enc_in(&qpack->decoder, data, length);
    if (result != 0) {
        WTF_LOG_ERROR(ctx, "qpack", "Failed to process QPACK encoder input: %d", result);
        goto cleanup_unlock;
    }

    success = true;

cleanup_unlock:
    mtx_unlock(&qpack->mutex);
cleanup:
    return success;
}

void wtf_connect_request_cleanup(wtf_connect_request* request)
{
    if (!request) {
        return;
    }

    free(request->headers);
    wtf_header_storage_cleanup(&request->storage);

    memset(request, 0, sizeof(*request));
}

void wtf_connect_response_cleanup(wtf_connect_response* response)
{
    if (!response) {
        return;
    }

    free(response->headers);
    wtf_header_storage_cleanup(&response->storage);

    memset(response, 0, sizeof(*response));
}

wtf_result_t wtf_qpack_parse_connect_headers(wtf_context* ctx, wtf_http3_stream* stream,
                                             const uint8_t* data, size_t data_len,
                                             wtf_connect_request* request)
{
    if (!ctx || !stream || !data || !request || !stream->connection) {
        return WTF_ERROR_INVALID_PARAMETER;
    }

    wtf_connection* conn = stream->connection;

    WTF_LOG_DEBUG(ctx, "qpack", "Parsing CONNECT headers: %zu bytes", data_len);

    // Initialize request structure
    memset(request, 0, sizeof(*request));
    request->valid = false;

    // Setup decode context
    wtf_header_decode_context decode_ctx = {0};
    decode_ctx.request = request;
    decode_ctx.connection = conn;
    decode_ctx.headers_complete = false;
    decode_ctx.header_count = 0;

    // Lock the QPACK context for thread safety
    mtx_lock(&conn->qpack.mutex);

    if (!conn->qpack.initialized) {
        mtx_unlock(&conn->qpack.mutex);
        WTF_LOG_ERROR(ctx, "qpack", "QPACK not initialized");
        return WTF_ERROR_INVALID_STATE;
    }

    struct lsqpack_dec* decoder = &conn->qpack.decoder;
    const uint8_t* header_data = data;
    uint64_t stream_id = stream->id;

    // Decode headers using QPACK decoder
    enum lsqpack_read_header_status decode_result = lsqpack_dec_header_in(
        decoder, &decode_ctx, stream_id, data_len, &header_data, data_len, NULL, NULL);

    mtx_unlock(&conn->qpack.mutex);

    wtf_result_t result = WTF_SUCCESS;

    // Process decode result
    switch (decode_result) {
        case LQRHS_DONE:
            WTF_LOG_DEBUG(ctx, "qpack", "Headers decoded successfully, %zu headers processed",
                          decode_ctx.header_count);

            if (decode_ctx.malformed_header_block) {
                WTF_LOG_ERROR(ctx, "qpack", "Malformed CONNECT header block");
                result = WTF_ERROR_PROTOCOL_VIOLATION;
            } else if (!validate_required_connect_headers(ctx, request)) {
                result = WTF_ERROR_PROTOCOL_VIOLATION;
            } else {
                request->valid = true;
                WTF_LOG_DEBUG(ctx, "qpack", "CONNECT headers validated successfully");
                WTF_LOG_TRACE(ctx, "qpack",
                              "CONNECT request - method:%s, protocol:%s, scheme:%s, authority:%s",
                              request->method ? request->method : "NULL",
                              request->protocol ? request->protocol : "NULL",
                              request->scheme ? request->scheme : "NULL",
                              request->authority ? request->authority : "NULL");
            }
            break;

        case LQRHS_BLOCKED:
            WTF_LOG_DEBUG(ctx, "qpack", "Header block blocked - waiting for encoder stream data");
            result = WTF_ERROR_PROTOCOL_VIOLATION;
            break;

        case LQRHS_NEED:
            WTF_LOG_ERROR(ctx, "qpack", "Incomplete header block - need more data");
            result = WTF_ERROR_PROTOCOL_VIOLATION;
            break;

        case LQRHS_ERROR:
        default:
            WTF_LOG_ERROR(ctx, "qpack", "Header decoding error: %d", decode_result);
            result = WTF_ERROR_PROTOCOL_VIOLATION;
            break;
    }

    // Cleanup on error
    if (result != WTF_SUCCESS) {
        wtf_connect_request_cleanup(request);
    }

    return result;
}

wtf_result_t wtf_qpack_parse_response_headers(wtf_context* ctx, wtf_http3_stream* stream,
                                              const uint8_t* data, size_t data_len,
                                              wtf_connect_response* response)
{
    if (!ctx || !stream || !data || !response || !stream->connection) {
        return WTF_ERROR_INVALID_PARAMETER;
    }

    wtf_connection* conn = stream->connection;

    WTF_LOG_DEBUG(ctx, "qpack", "Parsing CONNECT response headers: %zu bytes", data_len);
    memset(response, 0, sizeof(*response));
    response->valid = false;

    wtf_header_decode_context decode_ctx = {0};
    decode_ctx.response = response;
    decode_ctx.connection = conn;
    decode_ctx.decode_response = true;

    mtx_lock(&conn->qpack.mutex);

    if (!conn->qpack.initialized) {
        mtx_unlock(&conn->qpack.mutex);
        WTF_LOG_ERROR(ctx, "qpack", "QPACK not initialized");
        return WTF_ERROR_INVALID_STATE;
    }

    struct lsqpack_dec* decoder = &conn->qpack.decoder;
    const uint8_t* header_data = data;
    uint64_t stream_id = stream->id;

    enum lsqpack_read_header_status decode_result = lsqpack_dec_header_in(
        decoder, &decode_ctx, stream_id, data_len, &header_data, data_len, NULL, NULL);

    mtx_unlock(&conn->qpack.mutex);

    wtf_result_t result = WTF_SUCCESS;

    switch (decode_result) {
        case LQRHS_DONE:
            if (decode_ctx.malformed_header_block) {
                WTF_LOG_ERROR(ctx, "qpack", "Malformed CONNECT response header block");
                result = WTF_ERROR_PROTOCOL_VIOLATION;
            } else if (!decode_ctx.seen_status) {
                WTF_LOG_ERROR(ctx, "qpack", "Missing :status response header");
                result = WTF_ERROR_PROTOCOL_VIOLATION;
            } else {
                response->valid = true;
                WTF_LOG_DEBUG(ctx, "qpack", "CONNECT response status: %u",
                              response->status_code);
            }
            break;

        case LQRHS_BLOCKED:
            WTF_LOG_DEBUG(ctx, "qpack", "Response header block blocked");
            result = WTF_ERROR_PROTOCOL_VIOLATION;
            break;

        case LQRHS_NEED:
            WTF_LOG_ERROR(ctx, "qpack", "Incomplete response header block");
            result = WTF_ERROR_PROTOCOL_VIOLATION;
            break;

        case LQRHS_ERROR:
        default: {
            WTF_LOG_ERROR(ctx, "qpack", "Response header decoding error: %d", decode_result);
            const struct lsqpack_dec_err* err = lsqpack_dec_get_err_info(decoder);
            if (err) {
                WTF_LOG_ERROR(ctx, "qpack",
                              "QPACK decoder error detail: type=%d line=%d off=%llu stream=%llu",
                              (int)err->type, err->line, (unsigned long long)err->off,
                              (unsigned long long)err->stream_id);
            }
            result = WTF_ERROR_PROTOCOL_VIOLATION;
            break;
        }
    }

    if (result != WTF_SUCCESS) {
        wtf_connect_response_cleanup(response);
    }

    return result;
}
