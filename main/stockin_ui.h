#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    const char *qr;
    const char *order_ref;
    const char *product_name;
    const char *grade;
    const char *trace_code;
    const char *declared_net_kg;
    const char *label_state;
    const char *warehouse;
    int actual_blocks;
    int blocks_per_carton;
} stockin_carton_view_t;

#define STOCKIN_UI_MAX_GRADES 7

typedef enum {
    STOCKIN_PACKING_NONE = 0,
    STOCKIN_PACKING_PLANNED,
    STOCKIN_PACKING_ON,
} stockin_packing_state_t;

typedef struct {
    char product[96];
    char trace[48];
    char grade[40];
    char state[32];
    int expected_blocks;
    int labels_prepared;
    int expected_labels;
} stockin_grade_view_t;

typedef struct {
    bool active;
    char sheet_ref[96];
    char state[32];
    char started_at[40];
    uint32_t elapsed_seconds;
    int labels_prepared;
    size_t grade_total;
    size_t grade_count;
    stockin_grade_view_t grades[STOCKIN_UI_MAX_GRADES];
} stockin_packing_session_view_t;

esp_err_t stockin_ui_start(void);
void stockin_ui_set_wifi(bool connected, const char *detail);
bool stockin_ui_set_network(bool wifi, bool ethernet, const char *line1, const char *line2);
void stockin_ui_set_bluetooth(bool ready);
void stockin_ui_set_usb(bool connected, const char *detail);
void stockin_ui_show_ready(void);
void stockin_ui_set_packing_session(stockin_packing_state_t state,
                                    const stockin_packing_session_view_t *session);
void stockin_ui_show_queued(uint32_t scan_id, const char *qr, unsigned pending);
void stockin_ui_show_processing(uint32_t scan_id, const char *qr);
void stockin_ui_show_carton(uint32_t scan_id, const stockin_carton_view_t *carton,
                            const char *status, uint32_t accent);
void stockin_ui_show_success(uint32_t scan_id, const stockin_carton_view_t *carton,
                             const char *message);
void stockin_ui_show_not_in_labels(uint32_t scan_id);
void stockin_ui_show_duplicate(const char *qr);
void stockin_ui_show_error(uint32_t scan_id, const char *qr,
                           const char *title, const char *message);
void stockin_ui_set_activity(unsigned packed, unsigned rejected,
                             unsigned queued, unsigned available);
