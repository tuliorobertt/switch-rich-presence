#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <switch.h>
#include "ui.h"
#include "discovery.h"

#define CONFIG_PATH "sdmc:/config/switch-richpresence.ini"
#define TEMP_PATH "sdmc:/config/switch-richpresence.ini.tmp"
#define BACKUP_PATH "sdmc:/config/switch-richpresence.ini.bak"
#define STATUS_PATH "sdmc:/config/switch-richpresence.status"
#define MODULE_DIR "sdmc:/atmosphere/contents/4200000000075250"
#define MODULE_NSP MODULE_DIR "/exefs.nsp"
#define MODULE_FLAGS MODULE_DIR "/flags"
#define BOOT_FLAG MODULE_FLAGS "/boot2.flag"
#define MODULE_ID 0x4200000000075250ULL

typedef struct { char host[64]; unsigned port; bool enabled; } Config;
static Config cfg = {.host = "auto", .port = 39421, .enabled = true};
static char notice[160] = "Use a busca automática (X) ou configure o IP do PC.";
static bool dirty, sockets_ready, nifm_ready, graphical_ui, show_help;
static bool pmshell_ready, pmdmnt_ready;

static bool prompt(const char *guide, const char *initial, char *out, size_t size, u32 max_len) {
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0))) { snprintf(notice, sizeof(notice), "Não foi possível abrir o teclado."); return false; }
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetGuideText(&kbd, guide);
    swkbdConfigSetInitialText(&kbd, initial);
    swkbdConfigSetStringLenMax(&kbd, max_len);
    Result rc = swkbdShow(&kbd, out, size);
    swkbdClose(&kbd);
    return R_SUCCEEDED(rc);
}

static bool valid_host(const char *host) {
    struct in_addr address;
    return !strcmp(host, "auto") || (inet_pton(AF_INET, host, &address) == 1 && address.s_addr != INADDR_ANY);
}

static void load_config(void) {
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) return;
    char line[160];
    while (fgets(line, sizeof(line), f)) {
        char *comment = strchr(line, '#');
        if (comment) *comment = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq++ = 0;
        char *key = line;
        while (*key == ' ' || *key == '\t') key++;
        char *end = key + strlen(key);
        while (end > key && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
        while (*eq == ' ' || *eq == '\t') eq++;
        eq[strcspn(eq, " \t\r\n")] = 0;
        if (!strcmp(key, "host") && valid_host(eq)) snprintf(cfg.host, sizeof(cfg.host), "%s", eq);
        else if (!strcmp(key, "port")) {
            char *tail;
            long port = strtol(eq, &tail, 10);
            if (*eq && !*tail && port >= 1 && port <= 65535) cfg.port = (unsigned)port;
        } else if (!strcmp(key, "enabled")) {
            if (!strcmp(eq, "0")) cfg.enabled = false;
            else if (!strcmp(eq, "1")) cfg.enabled = true;
        }
    }
    fclose(f);
    snprintf(notice, sizeof(notice), "Configuração carregada do cartão SD.");
}

static bool save_config(void) {
    if (!valid_host(cfg.host) || cfg.port == 0 || cfg.port > 65535) {
        snprintf(notice, sizeof(notice), "IP ou porta inválidos.");
        return false;
    }
    if (mkdir("sdmc:/config", 0777) && errno != EEXIST) {
        snprintf(notice, sizeof(notice), "Erro ao abrir a pasta config (errno %d).", errno);
        return false;
    }
    FILE *f = fopen(TEMP_PATH, "w");
    if (!f) { snprintf(notice, sizeof(notice), "Erro ao gravar no SD (errno %d).", errno); return false; }
    bool ok = fprintf(f, "host=%s\nport=%u\nenabled=%d\n", cfg.host, cfg.port, cfg.enabled ? 1 : 0) > 0;
    if (fclose(f)) ok = false;
    bool had_config = access(CONFIG_PATH, F_OK) == 0;
    if (ok && had_config) {
        if (remove(BACKUP_PATH) != 0 && errno != ENOENT) ok = false;
        if (ok && rename(CONFIG_PATH, BACKUP_PATH) != 0) ok = false;
    }
    if (ok && rename(TEMP_PATH, CONFIG_PATH) != 0) {
        if (had_config) rename(BACKUP_PATH, CONFIG_PATH);
        ok = false;
    }
    if (!ok) { remove(TEMP_PATH); snprintf(notice, sizeof(notice), "Erro ao salvar; confira o cartão SD."); return false; }
    dirty = false;
    snprintf(notice, sizeof(notice), "Salvo. O sysmodule aplica a mudança em alguns segundos.");
    return true;
}

static bool autostart_enabled(void) { return access(BOOT_FLAG, F_OK) == 0; }

static void toggle_autostart(void) {
    if (access(MODULE_NSP, F_OK) != 0) { snprintf(notice, sizeof(notice), "Sysmodule não encontrado no SD. Copie a pasta atmosphere."); return; }
    if (autostart_enabled()) {
        if (remove(BOOT_FLAG) != 0) { snprintf(notice, sizeof(notice), "Não foi possível desativar o início automático."); return; }
        snprintf(notice, sizeof(notice), "Início automático desativado (vale no próximo boot).");
    } else {
        if (mkdir(MODULE_FLAGS, 0777) && errno != EEXIST) { snprintf(notice, sizeof(notice), "Não foi possível criar a pasta flags."); return; }
        FILE *f = fopen(BOOT_FLAG, "w");
        if (!f) { snprintf(notice, sizeof(notice), "Não foi possível ativar o início automático."); return; }
        if (fclose(f)) { snprintf(notice, sizeof(notice), "Erro ao gravar boot2.flag."); return; }
        snprintf(notice, sizeof(notice), "Início automático ativado (vale no próximo boot).");
    }
}

static void test_connection(void) {
    if (!valid_host(cfg.host)) { snprintf(notice, sizeof(notice), "IP inválido."); return; }
    if (!nifm_ready) nifm_ready = R_SUCCEEDED(nifmInitialize(NifmServiceType_User));
    SocketInitConfig socket_config = *socketGetDefaultInitConfig();
    socket_config.num_bsd_sessions = 1;
    Result rc = socketInitialize(&socket_config);
    if (R_FAILED(rc)) { snprintf(notice, sizeof(notice), "Erro ao iniciar a rede: 0x%08lX.", (unsigned long)rc); return; }
    sockets_ready = true;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { snprintf(notice, sizeof(notice), "Erro ao criar socket UDP."); socketExit(); sockets_ready = false; return; }
    struct sockaddr_in dst = {0}, from = {0};
    dst.sin_family = AF_INET;
    dst.sin_port = htons((uint16_t)cfg.port);
    bool automatic = !strcmp(cfg.host, "auto");
    if (automatic) {
        int yes = 1;
        if (setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes)) != 0) {
            snprintf(notice, sizeof(notice), "Erro ao iniciar a busca automática."); close(fd); socketExit(); sockets_ready = false; return;
        }
        dst.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    } else inet_pton(AF_INET, cfg.host, &dst.sin_addr);
    unsigned long long nonce = (unsigned long long)armGetSystemTick();
    char packet[100], expected[100], reply[128];
    snprintf(packet, sizeof(packet), automatic ? "{\"type\":\"ping\",\"nonce\":\"%016llX\",\"discovery\":true}" : "{\"type\":\"ping\",\"nonce\":\"%016llX\"}", nonce);
    snprintf(expected, sizeof(expected), "{\"type\":\"pong\",\"nonce\":\"%016llX\"}", nonce);
    if (automatic) discovery_send_candidates(fd, cfg.port, packet, strlen(packet), true);
    else if (sendto(fd, packet, strlen(packet), 0, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
        snprintf(notice, sizeof(notice), "Falha ao enviar o teste UDP (%d).", errno);
        close(fd);
        socketExit(); sockets_ready = false;
        return;
    }
    fd_set readset;
    FD_ZERO(&readset); FD_SET(fd, &readset);
    struct timeval timeout = {.tv_sec = 8, .tv_usec = 0};
    int selected = select(fd + 1, &readset, NULL, NULL, &timeout);
    socklen_t from_len = sizeof(from);
    ssize_t received = selected > 0 ? recvfrom(fd, reply, sizeof(reply) - 1, 0, (struct sockaddr *)&from, &from_len) : -1;
    if (received >= 0) reply[received] = 0;
    if (received > 0 && (automatic || from.sin_addr.s_addr == dst.sin_addr.s_addr) && from.sin_port == dst.sin_port && !strcmp(reply, expected)) {
        char address[INET_ADDRSTRLEN] = "";
        inet_ntop(AF_INET, &from.sin_addr, address, sizeof(address));
        snprintf(notice, sizeof(notice), "Conexão OK: o companion em %s respondeu. (O Discord não é testado aqui.)", address);
    } else
        snprintf(notice, sizeof(notice), "Sem resposta. Confira o IP, se o companion está aberto e o firewall.");
    close(fd);
    socketExit(); sockets_ready = false;
}

typedef struct {
    char line[160], game[384], title[48], companion[64];
    UiStatusKind kind;
} ModuleStatus;

/* Turns the sysmodule status file (line 1: message, line 2: game) into UI data. */
static void read_status(ModuleStatus *st) {
    memset(st, 0, sizeof(*st));
    FILE *f = fopen(STATUS_PATH, "r");
    if (!f) {
        snprintf(st->line, sizeof(st->line), "Nenhum registro. O sysmodule pode não ter iniciado.");
        snprintf(st->title, sizeof(st->title), "Sem registro");
        st->kind = UI_STATUS_UNKNOWN;
        return;
    }
    if (!fgets(st->line, sizeof(st->line), f)) snprintf(st->line, sizeof(st->line), "Registro de status vazio.");
    if (!fgets(st->game, sizeof(st->game), f)) st->game[0] = 0;
    fclose(f);
    st->line[strcspn(st->line, "\r\n")] = 0;
    st->game[strcspn(st->game, "\r\n")] = 0;
    const char *m = st->line;
    unsigned long long tid = 0;
    char host[64] = "";
    unsigned port = 0;
    if (sscanf(m, "UDP enviado: %llX para %63[^:]:%u", &tid, host, &port) >= 2) {
        st->kind = UI_STATUS_OK;
        snprintf(st->title, sizeof(st->title), "Enviando ao PC");
        snprintf(st->companion, sizeof(st->companion), "%s", host);
        if (!tid) snprintf(st->game, sizeof(st->game), "Menu HOME");
        else if (!st->game[0]) snprintf(st->game, sizeof(st->game), "Title ID %016llX", tid);
    } else if (strstr(m, "Procurando")) { st->kind = UI_STATUS_BUSY; snprintf(st->title, sizeof(st->title), "Procurando o PC"); }
    else if (strstr(m, "Aguardando rede")) { st->kind = UI_STATUS_BUSY; snprintf(st->title, sizeof(st->title), "Aguardando a rede"); }
    else if (strstr(m, "Iniciando")) { st->kind = UI_STATUS_BUSY; snprintf(st->title, sizeof(st->title), "Iniciando"); }
    else if (strstr(m, "pausada")) { st->kind = UI_STATUS_PAUSED; snprintf(st->title, sizeof(st->title), "Presença pausada"); }
    else if (!strncmp(m, "Erro", 4) || strstr(m, "invalida")) { st->kind = UI_STATUS_ERROR; snprintf(st->title, sizeof(st->title), "Atenção"); }
    else { st->kind = UI_STATUS_UNKNOWN; snprintf(st->title, sizeof(st->title), "Estado"); }
    if (st->kind != UI_STATUS_OK) st->game[0] = 0;
}

static void local_ip(char *out, size_t size) {
    u32 ip;
    if (!nifm_ready) nifm_ready = R_SUCCEEDED(nifmInitialize(NifmServiceType_User));
    if (!nifm_ready) { snprintf(out, size, "indisponível"); return; }
    if (R_FAILED(nifmGetCurrentIpAddress(&ip)) || !ip) { snprintf(out, size, "sem rede"); return; }
    struct in_addr address = {.s_addr = ip};
    if (!inet_ntop(AF_INET, &address, out, size)) snprintf(out, size, "indisponível");
}

/* 1 running, 0 stopped, -1 unknown (pm:dmnt unavailable). */
static int module_running(void) {
    if (!pmdmnt_ready) pmdmnt_ready = R_SUCCEEDED(pmdmntInitialize());
    if (!pmdmnt_ready) return -1;
    u64 pid = 0;
    return R_SUCCEEDED(pmdmntGetProcessId(&pid, MODULE_ID)) && pid ? 1 : 0;
}

/* Tells the companion to clear the activity right away, since a stopped
   module cannot. Best effort: only when the companion address is known. */
static void send_paused(const char *host, unsigned port) {
    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port = htons((uint16_t)port);
    if (!host[0] || inet_pton(AF_INET, host, &dst.sin_addr) != 1) return;
    SocketInitConfig socket_config = *socketGetDefaultInitConfig();
    socket_config.num_bsd_sessions = 1;
    if (R_FAILED(socketInitialize(&socket_config))) return;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd >= 0) {
        static const char packet[] = "{\"title_id\":\"0000000000000000\",\"state\":\"paused\"}";
        sendto(fd, packet, sizeof(packet) - 1, 0, (struct sockaddr *)&dst, sizeof(dst));
        close(fd);
    }
    socketExit();
}

/* Starts or stops the sysmodule from the SD without rebooting (Atmosphère). */
static void toggle_module(const char *companion) {
    if (access(MODULE_NSP, F_OK) != 0) { snprintf(notice, sizeof(notice), "Sysmodule não encontrado no SD. Copie a pasta atmosphere."); return; }
    if (!pmshell_ready) pmshell_ready = R_SUCCEEDED(pmshellInitialize());
    if (!pmshell_ready) { snprintf(notice, sizeof(notice), "Sem acesso ao pm:shell; reinicie o console para iniciar o módulo."); return; }
    if (module_running() == 1) {
        Result rc = pmshellTerminateProgram(MODULE_ID);
        if (R_FAILED(rc)) { snprintf(notice, sizeof(notice), "Não foi possível parar o sysmodule (0x%08lX).", (unsigned long)rc); return; }
        send_paused(companion[0] ? companion : (strcmp(cfg.host, "auto") ? cfg.host : ""), cfg.port);
        remove(STATUS_PATH);
        snprintf(notice, sizeof(notice), "Sysmodule parado. A presença foi removida do Discord.");
    } else {
        NcmProgramLocation location = {.program_id = MODULE_ID, .storageID = NcmStorageId_None};
        u64 pid = 0;
        Result rc = pmshellLaunchProgram(0, &location, &pid);
        if (R_FAILED(rc)) { snprintf(notice, sizeof(notice), "Não foi possível iniciar o sysmodule (0x%08lX).", (unsigned long)rc); return; }
        snprintf(notice, sizeof(notice), "Sysmodule iniciado. Ele procura o PC em alguns segundos.");
    }
}

/* libnx's text console has no accented glyphs: fold common Latin letters. */
static const char *ascii(const char *in) {
    static char out[4][400];
    static int slot;
    slot = (slot + 1) % 4;
    char *o = out[slot];
    size_t j = 0;
    while (*in && j + 1 < sizeof(out[0])) {
        unsigned char c = (unsigned char)*in;
        if (c == 0xC3 && in[1]) {
            static const char map[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
            unsigned char d = (unsigned char)in[1];
            o[j++] = d >= 0x80 && d <= 0xBF ? map[d - 0x80] : '?';
            in += 2;
        } else if (c >= 0x80) {
            if (j == 0 || o[j - 1] != '?') o[j++] = '?';
            ++in;
            while (((unsigned char)*in & 0xC0) == 0x80) ++in;
        } else o[j++] = *in++;
    }
    o[j] = 0;
    return o;
}

static void draw_console(int selected, const ModuleStatus *st, int running, const char *ip) {
    consoleClear();
    printf("\x1b[1;1H\x1b[36m  SWITCH RICH PRESENCE\x1b[0m\n");
    printf("  Configuracao e diagnostico\n");
    printf("  -------------------------------------------------------\n\n");
    printf("  %s IP do PC           %s\n", selected == 0 ? ">" : " ", cfg.host);
    printf("  %s Porta UDP          %u\n", selected == 1 ? ">" : " ", cfg.port);
    printf("  %s Presenca           %s\n", selected == 2 ? ">" : " ", cfg.enabled ? "Ativa" : "Pausada");
    printf("  %s Testar conexao     Aguardando acao\n", selected == 3 ? ">" : " ");
    printf("  %s Sysmodule          %s\n", selected == 4 ? ">" : " ", running == 1 ? "Em execucao" : running == 0 ? "Parado" : "Desconhecido");
    printf("  %s Inicio automatico  %s\n", selected == 5 ? ">" : " ", autostart_enabled() ? "Ligado" : "Desligado");
    printf("\n  IP do Switch: %s\n", ascii(ip));
    printf("  Sysmodule (ultimo registro): %s\n  %s\n", ascii(st->title), ascii(st->line));
    if (st->game[0]) printf("  Jogo: %s\n", ascii(st->game));
    printf("\n  %s%s\n", dirty ? "[NAO SALVO] " : "", ascii(notice));
    printf("\n  Direcional: selecionar   A: editar/acionar\n  X: busca automatica   +: salvar   -: sair\n");
    printf("  Salve para aplicar IP, porta e presenca.\n");
}

static ModuleStatus last_status;

static void draw(int selected) {
    char ip[32];
    read_status(&last_status);
    local_ip(ip, sizeof(ip));
    int running = module_running();
    if (running == 0 && last_status.kind != UI_STATUS_UNKNOWN) {
        last_status.kind = UI_STATUS_ERROR;
        snprintf(last_status.title, sizeof(last_status.title), "Módulo parado");
        snprintf(last_status.line, sizeof(last_status.line), "Use a opção Sysmodule para iniciá-lo agora.");
        last_status.game[0] = 0;
        last_status.companion[0] = 0;
    }
    if (!graphical_ui) { draw_console(selected, &last_status, running, ip); consoleUpdate(NULL); return; }
    UiState state = { .host = cfg.host, .port = cfg.port, .enabled = cfg.enabled,
                      .autostart = autostart_enabled(), .dirty = dirty, .help = show_help,
                      .module_running = running, .ip = ip, .notice = notice,
                      .status_kind = last_status.kind, .status_title = last_status.title,
                      .status_detail = last_status.line, .game = last_status.game,
                      .companion = last_status.companion, .selected = selected };
    ui_draw(&state);
}

static void activate(int selected) {
    if (selected == 0) {
        char input[64] = "";
        if (prompt("IP do PC (ou auto)", cfg.host, input, sizeof(input), 15)) {
            if (valid_host(input)) { snprintf(cfg.host, sizeof(cfg.host), "%s", input); dirty = true; snprintf(notice, sizeof(notice), "IP alterado. Pressione + para salvar."); }
            else snprintf(notice, sizeof(notice), "IPv4 inválido.");
        }
    } else if (selected == 1) {
        char old[8], input[16] = "";
        snprintf(old, sizeof(old), "%u", cfg.port);
        if (prompt("Porta UDP: 1 até 65535", old, input, sizeof(input), 5)) {
            char *tail;
            long port = strtol(input, &tail, 10);
            if (*input && !*tail && port >= 1 && port <= 65535) { cfg.port = (unsigned)port; dirty = true; snprintf(notice, sizeof(notice), "Porta alterada. Pressione + para salvar."); }
            else snprintf(notice, sizeof(notice), "Porta inválida.");
        }
    } else if (selected == 2) {
        cfg.enabled = !cfg.enabled;
        dirty = true;
        snprintf(notice, sizeof(notice), "Pressione + para aplicar a mudança.");
    } else if (selected == 3) {
        snprintf(notice, sizeof(notice), "Testando conexão com o PC...");
        draw(selected);
        test_connection();
    } else if (selected == 4) toggle_module(last_status.companion);
    else toggle_autostart();
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    graphical_ui = ui_init();
    if (!graphical_ui) consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    hidInitializeTouchScreen();
    load_config();
    int selected = 0;
    unsigned frame = 0;
    bool touching = false;
    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad);
        bool redraw = down != 0;
        HidTouchScreenState touch = {0};
        bool touched = hidGetTouchScreenStates(&touch, 1) && touch.count > 0;
        if (touched && !touching && graphical_ui) {
            redraw = true;
            if (show_help) show_help = false;
            else {
                int row = ui_row_at((int)touch.touches[0].x, (int)touch.touches[0].y);
                if (row >= 0) { if (row == selected) activate(row); else selected = row; }
            }
        }
        touching = touched;
        if (show_help) {
            if (down & HidNpadButton_Minus) break;
            if (down & (HidNpadButton_B | HidNpadButton_Y | HidNpadButton_A)) show_help = false;
        } else {
            if (down & HidNpadButton_Minus) break;
            if (down & HidNpadButton_AnyUp) selected = (selected + UI_ROWS - 1) % UI_ROWS;
            if (down & HidNpadButton_AnyDown) selected = (selected + 1) % UI_ROWS;
            if (down & HidNpadButton_Plus) save_config();
            if ((down & HidNpadButton_Y) && graphical_ui) show_help = true;
            if (down & HidNpadButton_X) { snprintf(cfg.host, sizeof(cfg.host), "auto"); dirty = true; save_config(); }
            if (down & HidNpadButton_A) activate(selected);
        }
        if (redraw || frame++ % 60 == 0) draw(selected);
        svcSleepThread(16666667ULL);
    }
    if (pmshell_ready) pmshellExit();
    if (pmdmnt_ready) pmdmntExit();
    if (nifm_ready) nifmExit();
    if (sockets_ready) socketExit();
    if (graphical_ui) ui_exit();
    else consoleExit(NULL);
    return 0;
}
