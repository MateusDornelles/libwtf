#include <gtest/gtest.h>

#include <cstring>
#include <cstdint>

extern "C" {
#include "utils.h"
}

TEST(Utils, MapsWebTransportApplicationErrorsIntoHttp3Range)
{
    const uint32_t app_codes[] = {0, 1, 29, 30, 31, 0xffffffffU};

    for (uint32_t app_code : app_codes) {
        uint64_t expected = WTF_WEBTRANSPORT_APPLICATION_ERROR_BASE + app_code
            + (app_code / 0x1eU);
        if (expected > WTF_WEBTRANSPORT_APPLICATION_ERROR_MAX) {
            expected = WTF_WEBTRANSPORT_APPLICATION_ERROR_MAX;
        }

        uint64_t mapped = wtf_map_webtransport_error_to_h3(app_code);
        EXPECT_EQ(mapped, expected);
        EXPECT_GE(mapped, WTF_WEBTRANSPORT_APPLICATION_ERROR_BASE);
        EXPECT_LE(mapped, WTF_WEBTRANSPORT_APPLICATION_ERROR_MAX);
    }
}

TEST(Utils, MapsHttp3ApplicationErrorsBackToWebTransport)
{
    EXPECT_EQ(wtf_map_h3_error_to_webtransport(WTF_WEBTRANSPORT_APPLICATION_ERROR_BASE), 0u);
    EXPECT_EQ(wtf_map_h3_error_to_webtransport(WTF_WEBTRANSPORT_APPLICATION_ERROR_BASE + 31), 30u);
    EXPECT_EQ(wtf_map_h3_error_to_webtransport(WTF_WEBTRANSPORT_APPLICATION_ERROR_BASE - 1), 0u);
    EXPECT_EQ(wtf_map_h3_error_to_webtransport(WTF_WEBTRANSPORT_APPLICATION_ERROR_MAX + 1), 0u);
}

TEST(Utils, ParsesThumbprintsWithCommonSeparators)
{
    uint8_t hash[20] = {};

    EXPECT_TRUE(wtf_parse_thumbprint("00112233445566778899aabbccddeeff00112233", hash));
    EXPECT_EQ(hash[0], 0x00);
    EXPECT_EQ(hash[19], 0x33);

    EXPECT_TRUE(wtf_parse_thumbprint("00:11:22:33:44:55:66:77:88:99:aa:bb:cc:dd:ee:ff:00:11:22:33",
                                     hash));
    EXPECT_TRUE(wtf_parse_thumbprint("00-11-22-33-44-55-66-77-88-99-aa-bb-cc-dd-ee-ff-00-11-22-33",
                                     hash));
    EXPECT_FALSE(wtf_parse_thumbprint("not-a-thumbprint", hash));
    EXPECT_FALSE(wtf_parse_thumbprint("00112233", hash));
}

TEST(Utils, ProvidesStableResultStrings)
{
    EXPECT_STREQ(wtf_result_to_string(WTF_SUCCESS), "Success");
    EXPECT_STREQ(wtf_result_to_string(WTF_ERROR_INVALID_PARAMETER), "Invalid parameter");
    EXPECT_STREQ(wtf_result_to_string(static_cast<wtf_result_t>(9999)), "Unknown error");
}

TEST(Utils, InternalSendContextKeepsSmallCopiesInline)
{
    uint8_t payload[WTF_INLINE_SEND_STORAGE];
    std::memset(payload, 0x5a, sizeof(payload));

    wtf_internal_send_context* context = nullptr;
    ASSERT_EQ(WTF_SUCCESS,
              wtf_internal_send_context_create_copy(payload, sizeof(payload), &context));
    ASSERT_NE(context, nullptr);
    ASSERT_EQ(context->count, 1u);
    EXPECT_EQ(context->buffers, context->inline_buffers);
    EXPECT_EQ(context->buffers[0].data, context->inline_data);
    EXPECT_EQ(context->owned_buffer_data, nullptr);
    EXPECT_EQ(std::memcmp(context->buffers[0].data, payload, sizeof(payload)), 0);

    wtf_internal_send_context_destroy(context);
}

TEST(Utils, InternalSendContextUsesOneOwnedAllocationForLargeCopies)
{
    uint8_t payload[WTF_INLINE_SEND_STORAGE + 1];
    std::memset(payload, 0xa5, sizeof(payload));

    wtf_internal_send_context* context = nullptr;
    ASSERT_EQ(WTF_SUCCESS,
              wtf_internal_send_context_create_copy(payload, sizeof(payload), &context));
    ASSERT_NE(context, nullptr);
    ASSERT_NE(context->owned_buffer_data, nullptr);
    EXPECT_EQ(context->buffers[0].data, context->owned_buffer_data);
    EXPECT_NE(context->buffers[0].data, context->inline_data);
    EXPECT_EQ(std::memcmp(context->buffers[0].data, payload, sizeof(payload)), 0);

    wtf_internal_send_context_destroy(context);
}
