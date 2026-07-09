#include "ble_link.h"

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
    printf("\n");
    printf("Example:\n");
    printf("  %s 50:84:92:3F:F2:2C 0000xxxx-0000-1000-8000-00805f9b34fb 0000yyyy-0000-1000-8000-00805f9b34fb 0000zzzz-0000-1000-8000-00805f9b34fb \"A5 04 00 43 00 80 10 10 96\" 3000\n", prog);
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

int main(int argc, char **argv)
{
    ble_link_context_t ctx;
    uint8_t tx[DIAG_MAX_PAYLOAD_LEN];
    uint8_t rx[DIAG_MAX_PAYLOAD_LEN];
    size_t tx_len = 0U;
    int timeout_ms = 3000;
    int ret;

    if (argc < 5) {
        diag_usage(argv[0]);
        return 2;
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

    ble_link_context_init(&ctx);
    ctx.config.scan_timeout_ms = 10000;
    ctx.config.recv_timeout_ms = timeout_ms;
    ctx.config.verbose = 1;

    if (diag_copy_string(ctx.target.mac, sizeof(ctx.target.mac), argv[1], "mac") != 0 ||
        diag_copy_string(ctx.target.service_uuid, sizeof(ctx.target.service_uuid), argv[2], "service_uuid") != 0 ||
        diag_copy_string(ctx.target.write_char_uuid, sizeof(ctx.target.write_char_uuid), argv[3], "write_char_uuid") != 0 ||
        diag_copy_string(ctx.target.notify_char_uuid, sizeof(ctx.target.notify_char_uuid), argv[4], "notify_char_uuid") != 0) {
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
            }
        }
    } else {
        printf("no payload supplied; open/close only\n");
    }

    ble_link_close(&ctx);
    return 0;
}
