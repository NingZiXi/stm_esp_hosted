#ifndef STM_ESP_HOSTED_TEST_HAL_STUB_H
#define STM_ESP_HOSTED_TEST_HAL_STUB_H

#include <stdint.h>

typedef struct { uint32_t unused; } SPI_HandleTypeDef;
typedef struct { uint32_t unused; } GPIO_TypeDef;
typedef uint16_t GPIO_PinState;
typedef enum { HAL_OK = 0U, HAL_ERROR = 1U, HAL_BUSY = 2U, HAL_TIMEOUT = 3U } HAL_StatusTypeDef;

typedef struct { uint32_t CCR; } SCB_Type;
extern SCB_Type *SCB;
#define SCB_CCR_DC_Msk (1UL << 16)

#define GPIO_PIN_RESET ((GPIO_PinState)0U)
#define GPIO_PIN_SET   ((GPIO_PinState)1U)

HAL_StatusTypeDef HAL_SPI_TransmitReceive(SPI_HandleTypeDef *hspi,
                                          uint8_t *tx_data,
                                          uint8_t *rx_data,
                                          uint16_t size,
                                          uint32_t timeout);
void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState state);
GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef *port, uint16_t pin);
void HAL_Delay(uint32_t delay_ms);
void test_hal_inject_rx(const uint8_t *frame, uint16_t size);
void test_hal_set_frame_callback(void (*callback)(const uint8_t *, uint8_t *, uint16_t));
void test_hal_set_tick(uint32_t tick);
void test_hal_set_signals(GPIO_PinState state);
uint32_t test_hal_last_spi_timeout(void);
uint32_t HAL_GetTick(void);
void SCB_CleanDCache_by_Addr(uint32_t *addr, int32_t dsize);
void SCB_InvalidateDCache_by_Addr(uint32_t *addr, int32_t dsize);

#endif
