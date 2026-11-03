#include <zephyr/kernel.h>
#include <string.h>
#include <stdio.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/display.h>
#include <zmk/event_manager.h>
#include <zmk/events/wpm_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/display/widgets/layer_status.h>
#include <zmk/endpoints.h>
#include <zmk/keymap.h>

#include "wpm_status.h"

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);
static int last_wpm = -1;
static int64_t last_wpm_update_time = 0;
#define WPM_UPDATE_INTERVAL_MS 250  // Throttle: max 4 updates per second

/* Keys per second = key presses seen in the last KPS_WINDOW_MS. */
#define KPS_WINDOW_MS 1000
#define KPS_REFRESH_MS 250
#define KPS_RING_SIZE 200 /* also the maximum KPS that can be shown */

static uint32_t press_times[KPS_RING_SIZE];
static uint8_t press_head;
static struct k_spinlock press_lock;

struct wpm_status_state
{
    int wpm;
    const char *layer;
};

static int kps_listener(const zmk_event_t *eh)
{
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (ev != NULL && ev->state) {
        k_spinlock_key_t key = k_spin_lock(&press_lock);
        press_times[press_head] = k_uptime_get_32();
        press_head = (press_head + 1) % KPS_RING_SIZE;
        k_spin_unlock(&press_lock, key);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(widget_kps_counter, kps_listener);
ZMK_SUBSCRIPTION(widget_kps_counter, zmk_position_state_changed);

static int current_kps(void)
{
    uint32_t now = k_uptime_get_32();
    int count = 0;

    k_spinlock_key_t key = k_spin_lock(&press_lock);
    for (int i = 0; i < KPS_RING_SIZE; i++) {
        /* Unused slots are 0; guard so boot time doesn't count them. */
        if (press_times[i] != 0 && (now - press_times[i]) <= KPS_WINDOW_MS) {
            count++;
        }
    }
    k_spin_unlock(&press_lock, key);

    return count;
}

/* BPM = tempo of your recent key presses, like tap-tempo: the average gap between the
 * last few presses, converted to beats per minute. A pause longer than BPM_MAX_GAP_MS
 * ends the run (and shows 0). Presses closer than BPM_CHORD_MS count as one beat so
 * chords and rollover don't inflate the tempo. */
#define BPM_MAX_BEATS 8
#define BPM_MIN_BEATS 3
#define BPM_MAX_GAP_MS 1500
#define BPM_CHORD_MS 40
#define BPM_PRESSES_PER_BEAT 1 /* 2 = treat every second press as a beat, etc. */

static int current_bpm(void)
{
    uint32_t now = k_uptime_get_32();
    uint32_t beats[BPM_MAX_BEATS];
    int n = 0;

    k_spinlock_key_t key = k_spin_lock(&press_lock);
    uint8_t idx = press_head;
    for (int i = 0; i < KPS_RING_SIZE && n < BPM_MAX_BEATS; i++) {
        idx = (idx + KPS_RING_SIZE - 1) % KPS_RING_SIZE;
        uint32_t t = press_times[idx];
        if (t == 0) {
            break;
        }
        if (n == 0) {
            if ((now - t) > BPM_MAX_GAP_MS) {
                break;
            }
        } else {
            uint32_t gap = beats[n - 1] - t;
            if (gap < BPM_CHORD_MS) {
                continue;
            }
            if (gap > BPM_MAX_GAP_MS) {
                break;
            }
        }
        beats[n++] = t;
    }
    k_spin_unlock(&press_lock, key);

    if (n < BPM_MIN_BEATS) {
        return 0;
    }

    uint32_t span = beats[0] - beats[n - 1];
    int bpm = (int)((60000UL * (n - 1) + span / 2) / span) / BPM_PRESSES_PER_BEAT;
    return bpm > 999 ? 999 : bpm;
}

static struct wpm_status_state get_state(const zmk_event_t *_eh)
{
    const struct zmk_wpm_state_changed *ev = as_zmk_wpm_state_changed(_eh);
    uint8_t index = zmk_keymap_highest_layer_active();

    return (struct wpm_status_state){
        .wpm = ev ? ev->state : 0,
        .layer = zmk_keymap_layer_name(index)
    };
}

static void set_wpm(struct zmk_widget_wpm_status *widget, struct wpm_status_state state)
{
    // Early exit if WPM unchanged
    if (state.wpm == last_wpm) {
        return;
    }

    // Throttle updates to prevent display thread flooding
    int64_t now = k_uptime_get();
    if ((now - last_wpm_update_time) < WPM_UPDATE_INTERVAL_MS) {
        return;
    }
    last_wpm_update_time = now;
    last_wpm = state.wpm;

    // NULL check for layer name before strstr to prevent crash
    if (state.layer != NULL &&
        strstr(CONFIG_ZMK_DONGLE_DISPLAY_WPM_DISABLED_LAYERS, state.layer) != NULL) {
        lv_label_set_text(widget->wpm_label, "- WPM");
        return;
    }

    char wpm_text[16];
    snprintf(wpm_text, sizeof(wpm_text), "%i WPM", state.wpm);
    lv_label_set_text(widget->wpm_label, wpm_text);
}

static void wpm_status_update_cb(struct wpm_status_state state)
{
    struct zmk_widget_wpm_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node)
    {
        set_wpm(widget, state);
    }
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_wpm_status, struct wpm_status_state,
                            wpm_status_update_cb, get_state)
ZMK_SUBSCRIPTION(widget_wpm_status, zmk_wpm_state_changed);

/* Runs inside LVGL's own timer handler (display thread), so it may touch labels. */
static void kps_refresh_timer_cb(lv_timer_t *timer)
{
    ARG_UNUSED(timer);
    static int shown_kps = -1;
    static int shown_bpm = -1;

    int kps = current_kps();
    int bpm = current_bpm();
    if (kps == shown_kps && bpm == shown_bpm) {
        return;
    }

    char kps_text[16];
    char bpm_text[16];
    snprintf(kps_text, sizeof(kps_text), "%i KPS", kps);
    snprintf(bpm_text, sizeof(bpm_text), "%i BPM", bpm);

    struct zmk_widget_wpm_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node)
    {
        if (kps != shown_kps) {
            lv_label_set_text(widget->kps_label, kps_text);
        }
        if (bpm != shown_bpm) {
            lv_label_set_text(widget->bpm_label, bpm_text);
        }
    }
    shown_kps = kps;
    shown_bpm = bpm;
}

int zmk_widget_wpm_status_init(struct zmk_widget_wpm_status *widget, lv_obj_t *parent)
{
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);

    /* Two stacked lines:  "68 WPM" / "11 KPS" */
    widget->wpm_label = lv_label_create(widget->obj);
    lv_label_set_text(widget->wpm_label, "0 WPM");
    lv_obj_align(widget->wpm_label, LV_ALIGN_TOP_LEFT, 0, 0);

    widget->kps_label = lv_label_create(widget->obj);
    lv_label_set_text(widget->kps_label, "0 KPS");
    lv_obj_align_to(widget->kps_label, widget->wpm_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 1);

    widget->bpm_label = lv_label_create(widget->obj);
    lv_label_set_text(widget->bpm_label, "0 BPM");
    lv_obj_align_to(widget->bpm_label, widget->kps_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 1);

    sys_slist_append(&widgets, &widget->node);

    lv_timer_create(kps_refresh_timer_cb, KPS_REFRESH_MS, NULL);

    widget_wpm_status_init();
    return 0;
}

lv_obj_t *zmk_widget_wpm_status_obj(struct zmk_widget_wpm_status *widget)
{
    return widget->obj;
}
