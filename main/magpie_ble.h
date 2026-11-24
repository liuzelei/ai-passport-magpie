#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "magpie_protocol.h"

typedef struct {
    magpie_link_t link;
    bool ready;
    bool pairing;
    bool has_bond;
    bool showing_passkey;
    uint32_t passkey;
} magpie_ble_view_t;

/* Pass a one-slot queue for magpie_ble_view_t snapshots owned by the caller. */
esp_err_t magpie_ble_init(QueueHandle_t view_updates);
void magpie_ble_request_pairing(void);
