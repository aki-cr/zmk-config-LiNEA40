/*
 * 充電の状態を LED で知らせる（左右それぞれ、自分の電池を自分の LED で）。点滅し続けず、変わったときに1回だけ:
 *   USB を差したとき: 充電中なら黄、充電がいらない（満タン）なら緑を1回
 *   充電が終わったとき: 緑を1回（長め）
 * 残りが少ないときの赤は rgbled-widget がもともと出す（CONFIG_RGBLED_WIDGET_BATTERY_LEVEL_CRITICAL 以下で残りが変わるたび）。
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

#define CHG_PIN 17
#define POLL_MS 1000

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
static bool was_charging;
static uint8_t settle; /* 差した直後は充電 IC が動き出すまで少し待ってから判定する */

static void blink(uint8_t color, uint16_t ms) {
    struct blink_item b = {.color = color, .duration_ms = ms, .sleep_ms = 0};
    k_msgq_put(&led_msgq, &b, K_NO_WAIT);
}

static void charge_poll(struct k_work *work) {
    bool powered = (NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk) != 0;
    bool charging = powered && gpio_pin_get_raw(gpio0, CHG_PIN) == 0;

    if (powered && !was_powered) {
        settle = 2;
    } else if (settle > 0 && --settle == 0) {
        blink(charging ? COLOR_YELLOW : COLOR_GREEN, 500);
    } else if (settle == 0 && powered && was_charging && !charging) {
        blink(COLOR_GREEN, 1000);
    }
    was_powered = powered;
    was_charging = charging;
    k_work_schedule(&charge_work, K_MSEC(POLL_MS));
}

static int linea_charge_init(void) {
    if (!device_is_ready(gpio0)) {
        return -ENODEV;
    }
    gpio_pin_configure(gpio0, CHG_PIN, GPIO_INPUT | GPIO_PULL_UP);
    k_work_init_delayable(&charge_work, charge_poll);
    /* 起動時の widget の表示（電池・接続）が終わってから始める。USB で起動したときは差した扱いにしない */
    was_powered = true;
    was_charging = gpio_pin_get_raw(gpio0, CHG_PIN) == 0;
    k_work_schedule(&charge_work, K_SECONDS(5));
    return 0;
}

SYS_INIT(linea_charge_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
