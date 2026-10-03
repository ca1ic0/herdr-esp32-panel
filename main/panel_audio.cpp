#include "panel_audio.h"

#include <stddef.h>

#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "panel_prefs.h"
#include "user_config.h"

static const char *TAG = "panel_audio";
static QueueHandle_t s_audio_q;
static i2c_master_bus_handle_t s_bus;
static volatile bool s_ready;
static volatile bool s_stop_now;
static volatile bool s_boot_pending;   /* boot chime requested before codec up */
static volatile uint8_t s_desired_volume = 50;
static i2s_chan_handle_t s_tx;
static esp_codec_dev_handle_t s_codec;

/* ESP-IDF 6.1 embeds with basename-derived symbols (done.pcm -> done_pcm). */
extern const uint8_t done_pcm_start[] asm("_binary_done_pcm_start");
extern const uint8_t done_pcm_end[] asm("_binary_done_pcm_end");
extern const uint8_t request_pcm_start[] asm("_binary_request_pcm_start");
extern const uint8_t request_pcm_end[] asm("_binary_request_pcm_end");
extern const uint8_t boot_pcm_start[] asm("_binary_boot_pcm_start");
extern const uint8_t boot_pcm_end[] asm("_binary_boot_pcm_end");

static bool init_codec(void)
{
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.auto_clear = true;
    if (i2s_new_channel(&chan, &s_tx, NULL) != ESP_OK) return false;

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                         I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    if (i2s_channel_init_std_mode(s_tx, &std_cfg) != ESP_OK) return false;
    if (i2s_channel_enable(s_tx) != ESP_OK) return false;

    audio_codec_i2s_cfg_t data_cfg = {};
    data_cfg.port = I2S_NUM_0;
    data_cfg.tx_handle = s_tx;
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&data_cfg);
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = BSP_I2C_NUM,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = s_bus,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    if (data_if == NULL || ctrl_if == NULL || gpio_if == NULL) return false;

    es8311_codec_cfg_t codec_cfg = {};
    codec_cfg.ctrl_if = ctrl_if;
    codec_cfg.gpio_if = gpio_if;
    codec_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
    codec_cfg.pa_pin = GPIO_NUM_NC;
    codec_cfg.use_mclk = true;
    codec_cfg.hw_gain.pa_voltage = 5.0;
    codec_cfg.hw_gain.codec_dac_voltage = 3.3;
    codec_cfg.mclk_div = 256;
    const audio_codec_if_t *codec_if = es8311_codec_new(&codec_cfg);
    if (codec_if == NULL) return false;

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_codec = esp_codec_dev_new(&dev_cfg);
    if (s_codec == NULL) return false;
    esp_codec_dev_sample_info_t sample = {
        .bits_per_sample = 16,
        .channel = 2,
        .channel_mask = 0x03,
        .sample_rate = 16000,
        .mclk_multiple = 256,
    };

    /*
     * Open muted. Enabling the ES8311 DAC with a non-zero volume produces a
     * DC step that the speaker amplifier turns into an audible pop, which
     * previously landed right in the middle of the boot splash animation.
     * auto_clear keeps the DMA buffer zeroed while muted, so the output stays
     * at 0 V until the real volume is applied below.
     */
    if (esp_codec_dev_set_out_vol(s_codec, 0) != ESP_CODEC_DEV_OK) return false;
    if (esp_codec_dev_open(s_codec, &sample) != ESP_CODEC_DEV_OK) return false;
    return esp_codec_dev_set_out_vol(s_codec, s_desired_volume) == ESP_CODEC_DEV_OK;
}

static void play_pcm(panel_audio_kind_t kind)
{
    if (kind == PANEL_AUDIO_BOOT) {
        /* Generated 8-bit chime (tools/sounds/make_boot_chime.py). Plays
         * straight through: it is short, and interrupting it sounds worse
         * than letting it finish. */
        const uint8_t *p = boot_pcm_start;
        const uint8_t *end = boot_pcm_end;
        while (p < end && !s_stop_now) {
            size_t len = (size_t)(end - p);
            if (len > 512) len = 512;
            if (esp_codec_dev_write(s_codec, (void *)p, (int)len) != ESP_CODEC_DEV_OK) {
                ESP_LOGW(TAG, "audio write failed");
                s_ready = false;
                break;
            }
            p += len;
        }
        return;
    }
    const uint8_t *start = kind == PANEL_AUDIO_REQUEST ? request_pcm_start : done_pcm_start;
    const uint8_t *end = kind == PANEL_AUDIO_REQUEST ? request_pcm_end : done_pcm_end;
    const uint8_t *p = start;
    uint8_t applied_volume = 255;
    while (p < end && !s_stop_now) {
        if (s_desired_volume != applied_volume) {
            applied_volume = s_desired_volume;
            esp_codec_dev_set_out_vol(s_codec, applied_volume);
        }
        size_t len = (size_t)(end - p);
        if (len > 512) len = 512;
        if (esp_codec_dev_write(s_codec, (void *)p, (int)len) != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "audio write failed");
            s_ready = false;
            break;
        }
        p += len;
        /* Let a new request interrupt a completion tone. */
        if (kind == PANEL_AUDIO_DONE && uxQueueMessagesWaiting(s_audio_q) > 0) break;
    }
}

static void audio_task(void *arg)
{
    (void)arg;
    if (!init_codec()) {
        ESP_LOGE(TAG, "ES8311/I2S unavailable; visual alerts remain active");
        s_ready = false;
        vTaskDelete(NULL);
        return;
    }
    s_ready = true;
    ESP_LOGI(TAG, "ES8311 playback ready (speaker connection requires physical check)");
    panel_audio_kind_t kind;
    uint8_t applied_volume = s_desired_volume;
    /* A boot chime requested during app_main() could not be queued before the
     * codec existed, so honour it as soon as the task comes up. */
    if (s_boot_pending) {
        s_boot_pending = false;
        kind = PANEL_AUDIO_BOOT;
        play_pcm(kind);
    }
    for (;;) {
        if (s_desired_volume != applied_volume) {
            applied_volume = s_desired_volume;
            esp_codec_dev_set_out_vol(s_codec, applied_volume);
        }
        if (xQueueReceive(s_audio_q, &kind, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (!s_stop_now && s_ready) play_pcm(kind);
        }
    }
}

extern "C" bool panel_audio_start(i2c_master_bus_handle_t bus)
{
    if (bus == NULL || s_audio_q != NULL) return false;
    s_bus = bus;
    panel_prefs_t p;
    panel_prefs_get(&p);
    s_desired_volume = p.sound_volume;
    s_audio_q = xQueueCreate(4, sizeof(panel_audio_kind_t));
    if (s_audio_q == NULL) return false;
    if (xTaskCreate(audio_task, "panel_audio", 6144, NULL, 3, NULL) != pdPASS) {
        vQueueDelete(s_audio_q);
        s_audio_q = NULL;
        return false;
    }
    return true;
}

extern "C" bool panel_audio_ready(void) { return s_ready; }

extern "C" bool panel_audio_play(panel_audio_kind_t kind)
{
    if (!s_ready || s_audio_q == NULL ||
        (kind != PANEL_AUDIO_REQUEST && kind != PANEL_AUDIO_DONE &&
         kind != PANEL_AUDIO_BOOT)) return false;
    s_stop_now = false;
    if (kind == PANEL_AUDIO_REQUEST) {
        if (xQueueSendToFront(s_audio_q, &kind, 0) == pdTRUE) return true;
        panel_audio_kind_t dropped;
        xQueueReceive(s_audio_q, &dropped, 0);
        return xQueueSendToFront(s_audio_q, &kind, 0) == pdTRUE;
    }
    return xQueueSend(s_audio_q, &kind, 0) == pdTRUE;
}

extern "C" void panel_audio_play_boot(void)
{
    /* Fired from app_main() while the codec is still initialising. Defer it:
     * the audio task plays it as soon as s_ready goes true. Honours
     * sound_enabled so a muted device stays silent. */
    panel_prefs_t p;
    panel_prefs_get(&p);
    if (!p.sound_enabled || p.sound_volume == 0) return;
    if (s_ready && s_audio_q != NULL) {
        panel_audio_kind_t kind = PANEL_AUDIO_BOOT;
        s_stop_now = false;
        xQueueSendToFront(s_audio_q, &kind, 0);
        return;
    }
    s_boot_pending = true;
}

extern "C" void panel_audio_stop(void)
{
    s_stop_now = true;
    if (s_audio_q != NULL) xQueueReset(s_audio_q);
}

extern "C" void panel_audio_set_volume(uint8_t percent)
{
    s_desired_volume = percent <= 100 ? percent : 100;
    if (s_desired_volume == 0) panel_audio_stop();
}
