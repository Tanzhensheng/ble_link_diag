#include "ble_protocol.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DIAG_MAX_PAYLOAD_LEN 1024U

static void diag_usage(const char *prog)
{
    printf("Usage:\n");
    printf("  %s <mac> <service_uuid> <write_char_uuid> <notify_char_uuid> [hex_payload] [timeout_ms]\n", prog);
    printf("  %s <mac> <service_uuid> <write_char_uuid> <notify_char_uuid> --packet <payload_text> [timeout_ms]\n", prog);
    printf("  %s <mac> <service_uuid> <write_char_uuid> <notify_char_uuid> --packet-trace <payload_text> [timeout_ms]\n", prog);
    printf("\n");
    printf("Example:\n");
    printf("  %s 50:84:92:3F:F2:2C 0000xxxx-0000-1000-8000-00805f9b34fb 0000yyyy-0000-1000-8000-00805f9b34fb 0000zzzz-0000-1000-8000-00805f9b34fb \"A5 06 00 A5 43 00 80 10 01 02 03 D9 96\" 3000\n", prog);
    printf("  %s 50:84:92:3F:F2:2C 0000xxxx-0000-1000-8000-00805f9b34fb 0000yyyy-0000-1000-8000-00805f9b34fb 0000zzzz-0000-1000-8000-00805f9b34fb --packet '{\"topic\":\"/sdttu/subdevice/realtime/get\",\"body\":{\"messageId\":\"diag-1\",\"timestamp\":\"2026-07-09 00:00:00\",\"body\":[]}}' 8000\n", prog);
}

static int diag_copy_string(char *dst, size_t dst_size, const char *src, const char *name)
{
    int len;

    if (dst == NULL || src == NULL || name == NULL || dst_size == 0U) {
        return -1;
    }

    len = snprintf(dst, dst_size, "%s", src);
    if (len < 0 || (size_t)len >= dst_size) {
        fprintf(stderr, "%s is too long\n", name);
        return -1;
    }

    return 0;
}

static int diag_hex_value(int ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

static int diag_parse_hex(const char *text, uint8_t *out, size_t out_size, size_t *out_len)
{
    int high = -1;
    size_t len = 0U;

    if (text == NULL || out == NULL || out_len == NULL) {
        return -1;
    }

    while (*text != '\0') {
        int value;

        if (isspace((unsigned char)*text) || *text == ':' || *text == '-' || *text == ',') {
            ++text;
            continue;
        }

        value = diag_hex_value((unsigned char)*text);
        if (value < 0) {
            fprintf(stderr, "invalid hex character: %c\n", *text);
            return -1;
        }

        if (high < 0) {
            high = value;
        } else {
            if (len >= out_size) {
                fprintf(stderr, "hex payload is too long\n");
                return -1;
            }
            out[len++] = (uint8_t)((high << 4) | value);
            high = -1;
        }

        ++text;
    }

    if (high >= 0) {
        fprintf(stderr, "hex payload has odd digit count\n");
        return -1;
    }

    *out_len = len;
    return 0;
}

static void diag_dump_hex(const char *title, const uint8_t *data, size_t len)
{
    size_t i;

    printf("%s len=%zu:", title, len);
    for (i = 0U; i < len; ++i) {
        printf(" %02X", data[i]);
    }
    printf("\n");
}

static void diag_dump_text(const char *title, const uint8_t *data, size_t len)
{
    size_t i;

    if (title == NULL || data == NULL) {
        return;
    }

    printf("%s text=\"", title);
    for (i = 0U; i < len; ++i) {
        if (data[i] >= 0x20U && data[i] <= 0x7EU) {
            putchar((int)data[i]);
        } else {
            printf("\\x%02X", data[i]);
        }
    }
    printf("\"\n");
}

typedef struct {
    uint8_t prot;
    uint8_t total_frames;
    uint8_t received[BLE_PROTOCOL_MAX_FRAME_COUNT];
    size_t frame_len[BLE_PROTOCOL_MAX_FRAME_COUNT];
    uint8_t frame_buf[BLE_PROTOCOL_MAX_FRAME_COUNT][BLE_PROTOCOL_MAX_DATA_LEN];
} diag_packet_rx_t;

static uint8_t diag_frame_dir(const ble_protocol_frame_t *frame)
{
    return (uint8_t)((frame->control >> 7U) & 0x01U);
}

static uint8_t diag_frame_prm(const ble_protocol_frame_t *frame)
{
    return (uint8_t)((frame->control >> 6U) & 0x01U);
}

static uint8_t diag_frame_fun(const ble_protocol_frame_t *frame)
{
    return (uint8_t)(frame->control & 0x0FU);
}

static uint8_t diag_frame_index(const ble_protocol_frame_t *frame)
{
    if ((frame->fseq & 0x80U) != 0U) {
        return 0U;
    }
    return (uint8_t)(frame->fseq & 0x7FU);
}

static uint8_t diag_frame_total(const ble_protocol_frame_t *frame)
{
    if ((frame->fseq & 0x80U) == 0U) {
        return 0U;
    }
    return (uint8_t)((frame->fseq & 0x7FU) + 1U);
}

static int diag_frame_is_ack(const ble_protocol_frame_t *frame)
{
    return frame != NULL &&
        diag_frame_prm(frame) == BLE_PROTOCOL_PRM_RESPONDER &&
        diag_frame_fun(frame) == BLE_PROTOCOL_RSP_ACK;
}

static int diag_frame_is_nak(const ble_protocol_frame_t *frame)
{
    return frame != NULL &&
        diag_frame_prm(frame) == BLE_PROTOCOL_PRM_RESPONDER &&
        diag_frame_fun(frame) == BLE_PROTOCOL_RSP_NAK;
}

static int diag_frame_is_user_data(const ble_protocol_frame_t *frame)
{
    return frame != NULL &&
        diag_frame_prm(frame) == BLE_PROTOCOL_PRM_STARTER &&
        diag_frame_fun(frame) == BLE_PROTOCOL_FUN_USER_DATA;
}

static void diag_dump_frame(const char *title, const ble_protocol_frame_t *frame)
{
    if (title == NULL || frame == NULL) {
        return;
    }

    printf(
        "%s C=0x%02X dir=%u prm=%u fun=%u pseq=%u fseq=0x%02X idx=%u total=%u prot=0x%02X data_len=%zu\n",
        title,
        frame->control,
        diag_frame_dir(frame),
        diag_frame_prm(frame),
        diag_frame_fun(frame),
        frame->pseq,
        frame->fseq,
        diag_frame_index(frame),
        diag_frame_total(frame),
        frame->prot,
        frame->data_len);
    if (frame->data_len > 0U) {
        diag_dump_text("  data", frame->data, frame->data_len);
    }
}

static void diag_packet_rx_init(diag_packet_rx_t *rx)
{
    if (rx != NULL) {
        memset(rx, 0, sizeof(*rx));
    }
}

static int diag_packet_rx_store(diag_packet_rx_t *rx, const ble_protocol_frame_t *frame)
{
    uint8_t index;
    uint8_t total;

    if (rx == NULL || frame == NULL || frame->data_len > BLE_PROTOCOL_MAX_DATA_LEN) {
        return -1;
    }

    index = diag_frame_index(frame);
    total = diag_frame_total(frame);
    if (index == 0U) {
        if (total == 0U || total > BLE_PROTOCOL_MAX_FRAME_COUNT) {
            return -1;
        }
        diag_packet_rx_init(rx);
        rx->total_frames = total;
        rx->prot = frame->prot;
    } else if (rx->total_frames == 0U || index >= rx->total_frames) {
        return -1;
    }

    memcpy(rx->frame_buf[index], frame->data, frame->data_len);
    rx->frame_len[index] = frame->data_len;
    rx->received[index] = 1U;
    return 0;
}

static int diag_packet_rx_complete(const diag_packet_rx_t *rx)
{
    uint8_t i;

    if (rx == NULL || rx->total_frames == 0U) {
        return 0;
    }

    for (i = 0U; i < rx->total_frames; ++i) {
        if (rx->received[i] == 0U) {
            return 0;
        }
    }
    return 1;
}

static int diag_packet_rx_copy(const diag_packet_rx_t *rx, uint8_t *out, size_t out_size, size_t *out_len)
{
    uint8_t i;
    size_t offset = 0U;

    if (rx == NULL || out == NULL || out_len == NULL) {
        return -1;
    }

    for (i = 0U; i < rx->total_frames; ++i) {
        if (offset + rx->frame_len[i] > out_size) {
            return -1;
        }
        if (rx->frame_len[i] > 0U) {
            memcpy(&out[offset], rx->frame_buf[i], rx->frame_len[i]);
        }
        offset += rx->frame_len[i];
    }

    *out_len = offset;
    return 0;
}

static int diag_fill_link_target(ble_link_context_t *ctx, char **argv)
{
    if (ctx == NULL || argv == NULL) {
        return -1;
    }

    return diag_copy_string(ctx->target.mac, sizeof(ctx->target.mac), argv[1], "mac") != 0 ||
        diag_copy_string(ctx->target.service_uuid, sizeof(ctx->target.service_uuid), argv[2], "service_uuid") != 0 ||
        diag_copy_string(ctx->target.write_char_uuid, sizeof(ctx->target.write_char_uuid), argv[3], "write_char_uuid") != 0 ||
        diag_copy_string(ctx->target.notify_char_uuid, sizeof(ctx->target.notify_char_uuid), argv[4], "notify_char_uuid") != 0 ? -1 : 0;
}

static int diag_run_raw(char **argv, const uint8_t *tx, size_t tx_len, int timeout_ms)
{
    ble_link_context_t ctx;
    uint8_t rx[DIAG_MAX_PAYLOAD_LEN];
    int ret;

    ble_link_context_init(&ctx);
    ctx.config.scan_timeout_ms = 10000;
    ctx.config.recv_timeout_ms = timeout_ms;
    ctx.config.verbose = 1;

    if (diag_fill_link_target(&ctx, argv) != 0) {
        return 2;
    }

    printf("opening BLE link...\n");
    ret = ble_link_open(&ctx);
    printf("ble_link_open ret=%d state=%d connected=%d\n", ret, ctx.state, ctx.is_connected);
    if (ret != BLE_LINK_STATUS_OK) {
        ble_link_close(&ctx);
        return 1;
    }

    if (tx_len > 0U) {
        diag_dump_hex("tx", tx, tx_len);
        ret = ble_link_send(&ctx, tx, tx_len);
        printf("ble_link_send ret=%d\n", ret);
        if (ret == BLE_LINK_STATUS_OK) {
            ret = ble_link_receive(&ctx, rx, sizeof(rx), timeout_ms);
            printf("ble_link_receive ret=%d\n", ret);
            if (ret > 0) {
                diag_dump_hex("rx", rx, (size_t)ret);
                diag_dump_text("rx", rx, (size_t)ret);
            }
        }
    } else {
        printf("no payload supplied; open/close only\n");
    }

    ble_link_close(&ctx);
    return 0;
}

static int diag_run_packet(char **argv, const char *payload_text, int timeout_ms)
{
    ble_protocol_context_t ctx;
    ble_link_context_t *link;
    uint8_t rx[DIAG_MAX_PAYLOAD_LEN];
    size_t payload_len;
    size_t rx_len = 0U;
    uint8_t rx_prot = 0U;
    int ret;

    if (payload_text == NULL) {
        return 2;
    }

    ble_protocol_context_init(&ctx);
    link = ble_protocol_get_link(&ctx);
    if (link == NULL) {
        return 1;
    }

    link->config.scan_timeout_ms = 10000;
    link->config.recv_timeout_ms = timeout_ms;
    link->config.verbose = 1;
    ctx.config.frame_ack_timeout_ms = timeout_ms;

    if (diag_fill_link_target(link, argv) != 0) {
        return 2;
    }

    payload_len = strlen(payload_text);

    printf("opening BLE protocol...\n");
    ret = ble_protocol_open(&ctx);
    printf("ble_protocol_open ret=%d state=%d connected=%d\n", ret, link->state, link->is_connected);
    if (ret != BLE_PROTOCOL_STATUS_OK) {
        ble_protocol_close(&ctx);
        return 1;
    }

    printf("packet prot=0x%02X payload_len=%zu\n", BLE_PROTOCOL_PROT_NEAR_FIELD, payload_len);
    diag_dump_text("tx_payload", (const uint8_t *)payload_text, payload_len);
    ret = ble_protocol_send_packet(&ctx, BLE_PROTOCOL_PROT_NEAR_FIELD, (const uint8_t *)payload_text, payload_len);
    printf("ble_protocol_send_packet ret=%d\n", ret);
    if (ret == BLE_PROTOCOL_STATUS_OK) {
        ret = ble_protocol_receive_packet(&ctx, rx, sizeof(rx), &rx_len, &rx_prot, timeout_ms);
        printf("ble_protocol_receive_packet ret=%d prot=0x%02X len=%zu\n", ret, rx_prot, rx_len);
        if (ret == BLE_PROTOCOL_STATUS_OK) {
            diag_dump_hex("rx_payload", rx, rx_len);
            diag_dump_text("rx_payload", rx, rx_len);
        }
    }

    ble_protocol_close(&ctx);
    return ret == BLE_PROTOCOL_STATUS_OK ? 0 : 1;
}

static int diag_trace_handle_user_data(
    ble_protocol_context_t *ctx,
    const ble_protocol_frame_t *frame,
    diag_packet_rx_t *rx)
{
    ble_protocol_frame_t ack;
    int ret;

    if (ctx == NULL || frame == NULL || rx == NULL) {
        return BLE_PROTOCOL_STATUS_INVALID_ARG;
    }

    ret = diag_packet_rx_store(rx, frame);
    if (ret != 0) {
        ble_protocol_frame_t nak;

        if (ble_protocol_build_nak(frame, &nak) == BLE_PROTOCOL_STATUS_OK) {
            (void)ble_protocol_send_frame(ctx, &nak);
            diag_dump_frame("tx_nak", &nak);
        }
        return BLE_PROTOCOL_STATUS_FORMAT;
    }

    ret = ble_protocol_build_ack(frame, &ack);
    if (ret != BLE_PROTOCOL_STATUS_OK) {
        return ret;
    }

    diag_dump_frame("tx_ack", &ack);
    return ble_protocol_send_frame(ctx, &ack);
}

static int diag_trace_wait_ack(
    ble_protocol_context_t *ctx,
    const ble_protocol_frame_t *request,
    diag_packet_rx_t *rx,
    int timeout_ms)
{
    int ret;

    if (ctx == NULL || request == NULL || rx == NULL) {
        return BLE_PROTOCOL_STATUS_INVALID_ARG;
    }

    while (1) {
        ble_protocol_frame_t frame;

        ret = ble_protocol_receive_frame(ctx, &frame, timeout_ms);
        printf("ble_protocol_receive_frame ret=%d while waiting ack\n", ret);
        if (ret != BLE_PROTOCOL_STATUS_OK) {
            return ret;
        }

        diag_dump_frame("rx_frame", &frame);
        if (frame.pseq == request->pseq && frame.fseq == request->fseq && diag_frame_is_ack(&frame)) {
            return BLE_PROTOCOL_STATUS_OK;
        }
        if (frame.pseq == request->pseq && frame.fseq == request->fseq && diag_frame_is_nak(&frame)) {
            return BLE_PROTOCOL_STATUS_NAK;
        }
        if (diag_frame_is_user_data(&frame)) {
            ret = diag_trace_handle_user_data(ctx, &frame, rx);
            if (ret != BLE_PROTOCOL_STATUS_OK) {
                return ret;
            }
        }
    }
}

static int diag_trace_send_packet(
    ble_protocol_context_t *ctx,
    const uint8_t *payload,
    size_t payload_len,
    diag_packet_rx_t *rx,
    int timeout_ms)
{
    size_t total_frames;
    size_t frame_index;
    uint8_t pseq_base;

    if (ctx == NULL || (payload_len > 0U && payload == NULL) || rx == NULL) {
        return BLE_PROTOCOL_STATUS_INVALID_ARG;
    }

    total_frames = payload_len == 0U ? 1U :
        ((payload_len + BLE_PROTOCOL_MAX_DATA_LEN - 1U) / BLE_PROTOCOL_MAX_DATA_LEN);
    if (total_frames == 0U || total_frames > BLE_PROTOCOL_MAX_FRAME_COUNT) {
        return BLE_PROTOCOL_STATUS_PACKET_TOO_LARGE;
    }

    pseq_base = ctx->next_pseq;
    for (frame_index = 0U; frame_index < total_frames; ++frame_index) {
        ble_protocol_frame_t frame;
        size_t offset = frame_index * BLE_PROTOCOL_MAX_DATA_LEN;
        size_t remaining = payload_len > offset ? (payload_len - offset) : 0U;
        size_t chunk_len = remaining > BLE_PROTOCOL_MAX_DATA_LEN ? BLE_PROTOCOL_MAX_DATA_LEN : remaining;
        int ret;

        memset(&frame, 0, sizeof(frame));
        frame.control = ble_protocol_build_control(
            BLE_PROTOCOL_DIR_DOWNLINK,
            BLE_PROTOCOL_PRM_STARTER,
            ctx->next_fcb,
            0U,
            BLE_PROTOCOL_FUN_USER_DATA);
        frame.pseq = ctx->config.pseq_mode == BLE_PROTOCOL_PSEQ_PER_FRAME ?
            (uint8_t)(pseq_base + (uint8_t)frame_index) : pseq_base;
        frame.fseq = frame_index == 0U ?
            ble_protocol_build_fseq(1U, (uint8_t)(total_frames - 1U)) :
            ble_protocol_build_fseq(0U, (uint8_t)frame_index);
        frame.prot = BLE_PROTOCOL_PROT_NEAR_FIELD;
        frame.data_len = chunk_len;
        if (chunk_len > 0U) {
            memcpy(frame.data, &payload[offset], chunk_len);
        }

        diag_dump_frame("tx_frame", &frame);
        ret = ble_protocol_send_frame(ctx, &frame);
        printf("ble_protocol_send_frame ret=%d\n", ret);
        if (ret != BLE_PROTOCOL_STATUS_OK) {
            return ret;
        }

        ret = diag_trace_wait_ack(ctx, &frame, rx, timeout_ms);
        printf("wait_ack ret=%d\n", ret);
        if (ret != BLE_PROTOCOL_STATUS_OK) {
            return ret;
        }
    }

    ctx->next_pseq = ctx->config.pseq_mode == BLE_PROTOCOL_PSEQ_PER_FRAME ?
        (uint8_t)(pseq_base + (uint8_t)total_frames) : (uint8_t)(pseq_base + 1U);
    ctx->next_fcb = (uint8_t)(ctx->next_fcb ^ 0x01U);
    return BLE_PROTOCOL_STATUS_OK;
}

static int diag_trace_receive_reply(ble_protocol_context_t *ctx, diag_packet_rx_t *rx, int timeout_ms)
{
    int ret;

    if (ctx == NULL || rx == NULL) {
        return BLE_PROTOCOL_STATUS_INVALID_ARG;
    }

    while (!diag_packet_rx_complete(rx)) {
        ble_protocol_frame_t frame;

        ret = ble_protocol_receive_frame(ctx, &frame, timeout_ms);
        printf("ble_protocol_receive_frame ret=%d while waiting packet\n", ret);
        if (ret != BLE_PROTOCOL_STATUS_OK) {
            return ret;
        }

        diag_dump_frame("rx_frame", &frame);
        if (diag_frame_is_user_data(&frame)) {
            ret = diag_trace_handle_user_data(ctx, &frame, rx);
            if (ret != BLE_PROTOCOL_STATUS_OK) {
                return ret;
            }
        }
    }

    return BLE_PROTOCOL_STATUS_OK;
}

static int diag_run_packet_trace(char **argv, const char *payload_text, int timeout_ms)
{
    ble_protocol_context_t ctx;
    ble_link_context_t *link;
    diag_packet_rx_t rx;
    uint8_t rx_payload[DIAG_MAX_PAYLOAD_LEN];
    size_t payload_len;
    size_t rx_payload_len = 0U;
    int ret;

    if (payload_text == NULL) {
        return 2;
    }

    ble_protocol_context_init(&ctx);
    link = ble_protocol_get_link(&ctx);
    if (link == NULL) {
        return 1;
    }

    link->config.scan_timeout_ms = 10000;
    link->config.recv_timeout_ms = timeout_ms;
    link->config.verbose = 1;
    ctx.config.frame_ack_timeout_ms = timeout_ms;
    diag_packet_rx_init(&rx);

    if (diag_fill_link_target(link, argv) != 0) {
        return 2;
    }

    payload_len = strlen(payload_text);

    printf("opening BLE protocol trace...\n");
    ret = ble_protocol_open(&ctx);
    printf("ble_protocol_open ret=%d state=%d connected=%d\n", ret, link->state, link->is_connected);
    if (ret != BLE_PROTOCOL_STATUS_OK) {
        ble_protocol_close(&ctx);
        return 1;
    }

    diag_dump_text("tx_payload", (const uint8_t *)payload_text, payload_len);
    ret = diag_trace_send_packet(&ctx, (const uint8_t *)payload_text, payload_len, &rx, timeout_ms);
    printf("trace_send_packet ret=%d\n", ret);
    if (ret == BLE_PROTOCOL_STATUS_OK && !diag_packet_rx_complete(&rx)) {
        ret = diag_trace_receive_reply(&ctx, &rx, timeout_ms);
        printf("trace_receive_reply ret=%d\n", ret);
    }

    if (ret == BLE_PROTOCOL_STATUS_OK && diag_packet_rx_copy(&rx, rx_payload, sizeof(rx_payload), &rx_payload_len) == 0) {
        printf("rx_payload prot=0x%02X len=%zu\n", rx.prot, rx_payload_len);
        diag_dump_hex("rx_payload", rx_payload, rx_payload_len);
        diag_dump_text("rx_payload", rx_payload, rx_payload_len);
    }

    ble_protocol_close(&ctx);
    return ret == BLE_PROTOCOL_STATUS_OK ? 0 : 1;
}

int main(int argc, char **argv)
{
    uint8_t tx[DIAG_MAX_PAYLOAD_LEN];
    size_t tx_len = 0U;
    int timeout_ms = 3000;

    if (argc < 5) {
        diag_usage(argv[0]);
        return 2;
    }

    if (argc >= 6 && (strcmp(argv[5], "--packet") == 0 || strcmp(argv[5], "--packet-trace") == 0)) {
        if (argc < 7) {
            diag_usage(argv[0]);
            return 2;
        }

        if (argc >= 8) {
            timeout_ms = atoi(argv[7]);
            if (timeout_ms < 0) {
                fprintf(stderr, "timeout_ms must be >= 0\n");
                return 2;
            }
        }

        if (strcmp(argv[5], "--packet-trace") == 0) {
            return diag_run_packet_trace(argv, argv[6], timeout_ms);
        }
        return diag_run_packet(argv, argv[6], timeout_ms);
    }

    if (argc >= 6 && diag_parse_hex(argv[5], tx, sizeof(tx), &tx_len) != 0) {
        return 2;
    }

    if (argc >= 7) {
        timeout_ms = atoi(argv[6]);
        if (timeout_ms < 0) {
            fprintf(stderr, "timeout_ms must be >= 0\n");
            return 2;
        }
    }

    return diag_run_raw(argv, tx, tx_len, timeout_ms);
}
