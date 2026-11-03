#pragma once
#include <stdbool.h>

typedef struct {
    const char *host;
    unsigned port;
    bool enabled, autostart, dirty;
    const char *ip, *module_status, *notice;
    int selected;
} UiState;

bool ui_init(void);
void ui_draw(const UiState *state);
void ui_exit(void);
