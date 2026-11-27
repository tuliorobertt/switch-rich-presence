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

/* Left column rows. Kept here so touch hit-testing matches drawing. */
#define ROW_X 48
#define ROW_W 744
#define ROW_Y0 200
#define ROW_STEP 66
#define ROW_H 58

/* Button glyphs in the console's NintendoExt shared font. */
#define GLYPH_A 0xE0E0
#define GLYPH_B 0xE0E1
#define GLYPH_X 0xE0E2
#define GLYPH_Y 0xE0E3
#define GLYPH_PLUS 0xE0EF
#define GLYPH_MINUS 0xE0F0
#define GLYPH_DPAD 0xE0EA

static Framebuffer fb;
static stbtt_fontinfo font, ext_font;
static PlFontData shared_font, shared_ext;
static bool have_ext;
static u32 *pixels, stride;
static bool have_fb, have_pl;

static const u32 bg = C(5, 10, 28), panel = C(14, 22, 46), field = C(24, 35, 66);
static const u32 white = C(244, 247, 255), muted = C(159, 174, 203), blue = C(86, 118, 255);
static const u32 red = C(252, 44, 63), green = C(41, 219, 133), border = C(48, 64, 106);
static const u32 amber = C(255, 193, 100);

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

static void dim(unsigned alpha) {
    for (int y = 0; y < HEIGHT; ++y) for (int x = 0; x < WIDTH; ++x) blend(x, y, C(0, 0, 0), alpha);
}

static void dot(int cx, int cy, int radius, u32 color) {
    for (int dy = -radius; dy <= radius; ++dy) for (int dx = -radius; dx <= radius; ++dx) {
        int d = dx * dx + dy * dy, r2 = radius * radius;
        if (d <= r2) blend(cx + dx, cy + dy, color, d > r2 - radius ? 140 : 255);
    }
}

static unsigned next_utf8(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    unsigned c = *p++;
    if (c >= 0xc2 && c <= 0xdf && (p[0] & 0xc0) == 0x80) { c = ((c & 31) << 6) | (*p++ & 63); }
    else if (c >= 0xe0 && c <= 0xef && p[0] && p[1] && (p[0] & 0xc0) == 0x80 && (p[1] & 0xc0) == 0x80) {
        c = ((c & 15) << 12) | ((p[0] & 63) << 6) | (p[1] & 63); p += 2;
    } else if (c >= 0xf0 && c <= 0xf4 && p[0] && p[1] && p[2]) { c = '?'; p += 3; }
    else if (c >= 0x80) c = '?';
    *s = (const char *)p;
    return c;
}

/* Picks the font that has the glyph: standard first, NintendoExt for icons. */
static stbtt_fontinfo *font_for(unsigned cp) {
    if (have_ext && cp >= 0xE000 && cp <= 0xF8FF && stbtt_FindGlyphIndex(&ext_font, (int)cp)) return &ext_font;
    return &font;
}

static int text_width(int size, const char *s) {
    float width = 0;
    while (*s) {
        unsigned cp = next_utf8(&s);
        stbtt_fontinfo *f = font_for(cp);
        int advance, lsb;
        stbtt_GetCodepointHMetrics(f, (int)cp, &advance, &lsb);
        width += advance * stbtt_ScaleForPixelHeight(f, (float)size);
    }
    return (int)width;
}

/* Coordinates specify the top of the line; max_width clips with an ellipsis. */
static void text(int x, int y, int size, u32 color, const char *s, int max_width) {
    float base_scale = stbtt_ScaleForPixelHeight(&font, (float)size);
    int ascent, descent, gap;
    stbtt_GetFontVMetrics(&font, &ascent, &descent, &gap);
    int baseline = y + (int)(ascent * base_scale);
    bool clip = text_width(size, s) > max_width;
    int limit = x + max_width - (clip ? text_width(size, "…") : 0);
    float cursor = (float)x;
    while (*s) {
        unsigned cp = next_utf8(&s);
        stbtt_fontinfo *f = font_for(cp);
        float scale = stbtt_ScaleForPixelHeight(f, (float)size);
        int advance, lsb, w, h, xoff, yoff;
        stbtt_GetCodepointHMetrics(f, (int)cp, &advance, &lsb);
        if (cursor + advance * scale > limit) break;
        unsigned char *bitmap = stbtt_GetCodepointBitmap(f, 0, scale, (int)cp, &w, &h, &xoff, &yoff);
        if (bitmap) {
            for (int yy = 0; yy < h; ++yy)
                for (int xx = 0; xx < w; ++xx)
                    blend((int)cursor + xoff + xx, baseline + yoff + yy, color, bitmap[yy * w + xx]);
            stbtt_FreeBitmap(bitmap, NULL);
        }
        cursor += advance * scale;
    }
    if (clip) text((int)cursor, y, size, color, "…", max_width);
}

/* Word-wraps up to `lines` lines inside `width` pixels. */
static void paragraph(int x, int y, int size, int line_height, u32 color, const char *s, int width, int lines) {
    char line[256];
    for (int row = 0; row < lines && *s; ++row) {
        size_t best = 0, i = 0;
        while (s[i] && i < sizeof(line) - 1) {
            size_t next = i;
            while (s[next] && s[next] != ' ') ++next;
            if (next >= sizeof(line)) break;
            memcpy(line, s, next); line[next] = 0;
            if (text_width(size, line) > width && best) break;
            best = next;
            if (!s[next]) break;
            i = next + 1;
        }
        if (!best) best = strlen(s) < sizeof(line) - 1 ? strlen(s) : sizeof(line) - 1;
        memcpy(line, s, best); line[best] = 0;
        bool last = row == lines - 1 && s[best];
        text(x, y + row * line_height, size, color, last ? s : line, width);
        s += best;
        while (*s == ' ') ++s;
    }
}

static void icon(int x, int y) {
    for (int yy = 0; yy < 104; ++yy) for (int xx = 0; xx < 104; ++xx) {
        u16 p = app_icon[yy * 104 + xx];
        pixels[(y + yy) * stride + x + xx] = C(((p >> 11) & 31) * 255 / 31, ((p >> 5) & 63) * 255 / 63, (p & 31) * 255 / 31);
    }
}

/* Draws a controller button: the console glyph when available, a badge otherwise. */
static int button_hint(int x, int y, unsigned glyph, const char *fallback, const char *label) {
    char symbol[8];
    int w;
    if (have_ext && stbtt_FindGlyphIndex(&ext_font, (int)glyph)) {
        symbol[0] = (char)(0xE0 | (glyph >> 12)); symbol[1] = (char)(0x80 | ((glyph >> 6) & 63));
        symbol[2] = (char)(0x80 | (glyph & 63)); symbol[3] = 0;
        text(x, y - 3, 22, white, symbol, 40);
        w = text_width(22, symbol);
    } else {
        w = text_width(14, fallback) + 16;
        if (w < 26) w = 26;
        round_rect(x, y - 2, w, 24, 12, white);
        text(x + (w - text_width(14, fallback)) / 2, y + 1, 14, bg, fallback, w);
    }
    text(x + w + 8, y, 16, muted, label, 220);
    return x + w + 8 + text_width(16, label) + 30;
}

static void field_row(int index, const char *name, const char *value, const char *hint, bool selected, u32 accent, u32 value_color) {
    int y = ROW_Y0 + index * ROW_STEP;
    round_rect(ROW_X, y, ROW_W, ROW_H, 15, selected ? blue : border);
    round_rect(ROW_X + 2, y + 2, ROW_W - 4, ROW_H - 4, 13, selected ? C(32, 45, 91) : field);
    if (selected) round_rect(ROW_X + 12, y + 11, 4, ROW_H - 22, 2, accent);
    text(79, y + 7, 19, white, name, 400);
    text(79, y + 33, 14, muted, hint, 420);
    int vw = text_width(19, value);
    if (vw > 250) vw = 250;
    text(ROW_X + ROW_W - 26 - vw, y + 18, 19, selected ? white : value_color, value, 250);
}

int ui_row_at(int x, int y) {
    if (x < ROW_X || x >= ROW_X + ROW_W || y < ROW_Y0) return -1;
    int index = (y - ROW_Y0) / ROW_STEP;
    if (index >= UI_ROWS || (y - ROW_Y0) % ROW_STEP >= ROW_H) return -1;
    return index;
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
    /* Optional: button icons. Text falls back to drawn badges without it. */
    if (R_SUCCEEDED(plGetSharedFontByType(&shared_ext, PlSharedFontType_NintendoExt))) {
        int ext_offset = stbtt_GetFontOffsetForIndex(shared_ext.address, 0);
        have_ext = ext_offset >= 0 && stbtt_InitFont(&ext_font, shared_ext.address, ext_offset);
    }
    // Um segundo buffer permite desenhar enquanto o primeiro é exibido.
    rc = framebufferCreate(&fb, nwindowGetDefault(), WIDTH, HEIGHT, PIXEL_FORMAT_RGBA_8888, 2);
    if (R_FAILED(rc)) { ui_exit(); return false; }
    have_fb = true;
    if (R_FAILED(framebufferMakeLinear(&fb))) { ui_exit(); return false; }
    return true;
}

static void draw_help(void) {
    dim(170);
    round_rect(150, 70, 980, 580, 24, border);
    round_rect(152, 72, 976, 576, 22, panel);
    text(190, 100, 14, blue, "AJUDA RÁPIDA", 400);
    text(190, 122, 30, white, "Como colocar seu jogo no Discord", 880);
    static const char *steps[] = {
        "1  No PC, instale e abra o Switch Rich Presence. Deixe o Discord Desktop aberto.",
        "2  Switch e PC precisam estar na mesma rede. Permita o app no Firewall (redes privadas).",
        "3  Aqui, use IP do computador = Automático (botão X) ou digite o IP mostrado no PC.",
        "4  Selecione Testar conexão. O PC registra o teste na tela Atividade.",
        "5  Abra um jogo. Em alguns segundos ele aparece no seu perfil do Discord.",
    };
    for (int i = 0; i < 5; ++i) paragraph(190, 182 + i * 54, 18, 24, white, steps[i], 900, 2);
    rect(190, 456, 900, 1, border);
    text(190, 472, 14, green, "SE NÃO FUNCIONAR", 400);
    paragraph(190, 498, 16, 22, muted,
              "Confira se o sysmodule está em execução (opção Sysmodule). Redes de convidados ou com isolamento "
              "entre aparelhos bloqueiam a busca automática; nesse caso informe o IP manualmente. Se um jogo "
              "aparecer com a logo do Switch, defina uma imagem no PC em Visão geral > Personalizar jogo.", 900, 4);
    button_hint(190, 604, GLYPH_B, "B", "Fechar ajuda");
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

    const char *badge = s->enabled ? "PRESENÇA ATIVA" : "PRESENÇA PAUSADA";
    u32 badge_color = s->enabled ? green : amber;
    int bw = text_width(16, badge) + 50;
    round_rect(1228 - bw, 47, bw, 39, 19, s->enabled ? C(22, 48, 47) : C(58, 45, 24));
    dot(1228 - bw + 20, 66, 5, badge_color);
    text(1228 - bw + 34, 57, 16, badge_color, badge, bw);

    text(48, 146, 19, white, "Configuração", 450);
    text(48, 172, 15, muted, "Selecione uma opção e pressione A, ou toque nela.", 600);
    char port[16]; snprintf(port, sizeof(port), "%u", s->port);
    bool automatic = !strcmp(s->host, "auto");
    field_row(0, "IP do computador", automatic ? "AUTOMÁTICO" : s->host,
              automatic ? "Busca o PC na rede local  |  A: digitar IP" : "IP manual  |  X: voltar ao automático",
              s->selected == 0, red, white);
    field_row(1, "Porta UDP", port, "Deve ser igual à porta do PC (padrão 39421)", s->selected == 1, blue, muted);
    field_row(2, "Compartilhar presença", s->enabled ? "ATIVADA" : "PAUSADA", "Exibir sua atividade no Discord",
              s->selected == 2, green, s->enabled ? green : amber);
    field_row(3, "Testar conexão", "TESTAR  ›", "Verifica a comunicação com o companion", s->selected == 3, blue, muted);
    field_row(4, "Sysmodule", s->module_running == 1 ? "EM EXECUÇÃO" : s->module_running == 0 ? "PARADO" : "DESCONHECIDO",
              s->module_running == 1 ? "A: parar agora (sem reiniciar o console)" : "A: iniciar agora (sem reiniciar o console)",
              s->selected == 4, green, s->module_running == 1 ? green : amber);
    field_row(5, "Iniciar com o console", s->autostart ? "LIGADO" : "DESLIGADO", "Vale a partir do próximo boot",
              s->selected == 5, red, s->autostart ? white : muted);

    /* Status card */
    static const u32 *kind_colors[] = {&muted, &green, &blue, &amber, &red};
    u32 kind_color = *kind_colors[s->status_kind];
    round_rect(817, 146, 411, 238, 20, panel);
    text(839, 166, 14, green, "ESTADO DO SYSMODULE", 360);
    dot(847, 207, 7, kind_color);
    text(866, 194, 22, white, s->status_title, 340);
    paragraph(839, 228, 14, 19, muted, s->status_detail, 365, 2);
    rect(839, 276, 365, 1, border);
    text(839, 290, 14, muted, "JOGO ATUAL", 360);
    text(839, 312, 20, s->game[0] ? white : muted, s->game[0] ? s->game : "Nenhum jogo aberto", 365);
    text(839, 346, 13, muted, "Último registro do módulo; pode estar antigo.", 365);

    /* Connection card */
    round_rect(817, 398, 411, 188, 20, panel);
    text(839, 418, 14, blue, "SUA CONEXÃO", 360);
    text(839, 446, 15, muted, "IP do Switch", 170);
    text(839, 468, 20, white, s->ip, 180);
    text(1030, 446, 15, muted, "Porta", 170);
    text(1030, 468, 20, white, port, 180);
    text(839, 510, 15, muted, "Companion em uso", 350);
    text(839, 532, 20, s->companion[0] ? white : muted, s->companion[0] ? s->companion : "ainda não encontrado", 365);
    if (s->dirty) text(1030, 510, 14, amber, "NÃO SALVO", 180);

    round_rect(48, 604, 1180, 52, 14, s->dirty ? C(48, 40, 26) : C(22, 33, 58));
    text(67, 619, 17, s->dirty ? amber : white, s->notice, 1140);
    int x = 49;
    x = button_hint(x, 678, GLYPH_DPAD, "Dir", "Navegar");
    x = button_hint(x, 678, GLYPH_A, "A", "Selecionar");
    x = button_hint(x, 678, GLYPH_X, "X", "Automático");
    x = button_hint(x, 678, GLYPH_Y, "Y", "Ajuda");
    x = button_hint(x, 678, GLYPH_PLUS, "+", "Salvar");
    button_hint(x, 678, GLYPH_MINUS, "−", "Sair");
    if (s->help) draw_help();
    framebufferEnd(&fb);
}

void ui_exit(void) {
    if (have_fb) { framebufferClose(&fb); have_fb = false; }
    if (have_pl) { plExit(); have_pl = false; have_ext = false; }
}
