#include "hal_stub.h"

#include <string.h>

static SCB_Type test_scb;
SCB_Type *SCB = &test_scb;

static uint32_t test_tick;
static GPIO_PinState test_signal = GPIO_PIN_SET;
void test_hal_set_tick(uint32_t tick) { test_tick = tick; }
void test_hal_set_signals(GPIO_PinState state) { test_signal = state; }
static HAL_StatusTypeDef test_spi_status = HAL_OK;
static uint8_t injected_rx[1600];
static uint16_t injected_size;
static void (*frame_callback)(const uint8_t *, uint8_t *, uint16_t);

void test_hal_set_frame_callback(void (*callback)(const uint8_t *, uint8_t *, uint16_t))
{
    frame_callback = callback;
}

void test_hal_inject_rx(const uint8_t *frame, uint16_t size)
{
    if (frame != NULL && size <= sizeof(injected_rx)) {
        memcpy(injected_rx, frame, size); injected_size = size;
    }
}

void test_hal_set_spi_status(HAL_StatusTypeDef status)
{
    test_spi_status = status;
}

HAL_StatusTypeDef HAL_SPI_TransmitReceive(SPI_HandleTypeDef *hspi,
                                          uint8_t *tx_data,
                                          uint8_t *rx_data,
                                          uint16_t size,
                                          uint32_t timeout)
{
    (void)hspi;
    (void)timeout;
    if (test_spi_status == HAL_OK && tx_data != NULL && rx_data != NULL) {
        if (frame_callback) {
            memset(rx_data, 0, size);
            frame_callback(tx_data, rx_data, size);
        } else if (injected_size) {
            memset(rx_data, 0, size);
            memcpy(rx_data, injected_rx, injected_size); injected_size = 0;
        } else { memcpy(rx_data, tx_data, size); }
    }
    return test_spi_status;
}

void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState state)
{
    (void)port;
    (void)pin;
    (void)state;
}

GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef *port, uint16_t pin)
{
    (void)port;
    (void)pin;
    return test_signal;
}

void HAL_Delay(uint32_t delay_ms)
{
    test_tick += delay_ms;
}

uint32_t HAL_GetTick(void)
{
    return test_tick++;
}

void SCB_CleanDCache_by_Addr(uint32_t *addr, int32_t dsize)
{
    (void)addr;
    (void)dsize;
}

void SCB_InvalidateDCache_by_Addr(uint32_t *addr, int32_t dsize)
{
    (void)addr;
    (void)dsize;
}
