#pragma once

#include "lvgl.h"

typedef enum {
    PACKING_VISUAL_IDLE,
    PACKING_VISUAL_PLANNED,
    PACKING_VISUAL_RUNNING,
    PACKING_VISUAL_SCAN,
    PACKING_VISUAL_TRANSFER,
    PACKING_VISUAL_ACCEPTED,
    PACKING_VISUAL_REJECTED,
    PACKING_VISUAL_DUPLICATE,
} packing_visual_mode_t;

typedef struct packing_visual packing_visual_t;

packing_visual_t *packing_visual_create(lv_obj_t *parent);
lv_obj_t *packing_visual_object(packing_visual_t *visual);
void packing_visual_set_mode(packing_visual_t *visual, packing_visual_mode_t mode);
