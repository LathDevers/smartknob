#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <XPowersLib.h>
#include <lvgl.h>
#include <math.h>
#include "touch/TouchDrvCST92xx.h"

// ── Hardware pins for Waveshare ESP32-S3 Touch AMOLED 1.75" ──

// Shared I2C (PMU + Touch)
#define I2C_SDA 15
#define I2C_SCL 14

// QSPI display
#define LCD_CS 12
#define LCD_SCLK 38
#define LCD_SDIO0 4
#define LCD_SDIO1 5
#define LCD_SDIO2 6
#define LCD_SDIO3 7
#define LCD_RST 39

// Touch (CST9217)
#define TP_INT 40
#define TP_RST 13

// ── Display constants ──
static const int SCREEN_SIZE = 466;
static const int CENTER = SCREEN_SIZE / 2;

// ── Hardware objects ──

XPowersAXP2101 PMU;
TouchDrvCST92xx touch;
static volatile bool touch_irq = false;

class Waveshare_CO5300 : public Arduino_CO5300
{
public:
    Waveshare_CO5300(Arduino_DataBus *bus, int8_t rst)
        : Arduino_CO5300(bus, rst, 0, false, 480, 480, 0U, 0U, 0U, 0U) {}
    bool begin(int32_t speed = GFX_NOT_DEFINED) override
    {
        bool res = Arduino_CO5300::begin(speed);
        _width = 480;
        _height = 480;
        _max_x = 479;
        _max_y = 479;
        WIDTH = 480;
        HEIGHT = 480;
        return res;
    }
};

Arduino_DataBus *bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_GFX *gfx = new Waveshare_CO5300(bus, LCD_RST);

// ── LVGL buffers ──
static lv_disp_draw_buf_t draw_buf;
static lv_color_t *lvgl_buf;

// ── SmartKnob config structure (matches proto) ──

struct KnobConfig
{
    int32_t position;
    int32_t min_position;
    int32_t max_position;
    float position_width_radians;
    float detent_strength_unit;
    float snap_point;
    const char *text;
    int32_t detent_positions[5];
    uint8_t detent_positions_count;
    uint32_t color;
};

static KnobConfig configs[] = {
    {0, 0, -1, 10 * PI / 180, 0, 1.1, "Unbounded\nNo detents", {}, 0, 0x6B12A0},
    {0, 0, 10, 10 * PI / 180, 0, 1.1, "Bounded 0-10\nNo detents", {}, 0, 0x1A5276},
    {0, 0, 72, 10 * PI / 180, 0, 1.1, "Multi-rev\nNo detents", {}, 0, 0x1E8449},
    {0, 0, 1, 60 * PI / 180, 1, 0.55, "On/Off\nStrong detent", {}, 0, 0xA93226},
    {0, 0, 0, 60 * PI / 180, 0.01, 1.1, "Return-to-center", {}, 0, 0xD4AC0D},
    {127, 0, 255, 1 * PI / 180, 0, 1.1, "Fine values\nNo detents", {}, 0, 0x2E86C1},
    {127, 0, 255, 1 * PI / 180, 1, 1.1, "Fine values\nWith detents", {}, 0, 0x7D3C98},
    {0, 0, 31, 8.225806452 * PI / 180, 2, 1.1, "Coarse values\nStrong detents", {}, 0, 0xCA6F1E},
    {0, 0, 31, 8.225806452 * PI / 180, 0.2, 1.1, "Coarse values\nWeak detents", {}, 0, 0x17A589},
    {0, 0, 31, 7 * PI / 180, 2.5, 0.7, "Magnetic detents", {2, 10, 21, 22, 0}, 4, 0x884EA0},
    {0, -6, 6, 60 * PI / 180, 1, 0.55, "Return-to-center\nwith detents", {}, 0, 0x2C3E50},
};
static const int NUM_CONFIGS = sizeof(configs) / sizeof(configs[0]);

// ── Virtual knob state ──

static int current_config_idx = 0;
static int32_t knob_position = 0;
static float knob_sub_position = 0;

// ── UI objects ──
static lv_obj_t *bg;
static lv_obj_t *position_label;
static lv_obj_t *config_label;
static lv_obj_t *indicator_arc;
static lv_obj_t *bounds_arc;
static lv_obj_t *dot_indicator;
static lv_obj_t *center_circle;

// ── Touch tracking state ──
static bool touch_active = false;
static float touch_start_angle = 0;
static float angle_accumulator = 0;
static uint32_t touch_start_time = 0;
static int16_t touch_start_x = 0, touch_start_y = 0;
static bool is_tap = true;

// ── Helpers ──

static float angle_of(int16_t x, int16_t y)
{
    return atan2f(-(y - CENTER), x - CENTER); // math convention: CCW positive, 0 = right
}

static float normalize_angle(float a)
{
    while (a > PI)
        a -= 2 * PI;
    while (a < -PI)
        a += 2 * PI;
    return a;
}

static float clampf(float v, float lo, float hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

// ── Apply config ──

static void apply_config(int idx)
{
    current_config_idx = idx;
    KnobConfig &cfg = configs[idx];
    knob_position = cfg.position;
    knob_sub_position = 0;
    angle_accumulator = 0;
}

// ── Update the UI from knob state ──

static void update_ui()
{
    KnobConfig &cfg = configs[current_config_idx];
    int32_t num_positions = cfg.max_position - cfg.min_position + 1;
    bool bounded = cfg.max_position >= cfg.min_position;

    // Position label
    char pos_buf[16];
    snprintf(pos_buf, sizeof(pos_buf), "%d", (int)knob_position);
    lv_label_set_text(position_label, pos_buf);

    // Config label
    lv_label_set_text(config_label, cfg.text);

    // Background color tint
    uint32_t c = cfg.color;
    lv_obj_set_style_bg_color(bg, lv_color_hex(c), 0);

    // Indicator arc: shows position within range
    if (bounded && num_positions > 1)
    {
        lv_obj_clear_flag(indicator_arc, LV_OBJ_FLAG_HIDDEN);
        int32_t progress = (knob_position - cfg.min_position) * 100 / (cfg.max_position - cfg.min_position);
        lv_arc_set_value(indicator_arc, progress);
    }
    else
    {
        lv_obj_add_flag(indicator_arc, LV_OBJ_FLAG_HIDDEN);
    }

    // Dot indicator: shows angular position of knob
    float left_bound = PI / 2;
    if (bounded)
    {
        float range_radians = (cfg.max_position - cfg.min_position) * cfg.position_width_radians;
        left_bound = PI / 2 + range_radians / 2;
    }
    float raw_angle = left_bound - (knob_position - cfg.min_position) * cfg.position_width_radians;

    // Rubber-band sub-position at bounds
    float adjusted_sub = knob_sub_position * cfg.position_width_radians;
    if (bounded)
    {
        if (knob_position == cfg.min_position && knob_sub_position < 0)
        {
            adjusted_sub = -logf(1 - knob_sub_position * cfg.position_width_radians / 5 / PI * 180) * 5 * PI / 180;
        }
        else if (knob_position == cfg.max_position && knob_sub_position > 0)
        {
            adjusted_sub = logf(1 + knob_sub_position * cfg.position_width_radians / 5 / PI * 180) * 5 * PI / 180;
        }
    }
    float display_angle = raw_angle - adjusted_sub;

    // Convert from math angle to screen coordinates for the dot
    int dot_radius = SCREEN_SIZE / 2 - 30;
    int dot_x = CENTER + (int)(dot_radius * cosf(display_angle));
    int dot_y = CENTER - (int)(dot_radius * sinf(display_angle));
    lv_obj_set_pos(dot_indicator, dot_x - 8, dot_y - 8);
    lv_obj_clear_flag(dot_indicator, LV_OBJ_FLAG_HIDDEN);
}

// ── Process touch input as virtual knob ──

static void process_touch(int16_t x, int16_t y, bool pressed)
{
    KnobConfig &cfg = configs[current_config_idx];
    bool bounded = cfg.max_position >= cfg.min_position;

    if (pressed && !touch_active)
    {
        // Touch down
        touch_active = true;
        touch_start_angle = angle_of(x, y);
        touch_start_time = millis();
        touch_start_x = x;
        touch_start_y = y;
        is_tap = true;
        angle_accumulator = 0;
    }
    else if (pressed && touch_active)
    {
        // Touch move
        float current_angle = angle_of(x, y);
        float delta = normalize_angle(current_angle - touch_start_angle);

        // Check if this is still a tap (small movement)
        int dx = x - touch_start_x;
        int dy = y - touch_start_y;
        if (dx * dx + dy * dy > 30 * 30)
        {
            is_tap = false;
        }

        if (!is_tap)
        {
            // Compute distance from center to determine sensitivity
            float dist = sqrtf((x - CENTER) * (x - CENTER) + (y - CENTER) * (y - CENTER));
            float sensitivity = 1.0f;
            if (dist < 80)
            {
                sensitivity = 0.2f; // Near center = less sensitive
            }
            else if (dist > 180)
            {
                sensitivity = 1.5f; // Edge = more sensitive
            }

            // Accumulate angle delta, scaled by position width
            angle_accumulator += delta * sensitivity;

            float positions_moved = angle_accumulator / cfg.position_width_radians;

            if (cfg.detent_strength_unit > 0)
            {
                // Detented mode: snap to integer positions
                int32_t steps = (int32_t)positions_moved;
                if (steps != 0)
                {
                    knob_position -= steps;
                    angle_accumulator -= steps * cfg.position_width_radians;

                    if (bounded)
                    {
                        knob_position = max(cfg.min_position, min(cfg.max_position, knob_position));
                    }
                }
                knob_sub_position = -angle_accumulator / cfg.position_width_radians;
            }
            else
            {
                // Smooth mode: free-flowing position + sub-position
                int32_t steps = (int32_t)positions_moved;
                if (steps != 0)
                {
                    knob_position -= steps;
                    angle_accumulator -= steps * cfg.position_width_radians;
                }
                knob_sub_position = -angle_accumulator / cfg.position_width_radians;

                if (bounded)
                {
                    if (knob_position < cfg.min_position)
                    {
                        knob_position = cfg.min_position;
                        if (knob_sub_position < 0)
                            knob_sub_position = clampf(knob_sub_position, -3.0f, 0);
                    }
                    if (knob_position > cfg.max_position)
                    {
                        knob_position = cfg.max_position;
                        if (knob_sub_position > 0)
                            knob_sub_position = clampf(knob_sub_position, 0, 3.0f);
                    }
                }
            }
        }

        touch_start_angle = current_angle;
    }
    else if (!pressed && touch_active)
    {
        // Touch up
        touch_active = false;
        uint32_t duration = millis() - touch_start_time;

        if (is_tap && duration < 400)
        {
            // Check if tap was near center (within ~100px radius)
            int dx = touch_start_x - CENTER;
            int dy = touch_start_y - CENTER;
            if (dx * dx + dy * dy < 120 * 120)
            {
                // Center tap: cycle config
                apply_config((current_config_idx + 1) % NUM_CONFIGS);
            }
        }

        // Snap sub-position back to 0 when released (simulates spring-back)
        knob_sub_position = 0;
        angle_accumulator = 0;
    }

    update_ui();
}

// ── LVGL callbacks ──

void my_disp_flush(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p)
{
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);
    gfx->draw16bitRGBBitmap(area->x1 + 5, area->y1 + 2, (uint16_t *)color_p, w, h);
    lv_disp_flush_ready(disp_drv);
}

static void my_touchpad_read(lv_indev_drv_t *indev_drv, lv_indev_data_t *data)
{
    int16_t tx[1], ty[1];
    uint8_t touched = touch.getPoint(tx, ty, 1);
    if (touched)
    {
        data->state = LV_INDEV_STATE_PR;
        data->point.x = tx[0];
        data->point.y = ty[0];
        process_touch(tx[0], ty[0], true);
    }
    else
    {
        data->state = LV_INDEV_STATE_REL;
        if (touch_active)
        {
            process_touch(0, 0, false);
        }
    }
}

// ── UI Setup ──

static void create_ui()
{
    // Root background
    bg = lv_obj_create(lv_scr_act());
    lv_obj_set_size(bg, SCREEN_SIZE, SCREEN_SIZE);
    lv_obj_set_pos(bg, 0, 0);
    lv_obj_set_style_bg_color(bg, lv_color_hex(0x6B12A0), 0);
    lv_obj_set_style_bg_opa(bg, LV_OPA_30, 0);
    lv_obj_set_style_border_width(bg, 0, 0);
    lv_obj_set_style_pad_all(bg, 0, 0);
    lv_obj_set_style_radius(bg, 0, 0);
    lv_obj_set_scrollbar_mode(bg, LV_SCROLLBAR_MODE_OFF);

    // Outer ring (decorative)
    lv_obj_t *ring = lv_arc_create(bg);
    lv_obj_set_size(ring, SCREEN_SIZE - 10, SCREEN_SIZE - 10);
    lv_obj_align(ring, LV_ALIGN_CENTER, 0, 0);
    lv_arc_set_bg_angles(ring, 0, 360);
    lv_arc_set_range(ring, 0, 100);
    lv_arc_set_value(ring, 0);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_style(ring, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(ring, 2, LV_PART_MAIN);
    lv_obj_set_style_arc_color(ring, lv_color_hex(0x444444), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(ring, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_arc_width(ring, 0, LV_PART_INDICATOR);

    // Indicator arc (shows position progress)
    indicator_arc = lv_arc_create(bg);
    lv_obj_set_size(indicator_arc, SCREEN_SIZE - 40, SCREEN_SIZE - 40);
    lv_obj_align(indicator_arc, LV_ALIGN_CENTER, 0, 0);
    lv_arc_set_bg_angles(indicator_arc, 135, 45); // bottom-left to bottom-right
    lv_arc_set_range(indicator_arc, 0, 100);
    lv_arc_set_value(indicator_arc, 0);
    lv_obj_clear_flag(indicator_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_style(indicator_arc, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(indicator_arc, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_color(indicator_arc, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(indicator_arc, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_arc_width(indicator_arc, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(indicator_arc, lv_color_hex(0x5DADE2), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(indicator_arc, true, LV_PART_INDICATOR);

    // Position label (large number)
    position_label = lv_label_create(bg);
    lv_obj_set_style_text_font(position_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(position_label, lv_color_white(), 0);
    lv_obj_align(position_label, LV_ALIGN_CENTER, 0, -20);
    lv_label_set_text(position_label, "0");

    // Config name label
    config_label = lv_label_create(bg);
    lv_obj_set_style_text_font(config_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(config_label, lv_color_hex(0xBBBBBB), 0);
    lv_obj_set_style_text_align(config_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(config_label, SCREEN_SIZE - 100);
    lv_obj_align(config_label, LV_ALIGN_CENTER, 0, 40);
    lv_label_set_text(config_label, "Unbounded\nNo detents");

    // "Tap to switch" hint
    lv_obj_t *hint = lv_label_create(bg);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 160);
    lv_label_set_text(hint, "Tap center to switch mode\nSwipe edge to rotate");

    // Dot indicator (position marker on the arc)
    dot_indicator = lv_obj_create(bg);
    lv_obj_set_size(dot_indicator, 16, 16);
    lv_obj_set_style_radius(dot_indicator, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot_indicator, lv_color_hex(0x5DADE2), 0);
    lv_obj_set_style_bg_opa(dot_indicator, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dot_indicator, 2, 0);
    lv_obj_set_style_border_color(dot_indicator, lv_color_white(), 0);
    lv_obj_clear_flag(dot_indicator, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scrollbar_mode(dot_indicator, LV_SCROLLBAR_MODE_OFF);

    // Config index indicator
    lv_obj_t *idx_label = lv_label_create(bg);
    lv_obj_set_style_text_font(idx_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(idx_label, lv_color_hex(0x555555), 0);
    lv_obj_align(idx_label, LV_ALIGN_CENTER, 0, -100);

    // Store the index label for later update (use user_data)
    lv_obj_set_user_data(bg, idx_label);
}

// ── Arduino entry points ──

void setup()
{
    Serial.begin(115200);
    delay(500);
    Serial.println("SmartKnob Touch - Starting...");

    // Init I2C
    Wire.begin(I2C_SDA, I2C_SCL);

    // Init PMU
    if (PMU.init(Wire, I2C_SDA, I2C_SCL, AXP2101_SLAVE_ADDRESS))
    {
        PMU.setALDO1Voltage(3300);
        PMU.enableALDO1();
        PMU.setALDO2Voltage(3300);
        PMU.enableALDO2();
        PMU.setALDO3Voltage(3300);
        PMU.enableALDO3();
        delay(100);
        Serial.println("PMU initialized");
    }

    // Init display
    gfx->begin();
    gfx->fillScreen(BLACK);
    Serial.println("Display initialized");

    // Init touch
    touch.setPins(TP_RST, TP_INT);
    if (touch.begin(Wire, CST92XX_SLAVE_ADDRESS, I2C_SDA, I2C_SCL))
    {
        Serial.print("Touch initialized: ");
        Serial.println(touch.getModelName());
    }
    else
    {
        Serial.println("WARNING: Touch init failed - continuing without touch");
    }

    // Init LVGL
    lv_init();

    size_t buffer_size = SCREEN_SIZE * SCREEN_SIZE / 10;
    lvgl_buf = (lv_color_t *)heap_caps_malloc(buffer_size * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    lv_disp_draw_buf_init(&draw_buf, lvgl_buf, NULL, buffer_size);

    // Display driver
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = SCREEN_SIZE;
    disp_drv.ver_res = SCREEN_SIZE;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    // Touch input driver
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = my_touchpad_read;
    lv_indev_drv_register(&indev_drv);

    // Build UI
    create_ui();

    // Apply first config
    apply_config(0);
    update_ui();

    Serial.println("SmartKnob Touch ready!");
}

void loop()
{
    static uint32_t last_tick = 0;
    uint32_t now = millis();
    lv_tick_inc(now - last_tick);
    last_tick = now;
    lv_timer_handler();

    // Update index display
    lv_obj_t *idx_label = (lv_obj_t *)lv_obj_get_user_data(bg);
    if (idx_label)
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d / %d", current_config_idx + 1, NUM_CONFIGS);
        lv_label_set_text(idx_label, buf);
    }

    delay(5);
}
