#pragma once

#include <arpa/inet.h>
#include <sys/socket.h>
#include <switch.h>

/* Only scan at most the local /24; never scan routed public addresses. */
static bool discovery_subnet(u32 *local, u32 *mask) {
    u32 ip = 0, netmask = 0, gateway = 0, dns1 = 0, dns2 = 0;
    Result rc = nifmGetCurrentIpConfigInfo(&ip, &netmask, &gateway, &dns1, &dns2);
    if (R_FAILED(rc) || !ip || !netmask) return false;
    u32 hostmask = ntohl(netmask);
    if ((hostmask | (hostmask - 1)) != 0xffffffffU) return false;
    if (hostmask < 0xffffff00U) netmask = htonl(0xffffff00U);
    *local = ntohl(ip); *mask = ntohl(netmask);
    return true;
}

static void discovery_send_candidates(int fd, unsigned port, const char *packet, size_t len, bool sweep) {
    struct sockaddr_in dst = {0}; dst.sin_family = AF_INET; dst.sin_port = htons((u16)port);
    dst.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    sendto(fd, packet, len, 0, (struct sockaddr *)&dst, sizeof(dst));
    u32 ip, mask;
    if (!discovery_subnet(&ip, &mask)) return;
    u32 network = ip & mask, broadcast = network | ~mask;
    if (broadcast > network + 1) {
        dst.sin_addr.s_addr = htonl(broadcast);
        sendto(fd, packet, len, 0, (struct sockaddr *)&dst, sizeof(dst));
    }
    if (!sweep) return;
    for (u32 address = network + 1; address < broadcast; ++address) {
        if (address == ip) continue;
        dst.sin_addr.s_addr = htonl(address);
        sendto(fd, packet, len, 0, (struct sockaddr *)&dst, sizeof(dst));
    }
}
