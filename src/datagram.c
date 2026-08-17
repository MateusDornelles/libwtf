#include "datagram.h"

#include "conn.h"
#include "log.h"
#include "session.h"
#include "varint.h"

static void wtf_datagram_shutdown_connection(wtf_connection* conn, uint64_t error_code)
{
    if (!conn || !conn->context || !conn->context->quic_api
        || !conn->quic_connection) {
        return;
    }

    conn->context->quic_api->ConnectionShutdown(
        conn->quic_connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, error_code);
}

static bool wtf_validate_stream_id(uint64_t stream_id, bool is_client_initiated)
{
    bool client_initiated = WTF_STREAM_IS_CLIENT_INITIATED(stream_id);
    return stream_id <= WTF_VARINT_MAX && !WTF_STREAM_IS_UNIDIRECTIONAL(stream_id)
        && client_initiated == is_client_initiated;
}

void wtf_datagram_process(wtf_connection* conn, const uint8_t* data, size_t data_len,
                          bool is_client_initiated)
{
    if (!conn)
        return;

    if (!data || data_len == 0) {
        WTF_LOG_WARN(conn->context, "datagram",
                     "HTTP/3 datagram too short to contain Quarter Stream ID");
        wtf_datagram_shutdown_connection(conn, WTF_H3_DATAGRAM_ERROR);
        return;
    }

    size_t offset = 0;
    uint64_t quarter_stream_id;

    if (!wtf_varint_decode(data_len, data, &offset, &quarter_stream_id)) {
        WTF_LOG_WARN(conn->context, "datagram", "Failed to decode Quarter Stream ID");
        wtf_datagram_shutdown_connection(conn, WTF_H3_DATAGRAM_ERROR);
        return;
    }

    if (quarter_stream_id > WTF_VARINT_MAX / 4) {
        WTF_LOG_WARN(conn->context, "datagram",
                     "Quarter Stream ID too large: %llu",
                     (unsigned long long)quarter_stream_id);
        wtf_datagram_shutdown_connection(conn, WTF_H3_DATAGRAM_ERROR);
        return;
    }

    uint64_t stream_id = quarter_stream_id * 4;

    if (!wtf_validate_stream_id(stream_id, is_client_initiated)) {
        WTF_LOG_WARN(conn->context, "datagram",
                     "Invalid stream ID %llu from Quarter Stream ID %llu",
                     (unsigned long long)stream_id, (unsigned long long)quarter_stream_id);
        wtf_datagram_shutdown_connection(conn, WTF_H3_DATAGRAM_ERROR);
        return;
    }
    wtf_session* session = wtf_connection_find_session_borrowed(conn, stream_id);
    if (!session) {
        WTF_LOG_TRACE(conn->context, "datagram", "Dropping datagram for unknown session %llu",
                      (unsigned long long)stream_id);
        return;
    }

    const uint8_t* payload = data + offset;
    size_t payload_len = data_len - offset;
    wtf_session_process_datagram(session, payload, payload_len);
}
