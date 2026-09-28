#include "stm_esp_hosted_lwip.h"
#include "stm_esp_hosted_private.h"
#include "hal_stub.h"
#include "lwip/pbuf.h"
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
    struct netif n={0};
    uint8_t frame[1600], payload[1514];
    CHECK(esp_hosted_create(&cfg,&h)==STM_OK);
    CHECK(esp_hosted_lwip_prepare(&a,h,&n,mac)==STM_OK);
    n.state=&a; n.input=input;
    CHECK(esp_hosted_lwip_netif_init(&n)==ERR_OK);
    CHECK(n.hwaddr_len==6 && memcmp(n.hwaddr,mac,6)==0 && n.mtu==1500);
    CHECK(esp_hosted_lwip_attach(&a)==STM_OK);
    /* RPC event 775: host STA link up. */
    const uint8_t event[]={1,0,0,2,5,0,8,3,16,0x87,6};
    CHECK(esp_hosted_encode_frame(h,3,0,1,event,sizeof(event),frame,sizeof(frame))==STM_OK);
    test_hal_inject_rx(frame,sizeof(frame));
    CHECK(esp_hosted_poll(h)==STM_OK && netif_is_link_up(&n));
    for (unsigned i=0;i<sizeof(payload);++i) { payload[i]=(uint8_t)i; }
    CHECK(esp_hosted_encode_frame(h,1,0,1,payload,sizeof(payload),frame,sizeof(frame))==STM_OK);
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
    pbuf_free(first);
    const uint8_t down[]={1,0,0,2,5,0,8,3,16,0x88,6};
    CHECK(esp_hosted_encode_frame(h,3,0,1,down,sizeof(down),frame,sizeof(frame))==STM_OK);
    test_hal_inject_rx(frame,sizeof(frame));
    CHECK(esp_hosted_poll(h)==STM_OK && !netif_is_link_up(&n));
    CHECK(esp_hosted_delete(&h)==STM_OK);
    puts("lwip adapter tests: PASS");
    return 0;
}
