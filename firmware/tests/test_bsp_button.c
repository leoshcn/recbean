// Execute the real BSP with public-interface stubs and a fake ADC (no SDK needed).
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include "../components/bsp/src/bsp_button.c"

struct button_dev_t { button_driver_t *driver; bool live; };
static struct button_dev_t buttons[BSP_BTN_COUNT];
static int adc_token, cal_token, adc_live, cal_live, live_buttons;
static int create_calls, callback_calls, fail_create, fail_callback;
static int fail_adc, fail_channel, fail_cal, fail_read, fail_convert, fail_delete;
static int raw_mv, reads, events, releases;
static unsigned registered_events;   // 已注册的 button_event_t 位集合
static int64_t clock_us;
// 睡眠接口用到的 GPIO / 定时器 / 唤醒源假实现状态。
static int timer_running, pin_level = 1, pin_pullup, isr_added, intr_enabled;
static int gpio_wake, ls_gpio_wake, wake_calls;
static uint64_t ds_wake_mask;
static gpio_isr_t isr_fn;

esp_err_t adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t *cfg, adc_oneshot_unit_handle_t *h) {
    assert(cfg->unit_id == BSP_BTN_ADC_UNIT && !adc_live);
    if (fail_adc) return ESP_ERR_NO_MEM;
    adc_live = 1; *h = &adc_token; return ESP_OK;
}
esp_err_t adc_oneshot_del_unit(adc_oneshot_unit_handle_t h) {
    // 回滚时按键已删除；睡眠暂停时按键仍在，但轮询定时器必须先停。
    assert(h == &adc_token && adc_live && (!live_buttons || !timer_running));
    adc_live = 0; return ESP_OK;
}
esp_err_t adc_oneshot_config_channel(adc_oneshot_unit_handle_t h, int channel, const adc_oneshot_chan_cfg_t *cfg) {
    assert(h == &adc_token && channel == BSP_BTN_ADC_CHANNEL && cfg->atten == ADC_ATTEN_DB_12);
    assert(!pin_pullup);   // 内部上拉会改变分压档位，重建 ADC 前必须已关闭
    return fail_channel ? ESP_FAIL : ESP_OK;
}
esp_err_t adc_oneshot_read(adc_oneshot_unit_handle_t h, int channel, int *raw) {
    assert(h == &adc_token && adc_live && channel == BSP_BTN_ADC_CHANNEL);
    ++reads;
    if (fail_read) return ESP_FAIL;
    *raw = raw_mv; return ESP_OK;
}
esp_err_t adc_cali_create_scheme_curve_fitting(const adc_cali_curve_fitting_config_t *cfg, adc_cali_handle_t *h) {
    assert(adc_live && !cal_live && cfg->atten == ADC_ATTEN_DB_12);
    if (fail_cal) return ESP_ERR_NO_MEM;
    cal_live = 1; *h = &cal_token; return ESP_OK;
}
esp_err_t adc_cali_delete_scheme_curve_fitting(adc_cali_handle_t h) {
    assert(h == &cal_token && cal_live && (!live_buttons || !timer_running));
    cal_live = 0; return ESP_OK;
}
esp_err_t adc_cali_raw_to_voltage(adc_cali_handle_t h, int raw, int *mv) {
    assert(h == &cal_token && cal_live);
    if (fail_convert) { *mv = 0; return ESP_FAIL; }
    *mv = raw; return ESP_OK;
}
int64_t esp_timer_get_time(void) { return clock_us; }
esp_err_t iot_button_create(const button_config_t *cfg, const button_driver_t *driver, button_handle_t *h) {
    // 判定门限必须由 BSP 显式下发(bsp_pins.h),不能退回组件默认的 180 / 1500ms。
    assert(cfg->short_press_time == BSP_BTN_SHORT_PRESS_MS);
    assert(cfg->long_press_time == BSP_BTN_LONG_PRESS_MS);
    if (++create_calls == fail_create) return ESP_ERR_NO_MEM;
    for (int i = 0; i < BSP_BTN_COUNT; ++i) {
        if (buttons[i].live) continue;
        buttons[i] = (struct button_dev_t){ .driver = (button_driver_t *)driver, .live = true };
        *h = &buttons[i]; ++live_buttons; timer_running = 1;   // 组件在首个按键创建时启动定时器
        assert(driver->get_key_level((button_driver_t *)driver) == BUTTON_INACTIVE);
        return ESP_OK;
    }
    assert(false); return ESP_FAIL;
}
esp_err_t iot_button_delete(button_handle_t h) {
    assert(h && h->live && adc_live && cal_live);
    if (fail_delete) return ESP_FAIL;
    assert(h->driver->del(h->driver) == ESP_OK);
    h->live = false; --live_buttons;
    if (!live_buttons) timer_running = 0;   // 最后一个按键删除时组件删除定时器
    return ESP_OK;
}
esp_err_t iot_button_register_cb(button_handle_t h, button_event_t ev, button_event_args_t *args, button_cb_t cb, void *u) {
    (void)args;
    assert(h->live);
    registered_events |= 1u << ev;
    cb(h, u); // No user callbacks may escape a partial initialization.
    return ++callback_calls == fail_callback ? ESP_ERR_NO_MEM : ESP_OK;
}
esp_err_t iot_button_stop(void) { assert(timer_running && adc_live); timer_running = 0; return ESP_OK; }
esp_err_t iot_button_resume(void) {
    assert(adc_live && cal_live);   // 先建 ADC 再恢复轮询
    timer_running = 1; return ESP_OK;
}
esp_err_t gpio_config(const gpio_config_t *cfg) {
    assert(cfg->pin_bit_mask == 1ULL << BSP_BTN_GPIO && cfg->mode == GPIO_MODE_INPUT);
    assert(cfg->pull_up_en == GPIO_PULLUP_ENABLE && cfg->intr_type == GPIO_INTR_DISABLE);
    assert(!adc_live);              // ADC 占着引脚时数字电平恒为 0，必须先释放
    pin_pullup = 1; return ESP_OK;
}
int gpio_get_level(gpio_num_t pin) { assert(pin == BSP_BTN_GPIO && !adc_live && pin_pullup); return pin_level; }
esp_err_t gpio_set_pull_mode(gpio_num_t pin, gpio_pull_mode_t m) {
    assert(pin == BSP_BTN_GPIO && m == GPIO_FLOATING); pin_pullup = 0; return ESP_OK;
}
esp_err_t gpio_set_intr_type(gpio_num_t pin, gpio_int_type_t t) { assert(pin == BSP_BTN_GPIO); (void)t; return ESP_OK; }
esp_err_t gpio_install_isr_service(int flags) { (void)flags; return ESP_ERR_INVALID_STATE; }
esp_err_t gpio_isr_handler_add(gpio_num_t pin, gpio_isr_t fn, void *arg) {
    assert(pin == BSP_BTN_GPIO && !isr_added); (void)arg; isr_fn = fn; isr_added = 1; return ESP_OK;
}
esp_err_t gpio_isr_handler_remove(gpio_num_t pin) { assert(pin == BSP_BTN_GPIO && isr_added); isr_added = 0; return ESP_OK; }
esp_err_t gpio_intr_enable(gpio_num_t pin) { assert(pin == BSP_BTN_GPIO && gpio_wake); intr_enabled = 1; return ESP_OK; }
esp_err_t gpio_intr_disable(gpio_num_t pin) { assert(pin == BSP_BTN_GPIO); intr_enabled = 0; return ESP_OK; }
esp_err_t gpio_wakeup_enable(gpio_num_t pin, gpio_int_type_t t) {
    assert(pin == BSP_BTN_GPIO && t == GPIO_INTR_LOW_LEVEL && pin_level == 1); gpio_wake = 1; return ESP_OK;
}
esp_err_t gpio_wakeup_disable(gpio_num_t pin) { assert(pin == BSP_BTN_GPIO); gpio_wake = 0; return ESP_OK; }
esp_err_t esp_sleep_enable_gpio_wakeup(void) { ls_gpio_wake = 1; return ESP_OK; }
esp_err_t esp_sleep_disable_wakeup_source(esp_sleep_source_t s) { assert(s == ESP_SLEEP_WAKEUP_GPIO); ls_gpio_wake = 0; return ESP_OK; }
esp_err_t esp_deep_sleep_enable_gpio_wakeup(uint64_t mask, esp_deepsleep_gpio_wake_up_mode_t mode) {
    assert(mode == ESP_GPIO_WAKEUP_GPIO_LOW && pin_level == 1 && !adc_live && !timer_running);
    ds_wake_mask = mask; return ESP_OK;
}
static void wake_cb(void *u) { assert(u == &wake_calls); ++wake_calls; }
static void event_cb(bsp_btn_t btn, bsp_btn_ev_t ev, void *u) {
    assert(u == &events);
    if (ev == BSP_BTN_RELEASE) {
        assert(btn == BSP_BTN_UP);
        ++releases;
        return;
    }
    assert(btn == BSP_BTN_OK && ev == BSP_BTN_CLICK);
    ++events;
}
static void reset_faults(void) {
    fail_adc = fail_channel = fail_cal = fail_read = fail_convert = fail_delete = 0;
    fail_create = fail_callback = create_calls = callback_calls = 0;
}
static void assert_clean(void) {
    assert(!adc_live && !cal_live && !live_buttons && !s_ready && !s_adc && !s_cali);
    for (int i = 0; i < BSP_BTN_COUNT; ++i) assert(!s_btn[i]);
}
static void retry_success(void) {
    assert_clean(); reset_faults();
    assert(bsp_button_init(event_cb, &events) == ESP_OK);
    assert(create_calls == BSP_BTN_COUNT && live_buttons == BSP_BTN_COUNT);
    assert(bsp_button_init(event_cb, &events) == ESP_OK);
    assert(create_calls == BSP_BTN_COUNT);
    button_cleanup(); assert_clean();
}
static void check_voltage(int mv, int expected) {
    raw_mv = mv; clock_us += 2000;
    const int before = reads;
    for (int i = 0; i < BSP_BTN_COUNT; ++i) {
        assert(s_drivers[i].base.get_key_level(&s_drivers[i].base) == (i == expected));
    }
    assert(reads - before == CONFIG_ADC_BUTTON_SAMPLE_TIMES);
}
// 浅睡暂停/恢复与深睡准备：ADC 释放先于数字回读，按住时拒绝武装唤醒。
static void test_sleep(void) {
    assert_clean(); reset_faults();
    assert(bsp_button_suspend(wake_cb, &wake_calls) == ESP_ERR_INVALID_STATE);   // 未初始化
    assert(bsp_button_init(event_cb, &events) == ESP_OK);

    // 有键按住：拒绝暂停并恢复原状。
    pin_level = 0;
    assert(bsp_button_suspend(wake_cb, &wake_calls) == ESP_ERR_INVALID_STATE);
    assert(adc_live && cal_live && timer_running && !s_suspended);
    assert(!gpio_wake && !ls_gpio_wake && !intr_enabled && !pin_pullup);
    pin_level = 1;

    // 松开：暂停，ADC 释放，GPIO 唤醒与中断已武装。
    assert(bsp_button_suspend(wake_cb, &wake_calls) == ESP_OK);
    assert(!adc_live && !cal_live && !timer_running && s_suspended);
    assert(gpio_wake && ls_gpio_wake && intr_enabled && isr_added && pin_pullup);
    assert(bsp_button_read_mv() == -1);
    clock_us += 2000;
    for (int i = 0; i < BSP_BTN_COUNT; ++i) assert(!button_level(&s_drivers[i].base));
    assert(bsp_button_suspend(wake_cb, &wake_calls) == ESP_OK);   // 幂等
    isr_fn(NULL);                                                  // 按键：只通知一次
    assert(wake_calls == 1 && !intr_enabled);

    // 恢复：先解除唤醒、关上拉、重建 ADC，再恢复轮询。
    assert(bsp_button_resume() == ESP_OK);
    assert(adc_live && cal_live && timer_running && !s_suspended);
    assert(!gpio_wake && !ls_gpio_wake && !isr_added && !pin_pullup);
    check_voltage(300, BSP_BTN_DOWN);
    assert(bsp_button_resume() == ESP_OK);                         // 未暂停时为空操作

    // 恢复时 ADC 重建失败：保持暂停，可重试。
    assert(bsp_button_suspend(wake_cb, &wake_calls) == ESP_OK);
    fail_adc = 1;
    assert(bsp_button_resume() != ESP_OK && s_suspended && !timer_running);
    fail_adc = 0;
    assert(bsp_button_resume() == ESP_OK && !s_suspended && timer_running);

    // 深睡准备（按键活动中）：按住时回读 0 并拒绝，松开后武装 GPIO0 低电平唤醒。
    int level = 5;
    pin_level = 0;
    assert(bsp_button_prepare_deep_sleep(&level) == ESP_ERR_INVALID_STATE);
    assert(level == 0 && ds_wake_mask == 0 && !adc_live && !timer_running);
    pin_level = 1;
    assert(bsp_button_prepare_deep_sleep(&level) == ESP_OK);
    assert(level == 1 && ds_wake_mask == 1ULL << BSP_BTN_GPIO);

    // 深睡准备（已浅睡暂停）：先解除浅睡唤醒。
    assert(bsp_button_resume() == ESP_OK);
    assert(bsp_button_suspend(wake_cb, &wake_calls) == ESP_OK);
    ds_wake_mask = 0;
    assert(bsp_button_prepare_deep_sleep(NULL) == ESP_OK);
    assert(ds_wake_mask && !gpio_wake && !ls_gpio_wake && !isr_added);
    assert(bsp_button_resume() == ESP_OK);
    button_cleanup(); assert_clean();

    // 从未初始化（低电唤醒后直接再睡）也能武装唤醒。
    ds_wake_mask = 0;
    assert(bsp_button_prepare_deep_sleep(NULL) == ESP_OK && ds_wake_mask);
    s_suspended = false;
}
int main(void) {
    for (int i = 1; i <= BSP_BTN_COUNT; ++i) {
        reset_faults(); fail_create = i;
        assert(bsp_button_init(event_cb, &events) != ESP_OK); retry_success();
    }
    // 每个按键注册 5 个回调：按下 / 单击 / 双击 / 长按 / 松开。
    for (int i = 1; i <= BSP_BTN_COUNT * 5; ++i) {
        reset_faults(); fail_callback = i;
        assert(bsp_button_init(event_cb, &events) != ESP_OK); retry_success();
    }
    reset_faults(); fail_adc = 1;
    assert(bsp_button_init(event_cb, &events) != ESP_OK); retry_success();
    fail_channel = 1;
    assert(bsp_button_init(event_cb, &events) != ESP_OK); retry_success();
    fail_cal = 1;
    assert(bsp_button_init(event_cb, &events) != ESP_OK); retry_success();
    assert(events == 0);
    assert(bsp_button_init(event_cb, &events) == ESP_OK);
    cb_click(NULL, (void *)(intptr_t)BSP_BTN_OK); assert(events == 1);
    // 松开事件（按住说话依赖它）：注册了 BUTTON_PRESS_UP，并以 BSP_BTN_RELEASE 上报。
    assert(registered_events & (1u << BUTTON_PRESS_UP));
    assert(registered_events & (1u << BUTTON_PRESS_DOWN));
    cb_up(NULL, (void *)(intptr_t)BSP_BTN_UP); assert(releases == 1 && events == 1);
    check_voltage(0, BSP_BTN_UP); check_voltage(149, BSP_BTN_UP);
    check_voltage(150, BSP_BTN_DOWN); check_voltage(446, BSP_BTN_DOWN);
    check_voltage(447, BSP_BTN_OK); check_voltage(1899, BSP_BTN_OK);
    check_voltage(1900, -1); check_voltage(3300, -1);
    assert(bsp_button_read_mv() == 3300);
    fail_read = 1; clock_us += 2000;
    for (int i = 0; i < BSP_BTN_COUNT; ++i) assert(!button_level(&s_drivers[i].base));
    assert(bsp_button_read_mv() == -1);
    fail_read = 0; fail_convert = 1; clock_us += 2000;
    for (int i = 0; i < BSP_BTN_COUNT; ++i) assert(!button_level(&s_drivers[i].base));
    assert(bsp_button_read_mv() == -1);
    fail_delete = 1; button_cleanup();
    assert(adc_live && cal_live && live_buttons == BSP_BTN_COUNT);
    assert(bsp_button_init(event_cb, &events) == ESP_ERR_INVALID_STATE);
    fail_delete = 0; button_cleanup(); retry_success();
    test_sleep();
    puts("BSP button fault-injection tests: PASS");
}
