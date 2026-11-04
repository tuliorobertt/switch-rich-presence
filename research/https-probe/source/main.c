#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <switch.h>

/* An isolated, unauthenticated read-only connectivity experiment. */
#define HOST "discord.com"
static const char request[] = "GET /api/v10/gateway HTTP/1.1\r\n"
                              "Host: " HOST "\r\n"
                              "User-Agent: SwitchRichPresenceResearch/1.0\r\n"
                              "Connection: close\r\n\r\n";

static void result_error(const char *stage, Result rc) {
    printf("FALHA em %s: 0x%08lX\n", stage, (unsigned long)rc);
}

static void run_probe(void) {
    bool nifm = false, sockets = false, ssl = false, context = false, connection = false;
    int fd = -1, ssl_fd = -1;
    struct addrinfo *addresses = NULL;
    SslContext ctx = {0};
    SslConnection conn = {0};
    Result rc;
    printf("\nIniciando teste de rede...\n"); consoleUpdate(NULL);
    rc = nifmInitialize(NifmServiceType_User);
    if (R_FAILED(rc)) { result_error("NIFM", rc); goto done; }
    nifm = true;
    SocketInitConfig config = *socketGetDefaultInitConfig();
    config.num_bsd_sessions = 1;
    rc = socketInitialize(&config);
    if (R_FAILED(rc)) { result_error("sockets", rc); goto done; }
    sockets = true;
    printf("Resolvendo " HOST "...\n"); consoleUpdate(NULL);
    struct addrinfo hints = {0};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int dns = getaddrinfo(HOST, "443", &hints, &addresses);
    if (dns || !addresses) { printf("FALHA no DNS: %d\n", dns); goto done; }
    for (struct addrinfo *address = addresses; address; address = address->ai_next) {
        fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd < 0) continue;
        struct timeval timeout = {.tv_sec = 10, .tv_usec = 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        if (connect(fd, address->ai_addr, address->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    if (fd < 0) { printf("FALHA ao conectar TCP (errno %d)\n", errno); goto done; }
    printf("TCP conectado. Verificando TLS...\n"); consoleUpdate(NULL);
    rc = sslInitialize(1);
    if (R_FAILED(rc)) { result_error("SSL", rc); goto done; }
    ssl = true;
    rc = sslCreateContext(&ctx, SslVersion_TlsV12 | SslVersion_TlsV13);
    if (R_FAILED(rc)) { result_error("contexto TLS", rc); goto done; }
    context = true;
    rc = sslContextCreateConnection(&ctx, &conn);
    if (R_FAILED(rc)) { result_error("conexao TLS", rc); goto done; }
    connection = true;
    /* Keep the OS defaults: both certificate chain and hostname are verified. */
    rc = sslConnectionSetHostName(&conn, HOST, sizeof(HOST));
    if (R_FAILED(rc)) { result_error("hostname TLS", rc); goto done; }
    errno = 0;
    ssl_fd = socketSslConnectionSetSocketDescriptor(&conn, fd);
    if (ssl_fd < 0 && errno != ENOENT) {
        printf("FALHA ao associar socket (errno %d; resultado 0x%08lX)\n", errno, (unsigned long)socketGetLastResult());
        goto done;
    }
    rc = sslConnectionDoHandshake(&conn, NULL, NULL, NULL, 0);
    if (R_FAILED(rc)) { result_error("verificacao TLS", rc); goto done; }
    printf("TLS validado. Consultando API publica...\n"); consoleUpdate(NULL);
    u32 sent = 0;
    while (sent < sizeof(request) - 1) {
        u32 written = 0;
        rc = sslConnectionWrite(&conn, request + sent, sizeof(request) - 1 - sent, &written);
        if (R_FAILED(rc)) { result_error("envio HTTP", rc); goto done; }
        if (!written) { printf("FALHA: envio HTTP sem progresso.\n"); goto done; }
        sent += written;
    }
    char response[2049];
    u32 total = 0;
    while (total < sizeof(response) - 1) {
        u32 count = 0;
        rc = sslConnectionRead(&conn, response + total, sizeof(response) - 1 - total, &count);
        if (R_FAILED(rc)) { result_error("resposta HTTP", rc); goto done; }
        if (!count) break;
        total += count; response[total] = 0;
        if (strstr(response, "\r\n")) break;
    }
    response[total] = 0;
    char *end = strstr(response, "\r\n");
    if (end) *end = 0;
    unsigned http_status = 0;
    if (sscanf(response, "HTTP/%*u.%*u %u", &http_status) != 1 || http_status < 100 || http_status > 599) {
        printf("Resposta HTTP invalida.\n"); goto done;
    }
    printf("Resposta: %.120s\n", response);
    printf("Teste de conexao concluido.\n");
    printf("Isto nao autentica nem publica presenca.\n");
done:
    if (ssl_fd >= 0 && ssl_fd != fd) close(ssl_fd);
    if (connection) sslConnectionClose(&conn);
    if (context) sslContextClose(&ctx);
    if (ssl) sslExit();
    if (fd >= 0) close(fd);
    if (addresses) freeaddrinfo(addresses);
    if (sockets) socketExit();
    if (nifm) nifmExit();
    printf("\nA: testar novamente   -: sair\n");
    consoleUpdate(NULL);
}

int main(void) {
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad; padInitializeDefault(&pad);
    printf("SWITCH RICH PRESENCE\nTeste experimental HTTPS\n\n");
    printf("Requer internet no Switch. Nao envia credenciais.\n");
    printf("A: testar conexao   -: sair\n");
    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad);
        if (down & HidNpadButton_Minus) break;
        if (down & HidNpadButton_A) run_probe();
        consoleUpdate(NULL);
    }
    consoleExit(NULL);
    return 0;
}
