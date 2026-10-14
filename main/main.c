#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

// Change to 1, 2, 3 or 4 and flash again.
// 1 = raw GPIO interrupts (no debounce)
// 2 = 50 ms time-based debounce (outside ISR)
// 3 = ISR + state-based debounce
// 4 = polling every 10 ms + state machine (no ISR)
#define TEST_MODE          3

#define BUTTON_GPIO        GPIO_NUM_15
#define DEBOUNCE_MS        50
#define POLL_INTERVAL_MS   10

#if TEST_MODE < 1 || TEST_MODE > 4
#error "TEST_MODE must be between 1 and 4"
#endif

static const char *TAG = "button";

#if TEST_MODE != 4
static TaskHandle_t s_button_task = NULL;

static void IRAM_ATTR button_isr(void *arg)
{
    (void)arg;
    BaseType_t higher_priority_task_woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_button_task, &higher_priority_task_woken);
    if (higher_priority_task_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}
#endif

#if TEST_MODE == 3 || TEST_MODE == 4
typedef enum {
    BUTTON_IDLE,
    BUTTON_CHECK_PRESS,
    BUTTON_PRESSED,
    BUTTON_CHECK_RELEASE
} button_state_t;

// Called only from app_main task. The ISR does not read GPIO or calculate time.
static void button_state_step(bool start_event, int64_t now_ms,
                              button_state_t *state,
                              int64_t *state_start_ms,
                              uint32_t *counter)
{
    const bool pressed = (gpio_get_level(BUTTON_GPIO) == 0);

    switch (*state) {
        case BUTTON_IDLE:
            if (start_event && pressed) {
                *state_start_ms = now_ms;
                *state = BUTTON_CHECK_PRESS;
            }
            break;

        case BUTTON_CHECK_PRESS:
            if (!pressed) {
                *state = BUTTON_IDLE;
            } else if (now_ms - *state_start_ms >= DEBOUNCE_MS) {
                ++(*counter);
                ESP_LOGI(TAG, "Counter: %" PRIu32, *counter);
                *state = BUTTON_PRESSED;
            }
            break;

        case BUTTON_PRESSED:
            if (!pressed) {
                *state_start_ms = now_ms;
                *state = BUTTON_CHECK_RELEASE;
            }
            break;

        case BUTTON_CHECK_RELEASE:
            if (pressed) {
                *state = BUTTON_PRESSED;
            } else if (now_ms - *state_start_ms >= DEBOUNCE_MS) {
                *state = BUTTON_IDLE;
            }
            break;
    }
}
#endif

void app_main(void)
{
#if TEST_MODE != 4
    s_button_task = xTaskGetCurrentTaskHandle();
#endif

    const gpio_config_t button_config = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        // External 10 kOhm pull-up from GPIO 15 to 3.3 V.
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
#if TEST_MODE == 4
        .intr_type = GPIO_INTR_DISABLE,
#else
        .intr_type = GPIO_INTR_NEGEDGE,
#endif
    };
    ESP_ERROR_CHECK(gpio_config(&button_config));

#if TEST_MODE != 4
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(BUTTON_GPIO, button_isr, NULL));
#endif

    // Give the serial monitor a moment to connect after boot.
    vTaskDelay(pdMS_TO_TICKS(1000));

#if TEST_MODE == 1
    ESP_LOGI(TAG, "MODE 1: RAW INTERRUPTS (NO DEBOUNCE)");
#elif TEST_MODE == 2
    ESP_LOGI(TAG, "MODE 2: TIME-BASED DEBOUNCE (%d ms)", DEBOUNCE_MS);
#elif TEST_MODE == 3
    ESP_LOGI(TAG, "MODE 3: INTERRUPT + STATE-BASED DEBOUNCE");
#else
    ESP_LOGI(TAG, "MODE 4: POLLING + STATE MACHINE (%d ms)", POLL_INTERVAL_MS);
#endif

    uint32_t counter = 0;

#if TEST_MODE == 2
    int64_t last_accepted_ms = -DEBOUNCE_MS;
#endif
#if TEST_MODE == 3 || TEST_MODE == 4
    button_state_t state = BUTTON_IDLE;
    int64_t state_start_ms = 0;
#endif

    while (true) {
#if TEST_MODE != 1
        const int64_t now_ms = esp_timer_get_time() / 1000;
#endif

#if TEST_MODE != 4
        // Read and clear the number of pending ISR notifications atomically.
        const uint32_t interrupts = ulTaskNotifyTake(pdTRUE, 0);
#endif

#if TEST_MODE == 1
        if (interrupts > 0) {
            counter += interrupts;
            ESP_LOGI(TAG, "Interrupts: %" PRIu32 " (+%" PRIu32 ")",
                     counter, interrupts);
        }
#elif TEST_MODE == 2
        // Ignore new interrupts for 50 ms after the last accepted event.
        if (interrupts > 0 && now_ms - last_accepted_ms >= DEBOUNCE_MS) {
            last_accepted_ms = now_ms;
            ++counter;
            ESP_LOGI(TAG, "Counter: %" PRIu32, counter);
        }
#elif TEST_MODE == 3
        // ISR only notifies this task; GPIO and timing checks happen here.
        button_state_step(interrupts > 0, now_ms,
                          &state, &state_start_ms, &counter);
#else
        // No GPIO interrupt at all. The task polls every 10 ms.
        button_state_step(true, now_ms,
                          &state, &state_start_ms, &counter);
#endif
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}
