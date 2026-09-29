#include "stm_esp_hosted_lwip.h"
#include "stm_esp_hosted_private.h"
#include "hal_stub.h"
#include "lwip/pbuf.h"
#include "lwip/dhcp.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); return 1; } } while (0)
static unsigned received;
static uint8_t received_frame[1514];
static err_t input(struct pbuf *p, struct netif *n)
{
    (void)n;
    received = p->tot_len;
    pbuf_copy_partial(p, received_frame, p->tot_len, 0);
    pbuf_free(p);
    return ERR_OK;
}
int main(void)
{
    static uint8_t tx[1600] __attribute__((aligned(32)));
    static uint8_t rx[1600] __attribute__((aligned(32)));
    static SPI_HandleTypeDef spi;
    static GPIO_TypeDef gpio;
    uint8_t mac[6] = {2, 3, 4, 5, 6, 7};
    esp_hosted_config_t cfg = {
        .spi=&spi, .cs_port=&gpio, .cs_pin=1, .reset_port=&gpio, .reset_pin=2,
        .handshake_port=&gpio, .handshake_pin=3, .data_ready_port=&gpio, .data_ready_pin=4,
        .tx_buffer=tx, .rx_buffer=rx, .buffer_size=1600, .checksum_enabled=1,
        .transfer_timeout_ms=10,
    };
    esp_hosted_handle_t h=NULL;
    esp_hosted_lwip_t a={0};
    struct netif n={0}, ap={0};
    uint8_t frame[1600], payload[1514];
    CHECK(esp_hosted_create(&cfg,&h)==STM_OK);
    CHECK(esp_hosted_lwip_prepare(&a,h,&n,mac)==STM_OK);
    n.state=&a; n.input=input;
    CHECK(esp_hosted_lwip_netif_init(&n)==ERR_OK);
    CHECK(n.hwaddr_len==6 && memcmp(n.hwaddr,mac,6)==0 && n.mtu==1500);
    CHECK(esp_hosted_lwip_attach(&a)==STM_OK);
    CHECK(esp_hosted_lwip_ap_prepare(&a,&ap,mac)==STM_OK);
    ap.state=&a; ap.input=input;
    CHECK(esp_hosted_lwip_ap_netif_init(&ap)==ERR_OK);
    CHECK(ap.hwaddr_len==6 && memcmp(ap.hwaddr,mac,6)==0 && ap.mtu==1500);
    CHECK(esp_hosted_lwip_ap_attach(&a)==STM_OK);
    h->wifi_mode=EH_WIFI_MODE_APSTA; h->wifi_started=1;
    /* CP Wi-Fi AP start (event 773, WIFI_EVENT_AP_START=12). */
    const uint8_t ap_up[]={1,0,0,2,10,0,8,3,16,0x85,6,0xAA,0x30,2,16,12};
    CHECK(esp_hosted_encode_frame(h,3,0,1,ap_up,sizeof(ap_up),frame,sizeof(frame))==STM_OK);
    test_hal_inject_rx(frame,sizeof(frame));
    CHECK(esp_hosted_poll(h)==STM_OK && netif_is_link_up(&ap));
    /* RPC event 775: host STA link up. */
    const uint8_t event[]={1,0,0,2,10,0,8,3,16,0x87,6,0xBA,0x30,2,0x12,0};
    CHECK(esp_hosted_encode_frame(h,3,0,1,event,sizeof(event),frame,sizeof(frame))==STM_OK);
    test_hal_inject_rx(frame,sizeof(frame));
    CHECK(esp_hosted_poll(h)==STM_OK && netif_is_link_up(&n));
    CHECK(esp_hosted_lwip_sta_update(&a)==STM_OK && a.dhcp_running && mock_dhcp_starts==1);
    CHECK(esp_hosted_lwip_sta_update(&a)==STM_OK && mock_dhcp_starts==1);
    CHECK(!esp_hosted_lwip_sta_has_address(&a));
    n.ip_addr=(ip4_addr_t){{192,168,1,80}};
    CHECK(esp_hosted_lwip_sta_has_address(&a));
    for (unsigned i=0;i<sizeof(payload);++i) { payload[i]=(uint8_t)i; }
    CHECK(esp_hosted_encode_frame(h,1,0,1,payload,sizeof(payload),frame,sizeof(frame))==STM_OK);
    test_hal_inject_rx(frame,sizeof(frame));
    CHECK(esp_hosted_poll(h)==STM_OK && received==sizeof(payload));
    CHECK(memcmp(received_frame,payload,sizeof(payload))==0);
    received=0;
    CHECK(esp_hosted_encode_frame(h,ESP_HOSTED_AP_IF_TYPE,0,1,payload,sizeof(payload),frame,sizeof(frame))==STM_OK);
    test_hal_inject_rx(frame,sizeof(frame));
    CHECK(esp_hosted_poll(h)==STM_OK && received==sizeof(payload));
    CHECK(memcmp(received_frame,payload,sizeof(payload))==0);
    /* Split pbuf chain, confirm exactly one complete Ethernet frame on SPI. */
    struct pbuf *first=pbuf_alloc(PBUF_RAW,100,PBUF_POOL);
    struct pbuf *second=pbuf_alloc(PBUF_RAW,sizeof(payload)-100,PBUF_POOL);
    CHECK(first && second);
    first->next=second; first->tot_len=sizeof(payload);
    CHECK(pbuf_take(first,payload,sizeof(payload))==ERR_OK);
    h->initialized=1;
    CHECK(n.linkoutput(&n,first)==ERR_OK);
    esp_hosted_frame_t decoded;
    CHECK(esp_hosted_decode_frame(h,tx,sizeof(tx),&decoded)==ESP_HOSTED_FRAME_OK);
    CHECK(decoded.payload_length==sizeof(payload) && memcmp(decoded.payload,payload,sizeof(payload))==0);
    CHECK(ap.linkoutput(&ap,first)==ERR_OK);
    CHECK(esp_hosted_decode_frame(h,tx,sizeof(tx),&decoded)==ESP_HOSTED_FRAME_OK);
    CHECK(decoded.if_type==ESP_HOSTED_AP_IF_TYPE && decoded.payload_length==sizeof(payload));
    pbuf_free(first);
    const uint8_t ap_down[]={1,0,0,2,10,0,8,3,16,0x85,6,0xAA,0x30,2,16,13};
    CHECK(esp_hosted_encode_frame(h,3,0,1,ap_down,sizeof(ap_down),frame,sizeof(frame))==STM_OK);
    test_hal_inject_rx(frame,sizeof(frame));
    CHECK(esp_hosted_poll(h)==STM_OK && !netif_is_link_up(&ap));
    const uint8_t down[]={1,0,0,2,10,0,8,3,16,0x88,6,0xC2,0x30,2,0x12,0};
    CHECK(esp_hosted_encode_frame(h,3,0,1,down,sizeof(down),frame,sizeof(frame))==STM_OK);
    test_hal_inject_rx(frame,sizeof(frame));
    CHECK(esp_hosted_poll(h)==STM_OK && !netif_is_link_up(&n));
    CHECK(esp_hosted_lwip_sta_update(&a)==STM_OK && !a.dhcp_running && mock_dhcp_stops==1);
    CHECK(!esp_hosted_lwip_sta_has_address(&a) && n.ip_addr.byte[0]==0);
    CHECK(esp_hosted_encode_frame(h,3,0,1,event,sizeof(event),frame,sizeof(frame))==STM_OK);
    test_hal_inject_rx(frame,sizeof(frame));
    CHECK(esp_hosted_poll(h)==STM_OK && netif_is_link_up(&n));
    mock_dhcp_error=1;
    CHECK(esp_hosted_lwip_sta_update(&a)==STM_ERR_IO && !a.dhcp_running);
    mock_dhcp_error=0;
    CHECK(esp_hosted_lwip_sta_update(&a)==STM_OK && a.dhcp_running);
    n.ip_addr=(ip4_addr_t){{192,168,1,81}};
    CHECK(esp_hosted_lwip_sta_stop(&a)==STM_OK && !esp_hosted_lwip_sta_has_address(&a));
    CHECK(esp_hosted_delete(&h)==STM_OK);
    puts("lwip adapter tests: PASS");
    return 0;
}
