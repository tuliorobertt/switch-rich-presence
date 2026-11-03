#include <switch.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui.h"
#include "icon_pixels.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "vendor/stb_truetype.h"

#define WIDTH 1280
#define HEIGHT 720
#define C(r,g,b) ((u32)RGBA8_MAXALPHA((u32)(r),(u32)(g),(u32)(b)))

static Framebuffer fb;
static stbtt_fontinfo font;
static PlFontData shared_font;
static u32 *pixels, stride;
static bool have_fb, have_pl;

static const u32 bg = C(5, 10, 28), panel = C(14, 22, 46), field = C(24, 35, 66);
static const u32 white = C(244, 247, 255), muted = C(159, 174, 203), blue = C(86, 118, 255);
static const u32 red = C(252, 44, 63), green = C(41, 219, 133), border = C(48, 64, 106);

static void rect(int x, int y, int w, int h, u32 color) {
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > WIDTH ? WIDTH : x + w, y1 = y + h > HEIGHT ? HEIGHT : y + h;
    for (int py = y0; py < y1; ++py) {
        u32 *row = pixels + py * stride;
        for (int px = x0; px < x1; ++px) row[px] = color;
    }
}

static void round_rect(int x, int y, int w, int h, int radius, u32 color) {
    rect(x + radius, y, w - 2 * radius, h, color);
    rect(x, y + radius, radius, h - 2 * radius, color);
    rect(x + w - radius, y + radius, radius, h - 2 * radius, color);
    for (int dy = 0; dy < radius; ++dy) {
        int dx = 0;
        while (dx < radius && (radius - dx) * (radius - dx) + (radius - dy) * (radius - dy) > radius * radius) ++dx;
        rect(x + dx, y + dy, w - 2 * dx, 1, color);
        rect(x + dx, y + h - 1 - dy, w - 2 * dx, 1, color);
    }
}

static void blend(int x, int y, u32 color, unsigned alpha) {
    if ((unsigned)x >= WIDTH || (unsigned)y >= HEIGHT || !alpha) return;
    u32 *p = pixels + y * stride + x, dst = *p;
    unsigned inv = 255 - alpha;
    unsigned r = ((color & 255) * alpha + (dst & 255) * inv) / 255;
    unsigned g = (((color >> 8) & 255) * alpha + ((dst >> 8) & 255) * inv) / 255;
    unsigned b = (((color >> 16) & 255) * alpha + ((dst >> 16) & 255) * inv) / 255;
    *p = C(r, g, b);
}

static unsigned next_utf8(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    unsigned c = *p++;
    if (c >= 0xc2 && c <= 0xdf && (p[0] & 0xc0) == 0x80) { c = ((c & 31) << 6) | (*p++ & 63); }
    else if (c >= 0xe0 && c <= 0xef && p[0] && p[1] && (p[0] & 0xc0) == 0x80 && (p[1] & 0xc0) == 0x80) {
        c = ((c & 15) << 12) | ((p[0] & 63) << 6) | (p[1] & 63); p += 2;
    } else if (c >= 0x80) c = '?';
    *s = (const char *)p;
    return c;
}

/* Coordinates specify the top of the line; max_width clips long status text. */
static void text(int x, int y, int size, u32 color, const char *s, int max_width) {
    float scale = stbtt_ScaleForPixelHeight(&font, (float)size);
    int ascent, descent, gap;
    stbtt_GetFontVMetrics(&font, &ascent, &descent, &gap);
    int baseline = y + (int)(ascent * scale);
    float cursor = (float)x;
    while (*s) {
        unsigned cp = next_utf8(&s);
        int advance, lsb, w, h, xoff, yoff;
        stbtt_GetCodepointHMetrics(&font, (int)cp, &advance, &lsb);
        unsigned char *bitmap = stbtt_GetCodepointBitmap(&font, 0, scale, (int)cp, &w, &h, &xoff, &yoff);
        if (cursor + advance * scale > x + max_width) { if (bitmap) stbtt_FreeBitmap(bitmap, NULL); break; }
        if (bitmap) {
            for (int yy = 0; yy < h; ++yy)
                for (int xx = 0; xx < w; ++xx)
                    blend((int)cursor + xoff + xx, baseline + yoff + yy, color, bitmap[yy * w + xx]);
            stbtt_FreeBitmap(bitmap, NULL);
        }
        cursor += advance * scale;
    }
}

static void status_text(int x, int y, const char *message) {
    char line[46];
    for (int row = 0; row < 2 && *message; ++row) {
        size_t count = strlen(message);
        if (count >= sizeof(line)) {
            count = sizeof(line) - 1;
            while (count > 0 && message[count] != ' ') --count;
            if (!count) count = sizeof(line) - 1;
        }
        memcpy(line, message, count); line[count] = 0;
        text(x, y + row * 22, 16, white, line, 363);
        message += count;
        while (*message == ' ') ++message;
    }
}

static void icon(int x, int y) {
    for (int yy = 0; yy < 104; ++yy) for (int xx = 0; xx < 104; ++xx) {
        u16 p = app_icon[yy * 104 + xx];
        pixels[(y + yy) * stride + x + xx] = C(((p >> 11) & 31) * 255 / 31, ((p >> 5) & 63) * 255 / 63, (p & 31) * 255 / 31);
    }
}

static void field_row(int y, const char *name, const char *value, const char *hint, bool selected, u32 accent) {
    round_rect(48, y, 744, 67, 16, selected ? blue : border);
    round_rect(50, y + 2, 740, 63, 14, selected ? C(32, 45, 91) : field);
    if (selected) round_rect(60, y + 13, 4, 41, 2, accent);
    text(79, y + 10, 20, white, name, 285);
    text(79, y + 37, 14, muted, hint, 385);
    text(514, y + 21, 20, selected ? white : muted, value, 250);
}

bool ui_init(void) {
    Result rc = plInitialize(PlServiceType_User);
    if (R_FAILED(rc)) return false;
    have_pl = true;
    rc = plGetSharedFontByType(&shared_font, PlSharedFontType_Standard);
    int offset = R_SUCCEEDED(rc) ? stbtt_GetFontOffsetForIndex(shared_font.address, 0) : -1;
    if (offset < 0 || !stbtt_InitFont(&font, shared_font.address, offset)) {
        ui_exit(); return false;
    }
    // Um segundo buffer permite desenhar enquanto o primeiro é exibido.
    rc = framebufferCreate(&fb, nwindowGetDefault(), WIDTH, HEIGHT, PIXEL_FORMAT_RGBA_8888, 2);
    if (R_FAILED(rc)) { ui_exit(); return false; }
    have_fb = true;
    if (R_FAILED(framebufferMakeLinear(&fb))) { ui_exit(); return false; }
    return true;
}

void ui_draw(const UiState *s) {
    u32 stride_bytes = 0;
    pixels = framebufferBegin(&fb, &stride_bytes);
    if (!pixels) return;
    stride = stride_bytes / sizeof(u32);
    rect(0, 0, WIDTH, HEIGHT, bg);
    rect(0, 0, 8, HEIGHT, blue);
    icon(47, 24);
    text(171, 38, 16, blue, "NINTENDO SWITCH  /  DISCORD", 490);
    text(171, 63, 34, white, "Switch Rich Presence", 590);
    text(171, 103, 17, muted, "Seu jogo conectado ao Discord.", 590);
    round_rect(1031, 47, 197, 39, 19, C(22, 48, 47));
    text(1051, 57, 16, green, s->enabled ? "PRESENÇA ATIVA" : "PRESENÇA PAUSADA", 165);

    text(48, 157, 19, white, "Configuração", 450);
    text(48, 184, 15, muted, "Selecione uma opção e pressione A.", 550);
    char port[16]; snprintf(port, sizeof(port), "%u", s->port);
    field_row(215, "IP do computador", s->host, "Endereço exibido no companion", s->selected == 0, red);
    field_row(291, "Porta UDP", port, "Deve ser igual à porta do PC", s->selected == 1, blue);
    field_row(367, "Compartilhar presença", s->enabled ? "ATIVADA" : "PAUSADA", "Exibir sua atividade no Discord", s->selected == 2, green);
    field_row(443, "Testar conexão", "TESTAR  >", "Verifica a comunicação com o companion", s->selected == 3, blue);
    field_row(519, "Iniciar com o console", s->autostart ? "LIGADO" : "DESLIGADO", "Alteração exige reiniciar o console", s->selected == 4, red);

    round_rect(817, 156, 411, 215, 20, panel);
    text(839, 179, 14, blue, "SUA CONEXÃO", 360);
    text(839, 214, 17, muted, "IP do Switch", 350);
    text(839, 240, 22, white, s->ip, 350);
    rect(839, 280, 365, 1, border);
    text(839, 298, 14, muted, "O teste confirma apenas a conexão", 355);
    text(839, 320, 14, muted, "com o companion no computador.", 355);

    round_rect(817, 388, 411, 198, 20, panel);
    text(839, 411, 14, green, "ESTADO DO SYSMODULE", 360);
    status_text(839, 441, s->module_status);
    text(839, 505, 14, muted, "Último registro; pode estar antigo", 355);
    text(839, 527, 14, muted, "se o módulo não estiver ativo.", 355);
    if (s->dirty) text(839, 546, 14, C(255, 193, 100), "ALTERAÇÕES NÃO SALVAS", 355);

    round_rect(48, 609, 1180, 48, 13, C(22, 33, 58));
    text(67, 624, 16, white, s->notice, 1130);
    text(49, 678, 15, muted, "DIRECIONAL / ANALÓGICO  navegar     A  selecionar     +  salvar     -  sair", 1120);
    framebufferEnd(&fb);
}

void ui_exit(void) {
    if (have_fb) { framebufferClose(&fb); have_fb = false; }
    if (have_pl) { plExit(); have_pl = false; }
}
