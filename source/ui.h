#pragma once
#include <stdbool.h>

#define UI_ROWS 6

typedef enum {
    UI_STATUS_UNKNOWN,   /* no status file */
    UI_STATUS_OK,        /* sending to the companion */
    UI_STATUS_BUSY,      /* starting, waiting for network or searching */
    UI_STATUS_PAUSED,
    UI_STATUS_ERROR,
} UiStatusKind;

typedef struct {
    const char *host;
    unsigned port;
    bool enabled, autostart, dirty, help;
    int module_running;            /* 1 running, 0 stopped, -1 unknown */
    const char *ip, *notice;
    UiStatusKind status_kind;
    const char *status_title;      /* short headline, e.g. "Conectado ao PC" */
    const char *status_detail;     /* raw sysmodule message */
    const char *game;              /* current game or "" */
    const char *companion;         /* companion address in use or "" */
    int selected;
} UiState;

bool ui_init(void);
void ui_draw(const UiState *state);
void ui_exit(void);
/* Returns the row under a touch point, or -1. */
int ui_row_at(int x, int y);
