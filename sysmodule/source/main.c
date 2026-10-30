#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <switch.h>

#define CONFIG_PATH "sdmc:/config/switch-richpresence.ini"
#define STATUS_PATH "sdmc:/config/switch-richpresence.status"
#define HEARTBEAT_SECONDS 90

u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 1;
void __libnx_initheap(void) {
    static u8 heap[0x80000];
    extern void *fake_heap_start, *fake_heap_end;
    fake_heap_start = heap;
    fake_heap_end = heap + sizeof(heap);
}
void __appInit(void) {
    Result rc = smInitialize();
    if (R_FAILED(rc)) diagAbortWithResult(rc);
    if (R_SUCCEEDED(setsysInitialize())) {
        SetSysFirmwareVersion fw;
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw)))
            hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
        setsysExit();
    }
}
void __appExit(void) { smExit(); }

typedef struct { char host[64]; unsigned port; bool enabled; } Config;
typedef struct { int fd; bool socket_ready, nifm_ready, request_open, registered; NifmRequest request; } Network;

static void status(const char *message) {
    static char previous[160];
    if (!strcmp(previous, message)) return;
    snprintf(previous, sizeof(previous), "%s", message);
    FILE *f = fopen(STATUS_PATH, "w");
    if (f) { fprintf(f, "%s\n", message); fclose(f); }
}

static bool load_config(Config *out) {
    Config cfg = {.port = 39421, .enabled = true};
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) return false;
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
        if (!strcmp(key, "host")) snprintf(cfg.host, sizeof(cfg.host), "%s", eq);
        else if (!strcmp(key, "port")) {
            char *tail;
            long port = strtol(eq, &tail, 10);
            if (*eq == 0 || *tail != 0 || port < 1 || port > 65535) { fclose(f); return false; }
            cfg.port = (unsigned)port;
        } else if (!strcmp(key, "enabled")) {
            if (!strcmp(eq, "0")) cfg.enabled = false;
            else if (!strcmp(eq, "1")) cfg.enabled = true;
            else { fclose(f); return false; }
        }
    }
    fclose(f);
    struct in_addr address;
    if (inet_pton(AF_INET, cfg.host, &address) != 1 || address.s_addr == INADDR_ANY) return false;
    *out = cfg;
    return true;
}

static void network_close(Network *n) {
    if (n->request_open) {
        NifmRequestState state;
        if (n->registered && R_SUCCEEDED(nifmGetRequestState(&n->request, &state)) &&
            state == NifmRequestState_Available)
            socketNifmRequestUnregisterSocketDescriptor(&n->request, n->fd);
        nifmRequestCancel(&n->request);
        nifmRequestClose(&n->request);
    }
    if (n->nifm_ready) nifmExit();
    if (n->fd >= 0) close(n->fd);
    if (n->socket_ready) socketExit();
    memset(n, 0, sizeof(*n));
    n->fd = -1;
}

static bool network_start(Network *n) {
    SocketInitConfig config = *socketGetDefaultInitConfig();
    // Apenas pequenos pacotes UDP: o padrao de TCP exige mais que o heap do modulo.
    config.tcp_tx_buf_size = 0x1000;
    config.tcp_rx_buf_size = 0x1000;
    config.tcp_tx_buf_max_size = 0;
    config.tcp_rx_buf_max_size = 0;
    config.sb_efficiency = 1;
    config.num_bsd_sessions = 1;
    Result rc = socketInitialize(&config);
    if (R_FAILED(rc)) {
        char message[160];
        snprintf(message, sizeof(message), "Erro inicializar sockets: 0x%08lX", (unsigned long)rc);
        status(message);
        return false;
    }
    n->socket_ready = true;
    n->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (n->fd < 0) { status("Erro: criar socket UDP"); network_close(n); return false; }
    rc = nifmInitialize(NifmServiceType_User);
    if (R_FAILED(rc)) { status("Erro: inicializar NIFM"); network_close(n); return false; }
    n->nifm_ready = true;
    rc = nifmCreateRequest(&n->request, true);
    if (R_FAILED(rc)) { status("Erro: criar pedido de rede"); network_close(n); return false; }
    n->request_open = true;
    rc = nifmRequestSubmit(&n->request);
    if (R_FAILED(rc)) { status("Erro: solicitar rede"); network_close(n); return false; }
    status("Aguardando rede NIFM");
    return true;
}

static bool send_event(Network *n, const Config *cfg, u64 title_id, bool paused) {
    struct sockaddr_in destination = {0};
    destination.sin_family = AF_INET;
    destination.sin_port = htons((uint16_t)cfg->port);
    if (inet_pton(AF_INET, cfg->host, &destination.sin_addr) != 1) return false;
    char packet[128];
    int length = snprintf(packet, sizeof(packet), "{\"title_id\":\"%016llX\",\"state\":\"%s\"}",
                          (unsigned long long)title_id, paused ? "paused" : title_id ? "playing" : "idle");
    return sendto(n->fd, packet, (size_t)length, 0, (struct sockaddr *)&destination, sizeof(destination)) == length;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    while (R_FAILED(fsInitialize())) svcSleepThread(5000000000ULL);
    while (R_FAILED(fsdevMountSdmc())) svcSleepThread(5000000000ULL);
    status("Iniciando sysmodule");
    Network net = {.fd = -1};
    Config active = {0};
    bool have_config = false, shell_ready = false, info_ready = false;
    u64 last_title = UINT64_MAX, last_send_tick = 0;

    while (true) {
        Config current;
        if (!load_config(&current)) {
            status("Configuracao ausente ou invalida");
            svcSleepThread(2000000000ULL);
            continue;
        }
        bool changed = !have_config || strcmp(current.host, active.host) || current.port != active.port || current.enabled != active.enabled;
        if (changed) {
            if (have_config && active.enabled && net.registered && (!current.enabled || strcmp(current.host, active.host) || current.port != active.port))
                send_event(&net, &active, 0, true);
            active = current;
            have_config = true;
            last_title = UINT64_MAX;
            last_send_tick = 0;
        }
        if (!active.enabled) {
            if (net.socket_ready) network_close(&net);
            status("Presenca pausada na configuracao");
            svcSleepThread(2000000000ULL);
            continue;
        }
        Result service_rc = 0;
        if (!shell_ready) { service_rc = pmshellInitialize(); shell_ready = R_SUCCEEDED(service_rc); }
        if (shell_ready && !info_ready) { service_rc = pminfoInitialize(); info_ready = R_SUCCEEDED(service_rc); }
        if (!shell_ready || !info_ready) {
            char message[160];
            snprintf(message, sizeof(message), "Erro %s: 0x%08lX", shell_ready ? "pm:info" : "pm:shell", (unsigned long)service_rc);
            status(message);
            svcSleepThread(5000000000ULL);
            continue;
        }
        if (!net.socket_ready && !network_start(&net)) {
            svcSleepThread(5000000000ULL);
            continue;
        }
        NifmRequestState state;
        if (R_FAILED(nifmGetRequestState(&net.request, &state))) {
            status("Erro: consultar rede; tentando novamente");
            network_close(&net);
            svcSleepThread(5000000000ULL);
            continue;
        }
        if (state != NifmRequestState_Available) {
            if (net.registered || state == NifmRequestState_Invalid || state == NifmRequestState_Unknown4) network_close(&net);
            else nifmRequestSubmit(&net.request);
            status("Aguardando rede NIFM");
            svcSleepThread(5000000000ULL);
            continue;
        }
        if (!net.registered) {
            if (socketNifmRequestRegisterSocketDescriptor(&net.request, net.fd) != 0) {
                status("Erro: registrar socket na rede");
                network_close(&net);
                svcSleepThread(5000000000ULL);
                continue;
            }
            net.registered = true;
            last_send_tick = 0;
        }
        u64 pid = 0, program_id = 0, title_id = 0;
        Result rc = pmshellGetApplicationProcessIdForShell(&pid);
        // PM retorna ProcessNotFound quando nao ha aplicacao aberta.
        if (R_FAILED(rc) && rc != 0x20F) {
            char message[160];
            snprintf(message, sizeof(message), "Erro consultar processo: 0x%08lX", (unsigned long)rc);
            status(message);
            svcSleepThread(2000000000ULL);
            continue;
        }
        if (R_SUCCEEDED(rc) && pid) {
            rc = pminfoGetProgramId(&program_id, pid);
            if (R_FAILED(rc)) {
                char message[160];
                snprintf(message, sizeof(message), "Erro consultar ID: 0x%08lX", (unsigned long)rc);
                status(message);
                svcSleepThread(2000000000ULL);
                continue;
            }
            title_id = program_id & ~0xFFFULL;
        }
        u64 now = armGetSystemTick();
        if (title_id != last_title || !last_send_tick || armTicksToNs(now - last_send_tick) >= HEARTBEAT_SECONDS * 1000000000ULL) {
            if (send_event(&net, &active, title_id, false)) {
                last_title = title_id;
                last_send_tick = now;
                char message[160];
                snprintf(message, sizeof(message), "UDP enviado: %016llX para %s:%u", (unsigned long long)title_id, active.host, active.port);
                status(message);
            } else status("Erro: enviar UDP; tentando novamente");
        }
        svcSleepThread(2000000000ULL);
    }
}
