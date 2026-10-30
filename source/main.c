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

#define CONFIG_PATH "sdmc:/config/switch-richpresence.ini"
#define TEMP_PATH "sdmc:/config/switch-richpresence.ini.tmp"
#define BACKUP_PATH "sdmc:/config/switch-richpresence.ini.bak"
#define STATUS_PATH "sdmc:/config/switch-richpresence.status"
#define MODULE_DIR "sdmc:/atmosphere/contents/4200000000075250"
#define MODULE_NSP MODULE_DIR "/exefs.nsp"
#define MODULE_FLAGS MODULE_DIR "/flags"
#define BOOT_FLAG MODULE_FLAGS "/boot2.flag"

typedef struct { char host[64]; unsigned port; bool enabled; } Config;
static Config cfg = {.host = "192.168.1.2", .port = 39421, .enabled = true};
static char notice[160] = "Configure o IP do PC e salve.";
static bool dirty, sockets_ready, nifm_ready;

static bool prompt(const char *guide, const char *initial, char *out, size_t size, u32 max_len) {
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0))) { snprintf(notice, sizeof(notice), "Nao foi possivel abrir o teclado."); return false; }
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
    return inet_pton(AF_INET, host, &address) == 1 && address.s_addr != INADDR_ANY;
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
    snprintf(notice, sizeof(notice), "Configuracao carregada do SD.");
}

static bool save_config(void) {
    if (!valid_host(cfg.host) || cfg.port == 0 || cfg.port > 65535) {
        snprintf(notice, sizeof(notice), "IP ou porta invalidos.");
        return false;
    }
    if (mkdir("sdmc:/config", 0777) && errno != EEXIST) {
        snprintf(notice, sizeof(notice), "Erro ao abrir pasta config (errno %d).", errno);
        return false;
    }
    FILE *f = fopen(TEMP_PATH, "w");
    if (!f) { snprintf(notice, sizeof(notice), "Erro ao gravar SD (errno %d).", errno); return false; }
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
    if (!ok) { remove(TEMP_PATH); snprintf(notice, sizeof(notice), "Erro ao salvar; confira o SD."); return false; }
    dirty = false;
    snprintf(notice, sizeof(notice), "Salvo. Sysmodule atualizado em alguns segundos.");
    return true;
}

static bool autostart_enabled(void) { return access(BOOT_FLAG, F_OK) == 0; }

static void toggle_autostart(void) {
    if (access(MODULE_NSP, F_OK) != 0) { snprintf(notice, sizeof(notice), "Sysmodule nao encontrado no SD."); return; }
    if (autostart_enabled()) {
        if (remove(BOOT_FLAG) != 0) { snprintf(notice, sizeof(notice), "Nao foi possivel desativar inicio automatico."); return; }
        snprintf(notice, sizeof(notice), "Inicio automatico desativado. Reinicie o Switch.");
    } else {
        if (mkdir(MODULE_FLAGS, 0777) && errno != EEXIST) { snprintf(notice, sizeof(notice), "Nao foi possivel criar pasta flags."); return; }
        FILE *f = fopen(BOOT_FLAG, "w");
        if (!f) { snprintf(notice, sizeof(notice), "Nao foi possivel ativar inicio automatico."); return; }
        if (fclose(f)) { snprintf(notice, sizeof(notice), "Erro ao gravar boot2.flag."); return; }
        snprintf(notice, sizeof(notice), "Inicio automatico ativado. Reinicie o Switch.");
    }
}

static void test_connection(void) {
    if (!valid_host(cfg.host)) { snprintf(notice, sizeof(notice), "IP invalido."); return; }
    if (!nifm_ready) nifm_ready = R_SUCCEEDED(nifmInitialize(NifmServiceType_User));
    SocketInitConfig socket_config = *socketGetDefaultInitConfig();
    socket_config.num_bsd_sessions = 1;
    Result rc = socketInitialize(&socket_config);
    if (R_FAILED(rc)) { snprintf(notice, sizeof(notice), "Erro ao iniciar rede: 0x%08lX.", (unsigned long)rc); return; }
    sockets_ready = true;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { snprintf(notice, sizeof(notice), "Erro ao criar socket UDP."); socketExit(); sockets_ready = false; return; }
    struct sockaddr_in dst = {0}, from = {0};
    dst.sin_family = AF_INET;
    dst.sin_port = htons((uint16_t)cfg.port);
    inet_pton(AF_INET, cfg.host, &dst.sin_addr);
    unsigned long long nonce = (unsigned long long)armGetSystemTick();
    char packet[100], expected[100], reply[128];
    snprintf(packet, sizeof(packet), "{\"type\":\"ping\",\"nonce\":\"%016llX\"}", nonce);
    snprintf(expected, sizeof(expected), "{\"type\":\"pong\",\"nonce\":\"%016llX\"}", nonce);
    if (sendto(fd, packet, strlen(packet), 0, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
        snprintf(notice, sizeof(notice), "Falha ao enviar teste UDP (%d).", errno);
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
    if (received > 0 && from.sin_addr.s_addr == dst.sin_addr.s_addr && from.sin_port == dst.sin_port && !strcmp(reply, expected))
        snprintf(notice, sizeof(notice), "Conexao OK: companion respondeu. Discord nao foi testado.");
    else
        snprintf(notice, sizeof(notice), "Sem resposta. Confira IP, companion e firewall UDP.");
    close(fd);
    socketExit(); sockets_ready = false;
}

static void read_status(char *out, size_t size) {
    FILE *f = fopen(STATUS_PATH, "r");
    if (!f) { snprintf(out, size, "Sem registro. Sysmodule pode nao ter iniciado."); return; }
    if (!fgets(out, size, f)) snprintf(out, size, "Registro de status vazio.");
    out[strcspn(out, "\r\n")] = 0;
    fclose(f);
}

static void local_ip(char *out, size_t size) {
    u32 ip;
    if (!nifm_ready) { snprintf(out, size, "use Testar conexao"); return; }
    if (R_FAILED(nifmGetCurrentIpAddress(&ip)) || !ip) { snprintf(out, size, "indisponivel"); return; }
    struct in_addr address = {.s_addr = ip};
    if (!inet_ntop(AF_INET, &address, out, size)) snprintf(out, size, "indisponivel");
}

static void draw(int selected) {
    char module_status[160], ip[32];
    read_status(module_status, sizeof(module_status));
    local_ip(ip, sizeof(ip));
    consoleClear();
    printf("\x1b[1;1H\x1b[36m  SWITCH RICH PRESENCE\x1b[0m\n");
    printf("  Configuracao e diagnostico\n");
    printf("  -------------------------------------------------------\n\n");
    printf("  %s IP do PC           %s\n", selected == 0 ? ">" : " ", cfg.host);
    printf("  %s Porta UDP          %u\n", selected == 1 ? ">" : " ", cfg.port);
    printf("  %s Presenca           %s\n", selected == 2 ? ">" : " ", cfg.enabled ? "Ativa" : "Pausada");
    printf("  %s Testar conexao     Aguardando acao\n", selected == 3 ? ">" : " ");
    printf("  %s Inicio automatico  %s\n", selected == 4 ? ">" : " ", autostart_enabled() ? "Ligado" : "Desligado");
    printf("\n  IP do Switch: %s\n", ip);
    printf("  Ultimo estado do sysmodule (pode ser antigo):\n  %s\n", module_status);
    printf("\n  %s%s\n", dirty ? "[NAO SALVO] " : "", notice);
    printf("\n  Direcional: selecionar   A: editar/acionar\n  +: salvar   -: sair\n");
    printf("  Salve para aplicar IP, porta e presenca.\n");
    printf("  Inicio automatico requer reiniciar o console.\n");
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    load_config();
    int selected = 0;
    unsigned frame = 0;
    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad);
        if (down & HidNpadButton_Minus) break;
        if (down & HidNpadButton_Up) selected = (selected + 4) % 5;
        if (down & HidNpadButton_Down) selected = (selected + 1) % 5;
        if (down & HidNpadButton_Plus) save_config();
        if (down & HidNpadButton_A) {
            if (selected == 0) {
                char input[64] = "";
                if (prompt("IPv4 do PC, ex: 192.168.1.42", cfg.host, input, sizeof(input), 15)) {
                    if (valid_host(input)) { snprintf(cfg.host, sizeof(cfg.host), "%s", input); dirty = true; snprintf(notice, sizeof(notice), "IP alterado. Pressione + para salvar."); }
                    else snprintf(notice, sizeof(notice), "IPv4 invalido.");
                }
            } else if (selected == 1) {
                char old[8], input[16] = "";
                snprintf(old, sizeof(old), "%u", cfg.port);
                if (prompt("Porta UDP: 1 ate 65535", old, input, sizeof(input), 5)) {
                    char *tail;
                    long port = strtol(input, &tail, 10);
                    if (*input && !*tail && port >= 1 && port <= 65535) { cfg.port = (unsigned)port; dirty = true; snprintf(notice, sizeof(notice), "Porta alterada. Pressione + para salvar."); }
                    else snprintf(notice, sizeof(notice), "Porta invalida.");
                }
            } else if (selected == 2) {
                cfg.enabled = !cfg.enabled;
                dirty = true;
                snprintf(notice, sizeof(notice), "Pressione + para aplicar a mudanca.");
            } else if (selected == 3) {
                snprintf(notice, sizeof(notice), "Testando conexao com o PC...");
                draw(selected); consoleUpdate(NULL);
                test_connection();
            } else toggle_autostart();
        }
        if (down || frame++ % 60 == 0) draw(selected);
        consoleUpdate(NULL);
    }
    if (nifm_ready) nifmExit();
    if (sockets_ready) socketExit();
    consoleExit(NULL);
    return 0;
}

