#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <switch.h>
#include "../../source/discovery.h"

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

static char status_game[384];

/* Line 1: human readable state. Line 2: name of the running game (may be empty). */
static void status(const char *message) {
    static char previous[160], previous_game[384];
    if (!strcmp(previous, message) && !strcmp(previous_game, status_game)) return;
    snprintf(previous, sizeof(previous), "%s", message);
    snprintf(previous_game, sizeof(previous_game), "%s", status_game);
    FILE *f = fopen(STATUS_PATH, "w");
    if (f) { fprintf(f, "%s\n%s\n", message, status_game); fclose(f); }
}

static bool load_config(Config *out) {
    Config cfg = {.host = "auto", .port = 39421, .enabled = true};
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) { *out = cfg; return true; }
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
    if (strcmp(cfg.host, "auto") && (inet_pton(AF_INET, cfg.host, &address) != 1 || address.s_addr == INADDR_ANY)) return false;
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
    int broadcast = 1;
    if (setsockopt(n->fd, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast)) != 0) { status("Erro: habilitar busca na rede"); network_close(n); return false; }
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

/* A ping response proves that a companion currently listens at this address.
   UDP is unauthenticated; discovery is only suitable for trusted LANs. */
static void discovery_send(Network *n, unsigned port, u64 nonce, bool sweep, const char *known_host) {
    char packet[112];
    int len = snprintf(packet, sizeof(packet), "{\"type\":\"ping\",\"nonce\":\"%016llX\",\"discovery\":true}", (unsigned long long)nonce);
    if (known_host[0]) {
        struct sockaddr_in dst = {0}; dst.sin_family = AF_INET; dst.sin_port = htons((u16)port);
        if (inet_pton(AF_INET, known_host, &dst.sin_addr) == 1)
            sendto(n->fd, packet, len, 0, (struct sockaddr *)&dst, sizeof(dst));
        return;
    }
    discovery_send_candidates(n->fd, port, packet, len, sweep);
}

static bool discovery_receive(Network *n, unsigned port, u64 nonce, char *host, size_t host_size) {
    fd_set set; FD_ZERO(&set); FD_SET(n->fd, &set); struct timeval timeout = {0};
    if (select(n->fd+1, &set, NULL, NULL, &timeout) <= 0) return false;
    struct sockaddr_in from = {0}; socklen_t from_len = sizeof(from); char reply[128], expected[96];
    ssize_t len = recvfrom(n->fd, reply, sizeof(reply)-1, 0, (struct sockaddr *)&from, &from_len);
    if (len <= 0) return false;
    reply[len] = 0;
    snprintf(expected, sizeof(expected), "{\"type\":\"pong\",\"nonce\":\"%016llX\"}", (unsigned long long)nonce);
    if (from.sin_family != AF_INET || from.sin_port != htons((u16)port) || strcmp(reply, expected)) return false;
    return inet_ntop(AF_INET, &from.sin_addr, host, host_size) != NULL;
}

/* Reads the game name from the console (control.nacp). This covers titles
   missing from the PC catalogs. ns:am2 is opened only while a title changes. */
static void read_title_name(u64 title_id, char *out, size_t size) {
    static NsApplicationControlData control;
    out[0] = 0;
    if (!title_id || R_FAILED(nsInitialize())) return;
    u64 actual = 0;
    Result rc = nsGetApplicationControlData(NsApplicationControlSource_Storage, title_id, &control, sizeof(control), &actual);
    nsExit();
    if (R_FAILED(rc) || actual < sizeof(control.nacp)) return;
    /* English names match the PC catalogs best; otherwise use the first one. */
    const char *name = control.nacp.lang[0].name;
    for (int i = 0; !name[0] && i < 16; ++i) name = control.nacp.lang[i].name;
    size_t j = 0;
    for (size_t i = 0; name[i] && i < sizeof(control.nacp.lang[0].name) && j + 1 < size; ++i)
        out[j++] = (unsigned char)name[i] < 0x20 ? ' ' : name[i];
    /* Do not leave a truncated UTF-8 sequence at the end. */
    size_t k = j;
    while (k > 0 && ((unsigned char)out[k-1] & 0xC0) == 0x80) --k;
    if (k > 0 && (unsigned char)out[k-1] >= 0xC0) {
        unsigned char lead = (unsigned char)out[k-1];
        size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
        if (j - (k - 1) < need) j = k - 1;
    }
    out[j] = 0;
}

static void json_escape(const char *in, char *out, size_t size) {
    size_t j = 0;
    for (; *in && j + 2 < size; ++in) {
        if (*in == '"' || *in == '\\') out[j++] = '\\';
        out[j++] = *in;
    }
    out[j] = 0;
}

static bool send_event(Network *n, const Config *cfg, u64 title_id, bool paused, const char *title) {
    struct sockaddr_in destination = {0};
    destination.sin_family = AF_INET;
    destination.sin_port = htons((uint16_t)cfg->port);
    if (inet_pton(AF_INET, cfg->host, &destination.sin_addr) != 1) return false;
    char packet[900], escaped[780];
    int length;
    if (!paused && title_id && title && title[0]) {
        json_escape(title, escaped, sizeof(escaped));
        length = snprintf(packet, sizeof(packet), "{\"title_id\":\"%016llX\",\"state\":\"playing\",\"title\":\"%s\"}",
                          (unsigned long long)title_id, escaped);
    }
    else
        length = snprintf(packet, sizeof(packet), "{\"title_id\":\"%016llX\",\"state\":\"%s\"}",
                          (unsigned long long)title_id, paused ? "paused" : title_id ? "playing" : "idle");
    if (length <= 0 || (size_t)length >= sizeof(packet)) return false;
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
    u64 discovery_tick = 0, discovery_nonce = 0, last_reply = 0, sweep_tick = 0;
    char found_host[64] = {0};
    u64 named_title = 0;

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
                send_event(&net, &active, 0, true, NULL);
            active = current;
            have_config = true;
            last_title = UINT64_MAX;
            last_send_tick = 0;
            found_host[0] = 0; discovery_tick = last_reply = sweep_tick = 0;
        }
        if (!active.enabled) {
            if (net.socket_ready) network_close(&net);
            status_game[0] = 0;
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
            found_host[0] = 0; discovery_tick = last_reply = sweep_tick = 0;
        }
        u64 now = armGetSystemTick();
        Config destination = active;
        if (!strcmp(active.host, "auto")) {
            char reply_host[64];
            if (discovery_tick && discovery_receive(&net, active.port, discovery_nonce, reply_host, sizeof(reply_host))) {
                if (!found_host[0] || !strcmp(found_host, reply_host)) {
                    if (strcmp(found_host, reply_host)) { snprintf(found_host, sizeof(found_host), "%s", reply_host); last_send_tick = 0; }
                    last_reply = now;
                }
            }
            if (found_host[0] && last_reply && armTicksToNs(now-last_reply) > 30000000000ULL) {
                found_host[0] = 0; last_reply = 0; last_send_tick = 0;
            }
            if (!discovery_tick || armTicksToNs(now-discovery_tick) >= 10000000000ULL) {
                discovery_nonce = armGetSystemTick(); discovery_tick = now;
                bool sweep = !found_host[0] && (!sweep_tick || armTicksToNs(now-sweep_tick) >= 60000000000ULL);
                if (sweep) sweep_tick = now;
                discovery_send(&net, active.port, discovery_nonce, sweep, found_host);
            }
            if (!found_host[0]) { status("Procurando companion na rede local"); svcSleepThread(2000000000ULL); continue; }
            snprintf(destination.host, sizeof(destination.host), "%s", found_host);
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
        if (title_id != named_title) {
            read_title_name(title_id, status_game, sizeof(status_game));
            named_title = title_id;
        }
        if (title_id != last_title || !last_send_tick || armTicksToNs(now - last_send_tick) >= HEARTBEAT_SECONDS * 1000000000ULL) {
            if (send_event(&net, &destination, title_id, false, status_game)) {
                last_title = title_id;
                last_send_tick = now;
                char message[160];
                snprintf(message, sizeof(message), "UDP enviado: %016llX para %s:%u", (unsigned long long)title_id, destination.host, destination.port);
                status(message);
            } else status("Erro: enviar UDP; tentando novamente");
        }
        svcSleepThread(2000000000ULL);
    }
}
