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

int main(int argc, char **argv)
{
    uint8_t tx[DIAG_MAX_PAYLOAD_LEN];
    size_t tx_len = 0U;
    int timeout_ms = 3000;

    if (argc < 5) {
        diag_usage(argv[0]);
        return 2;
    }

    if (argc >= 6 && strcmp(argv[5], "--packet") == 0) {
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
