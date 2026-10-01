#include "stm_esp_hosted_dhcps.h"
#include "lwip/ip.h"
#include "lwip/pbuf.h"
#include "lwip/sys.h"
#include "lwip/etharp.h"
#include <string.h>

#define DHCP_PORT_SERVER 67U
#define DHCP_PORT_CLIENT 68U
#define DHCP_SIZE 300U
#define DHCP_LEASE_MS 3600000UL
#define DHCP_OFFER_MS 60000UL

static void put32(uint8_t *p, uint32_t n)
{
    p[0] = (uint8_t)(n >> 24U); p[1] = (uint8_t)(n >> 16U);
    p[2] = (uint8_t)(n >> 8U); p[3] = (uint8_t)n;
}
static void put_ip(uint8_t *p, const ip4_addr_t *ip)
{
    p[0] = ip4_addr1(ip); p[1] = ip4_addr2(ip);
    p[2] = ip4_addr3(ip); p[3] = ip4_addr4(ip);
}
static void dhcps_receive(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                          const ip_addr_t *sender, u16_t port)
{
    esp_hosted_lwip_dhcps_t *s = (esp_hosted_lwip_dhcps_t *)arg;
    uint8_t in[576], out[DHCP_SIZE] = {0};
    uint8_t type = 0, requested[4] = {0}, server_id[4] = {0};
    uint8_t has_requested = 0U, has_server_id = 0U;
    (void)sender;
    if (!p) { return; }
    if (!s || !s->ap_netif || ip_current_input_netif() != s->ap_netif ||
        !netif_is_link_up(s->ap_netif) || port != DHCP_PORT_CLIENT ||
        p->tot_len < 240U || p->tot_len > sizeof(in) ||
        pbuf_copy_partial(p, in, p->tot_len, 0U) != p->tot_len) { goto done; }
    size_t size = p->tot_len;
    if (in[0] != 1U || in[1] != 1U || in[2] != 6U || in[3] != 0U ||
        in[236] != 99U || in[237] != 130U || in[238] != 83U || in[239] != 99U) { goto done; }
    for (size_t pos = 240U; pos < size;) {
        uint8_t tag = in[pos++];
        if (tag == 255U) { break; }
        if (tag == 0U) { continue; }
        if (pos >= size || in[pos] > size - pos - 1U) { goto done; }
        uint8_t len = in[pos++];
        if (tag == 53U && len == 1U) { type = in[pos]; }
        if (tag == 50U && len == 4U) { memcpy(requested, in + pos, 4U); has_requested = 1U; }
        if (tag == 54U && len == 4U) { memcpy(server_id, in + pos, 4U); has_server_id = 1U; }
        pos += len;
    }
    uint8_t ip[4]; put_ip(ip, netif_ip4_addr(s->ap_netif));
    if (has_server_id && memcmp(ip, server_id, 4U)) { goto done; }
    unsigned slot = 4U;
    uint32_t now = sys_now();
    for (unsigned i = 0U; i < 4U; ++i) {
        if (s->leases[i].last_octet && !memcmp(s->leases[i].mac, in + 28U, 6U) &&
            (int32_t)(now - s->leases[i].expires_at) < 0) {
            slot = i; break;
        }
    }
    if (type == 7U) { /* DHCPRELEASE: only its current owner can release the slot. */
        if (slot < 4U && in[15] == s->leases[slot].last_octet) {
            s->leases[slot].last_octet = 0U;
        }
        goto done;
    }
    if (type != 1U && type != 3U) { goto done; }
    const uint8_t *wanted = has_requested ? requested : in + 12U;
    if (slot == 4U && wanted[0] == ip[0] && wanted[1] == ip[1] &&
        wanted[2] == ip[2] && wanted[3] >= 100U && wanted[3] <= 103U) {
        unsigned candidate = wanted[3] - 100U;
        if (!s->leases[candidate].last_octet ||
            (int32_t)(now - s->leases[candidate].expires_at) >= 0) { slot = candidate; }
    }
    if (slot == 4U && type == 1U) {
        for (unsigned i = 0U; i < 4U; ++i) {
            if (!s->leases[i].last_octet ||
                (int32_t)(now - s->leases[i].expires_at) >= 0) { slot = i; break; }
        }
    }
    /* A request for another client's live lease must never evict it. */
    if (slot == 4U) { goto done; }
    uint8_t assigned[4] = {ip[0], ip[1], ip[2], (uint8_t)(100U + slot)};
    uint8_t response_type = (type == 1U) ? 2U : 5U;
    if (type == 3U && memcmp(wanted, assigned, 4U)) {
        response_type = 6U; /* DHCPNAK: request is outside our pool. */
    }
    out[0] = 2U; out[1] = 1U; out[2] = 6U;
    memcpy(out + 4U, in + 4U, 8U); /* transaction id, seconds, broadcast flag */
    memcpy(out + 28U, in + 28U, 6U);
    if (response_type != 6U) { memcpy(out + 16U, assigned, 4U); }
    memcpy(out + 20U, ip, 4U);
    out[236] = 99U; out[237] = 130U; out[238] = 83U; out[239] = 99U;
    size_t offset = 240U;
    out[offset++] = 53U; out[offset++] = 1U; out[offset++] = response_type;
    out[offset++] = 54U; out[offset++] = 4U; memcpy(out + offset, ip, 4U); offset += 4U;
    if (response_type != 6U) {
        out[offset++] = 51U; out[offset++] = 4U; put32(out + offset, DHCP_LEASE_MS / 1000U); offset += 4U;
        out[offset++] = 1U; out[offset++] = 4U;
        put_ip(out + offset, netif_ip4_netmask(s->ap_netif)); offset += 4U;
        out[offset++] = 3U; out[offset++] = 4U; memcpy(out + offset, ip, 4U); offset += 4U;
    }
    out[offset++] = 255U;
    /* BOOTP packets must be at least 300 bytes long. */
    offset = DHCP_SIZE;
    struct pbuf *reply = pbuf_alloc(PBUF_TRANSPORT, (u16_t)offset, PBUF_RAM);
    if (reply) {
        ip_addr_t broadcast, source;
        IP_ADDR4(&broadcast, 255, 255, 255, 255);
        ip_addr_copy_from_ip4(source, *netif_ip4_addr(s->ap_netif));
        if (pbuf_take(reply, out, (u16_t)offset) == ERR_OK &&
            udp_sendto_if_src(pcb, reply, &broadcast, DHCP_PORT_CLIENT,
                              s->ap_netif, &source) == ERR_OK && response_type != 6U) {
            memcpy(s->leases[slot].mac, in + 28U, 6U);
            s->leases[slot].last_octet = assigned[3];
            s->leases[slot].expires_at = now + (response_type == 2U ? DHCP_OFFER_MS : DHCP_LEASE_MS);
        }
        pbuf_free(reply);
    }
done:
    pbuf_free(p);
}
stm_err_t esp_hosted_lwip_dhcps_start(esp_hosted_lwip_dhcps_t *s, struct netif *ap)
{
    if (!s || !ap || s->pcb ||
        ip4_addr1(netif_ip4_addr(ap)) == 0U ||
        !ip4_addr4(netif_ip4_addr(ap)) || ip4_addr4(netif_ip4_addr(ap)) == 255U ||
        ip4_addr4(netif_ip4_netmask(ap)) != 0U ||
        ip4_addr1(netif_ip4_netmask(ap)) != 255U ||
        ip4_addr2(netif_ip4_netmask(ap)) != 255U ||
        ip4_addr3(netif_ip4_netmask(ap)) != 255U) { return STM_ERR_INVALID_ARG; }
    if (ip4_addr4(netif_ip4_addr(ap)) >= 100U &&
        ip4_addr4(netif_ip4_addr(ap)) <= 103U) { return STM_ERR_INVALID_CONFIG; }
    memset(s, 0, sizeof(*s));
    s->ap_netif = ap;
    s->pcb = udp_new_ip_type(IPADDR_TYPE_V4);
    if (!s->pcb) { s->ap_netif = NULL; return STM_ERR_NO_MEM; }
    udp_bind_netif(s->pcb, ap);
    if (udp_bind(s->pcb, IP_ANY_TYPE, DHCP_PORT_SERVER) != ERR_OK) {
        udp_remove(s->pcb); s->pcb = NULL; s->ap_netif = NULL;
        return STM_ERR_IO;
    }
    udp_recv(s->pcb, dhcps_receive, s);
    return STM_OK;
}
void esp_hosted_lwip_dhcps_stop(esp_hosted_lwip_dhcps_t *s)
{
    if (!s) { return; }
    if (s->pcb) { udp_remove(s->pcb); }
    if (s->ap_netif) { etharp_cleanup_netif(s->ap_netif); }
    memset(s, 0, sizeof(*s));
}

stm_err_t esp_hosted_lwip_dhcps_update(esp_hosted_lwip_dhcps_t *server,
                                       struct netif *ap_netif)
{
    if (!server || !ap_netif || (server->ap_netif && server->ap_netif != ap_netif)) {
        return STM_ERR_INVALID_ARG;
    }
    if (netif_is_link_up(ap_netif)) {
        if (!server->pcb) { return esp_hosted_lwip_dhcps_start(server, ap_netif); }
    } else if (server->pcb) { esp_hosted_lwip_dhcps_stop(server); }
    return STM_OK;
}
