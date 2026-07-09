#ifndef BLE_LINK_DIAG_ROBOT_INCLUDE_H
#define BLE_LINK_DIAG_ROBOT_INCLUDE_H

/*
 * Out-of-tree diagnostic stub for ble_link.c.
 * The real 005 project uses robot_include.h for RB_* logging, but pulling the
 * full project header into this small diagnostic binary would also pull many
 * unrelated robot modules. Keep only the pieces required by ble_link.c.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define RB_INFO(format, ...)  fprintf(stdout, "[INFO] " format "\n", ##__VA_ARGS__)
#define RB_DEBUG(format, ...) fprintf(stdout, "[DEBUG] " format "\n", ##__VA_ARGS__)
#define RB_ERROR(format, ...) fprintf(stderr, "[ERROR] " format "\n", ##__VA_ARGS__)

#endif
