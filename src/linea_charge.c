/*
 * 充電と電池の残りを LED で知らせる（左右それぞれ、自分の電池を自分の LED で）
 *   充電中:     黄を短く光らせる（3秒ごと）
 *   充電完了:   緑を短く光らせる（10秒ごと）
 *   残りが少ない: USB につないでいないとき、残りが CONFIG_RGBLED_WIDGET_BATTERY_LEVEL_CRITICAL % 以下なら赤を短く（30秒ごと）
 * USB を差した瞬間はすぐ1回光らせる。
 *
 * 判定:
 *   USB の電気が来ているか: nRF52840 の POWER->USBREGSTATUS の VBUSDETECT
 *   充電中か: XIAO nRF52840 の充電 IC（BQ25101）の CHG 端子 = P0.17（充電中は Low）
 * LED は rgbled-widget の送り待ち（led_msgq）に頼んで光らせる（widget のほかの表示とぶつからないように）。
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <nrfx.h>

#include <zmk/battery.h>

#define CHG_PIN 17
#define POLL_MS 1000

#define COLOR_RED 1
#define COLOR_GREEN 2
#define COLOR_YELLOW 3

/* rgbled-widget（widget.c）の送り待ちと同じ形 */
struct blink_item {
    uint8_t color;
    uint16_t duration_ms;
    uint16_t sleep_ms;
};
extern struct k_msgq led_msgq;

static const struct device *gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));
static struct k_work_delayable charge_work;
static bool was_powered;
static uint32_t ticks;

static void blink(uint8_t color, uint16_t ms) {
    struct blink_item b = {.color = color, .duration_ms = ms, .sleep_ms = 0};
    k_msgq_put(&led_msgq, &b, K_NO_WAIT);
}

static void charge_poll(struct k_work *work) {
    bool powered = (NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk) != 0;
    bool charging = powered && gpio_pin_get_raw(gpio0, CHG_PIN) == 0;
    ticks++;

    if (powered && !was_powered) {
        ticks = 0; /* 差した瞬間に1回 */
    }
    was_powered = powered;

    if (powered) {
        if (charging && ticks % 3 == 0) {
            blink(COLOR_YELLOW, 300);
        } else if (!charging && ticks % 10 == 0) {
            blink(COLOR_GREEN, 300);
        }
    } else {
        uint8_t soc = zmk_battery_state_of_charge();
        if (soc > 0 && soc <= CONFIG_RGBLED_WIDGET_BATTERY_LEVEL_CRITICAL && ticks % 30 == 0) {
            blink(COLOR_RED, 200);
        }
    }
    k_work_schedule(&charge_work, K_MSEC(POLL_MS));
}

static int linea_charge_init(void) {
    if (!device_is_ready(gpio0)) {
        return -ENODEV;
    }
    gpio_pin_configure(gpio0, CHG_PIN, GPIO_INPUT | GPIO_PULL_UP);
    k_work_init_delayable(&charge_work, charge_poll);
    /* 起動時の widget の表示（電池・接続）が終わってから始める */
    was_powered = true;
    k_work_schedule(&charge_work, K_SECONDS(5));
    return 0;
}

SYS_INIT(linea_charge_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
