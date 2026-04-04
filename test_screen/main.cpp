#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <XPowersLib.h>
#include <lvgl.h>
#include "icons/fan_fill_100.c"

XPowersAXP2101 PMU;

// I2C pins for Waveshare S3 1.75"
#define I2C_SDA 15
#define I2C_SCL 14

// QSPI pins for Waveshare S3 1.75"
#define LCD_CS 12
#define LCD_SCLK 38
#define LCD_SDIO0 4
#define LCD_SDIO1 5
#define LCD_SDIO2 6
#define LCD_SDIO3 7
#define LCD_RST 39

class Waveshare_CO5300 : public Arduino_CO5300
{
public:
    Waveshare_CO5300(Arduino_DataBus *bus, int8_t rst)
        : Arduino_CO5300(bus, rst, 0, false /* invert */, 480, 480, 0U, 0U, 0U, 0U) {}

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

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_GFX *gfx = new Waveshare_CO5300(bus, LCD_RST);

static lv_disp_draw_buf_t draw_buf;
static lv_color_t *buf;

void my_disp_flush(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p)
{
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);

    // Inject the factory bonding offsets directly into the GRAM push
    uint16_t offset_x = 5;
    uint16_t offset_y = 2;

    gfx->draw16bitRGBBitmap(area->x1 + offset_x, area->y1 + offset_y, (uint16_t *)color_p, w, h);
    lv_disp_flush_ready(disp_drv);
}

void set_arc_value(void *obj, int32_t v)
{
    lv_arc_set_value((lv_obj_t *)obj, v);
}

void setup()
{
    Serial.begin(115200);
    delay(2000);

    Wire.begin(I2C_SDA, I2C_SCL);
    if (PMU.init(Wire, I2C_SDA, I2C_SCL, AXP2101_SLAVE_ADDRESS))
    {
        PMU.setALDO1Voltage(3300);
        PMU.enableALDO1();
        PMU.setALDO2Voltage(3300);
        PMU.enableALDO2();
        PMU.setALDO3Voltage(3300);
        PMU.enableALDO3();
        delay(100);
    }

    gfx->begin();

    // Wipe the GRAM buffer
    gfx->fillScreen(BLACK);

    // Fire up LVGL
    lv_init();

    // Allocate a massive draw buffer in the S3's PSRAM (1/10th of screen size)
    size_t buffer_size = 466 * 466 / 10;
    buf = (lv_color_t *)heap_caps_malloc(buffer_size * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    lv_disp_draw_buf_init(&draw_buf, buf, NULL, buffer_size);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = 466;
    disp_drv.ver_res = 466;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    lv_obj_t *bg = lv_obj_create(lv_scr_act());
    lv_obj_set_size(bg, 466, 466);
    lv_obj_set_pos(bg, 0, 0);
    lv_obj_set_style_bg_color(bg, lv_color_black(), 0);
    lv_obj_set_style_border_width(bg, 0, 0);
    lv_obj_set_style_pad_all(bg, 0, 0);
    lv_obj_set_style_radius(bg, 0, 0);
    lv_obj_set_scrollbar_mode(bg, LV_SCROLLBAR_MODE_OFF);

    // Render the fan icon
    lv_obj_t *fan_img = lv_img_create(bg);
    lv_img_set_src(fan_img, &fan_fill_100);
    lv_obj_align(fan_img, LV_ALIGN_CENTER, 0, 0);

    // Rotation animation
    // lv_anim_t a;
    // lv_anim_init(&a);
    // lv_anim_set_var(&a, fan_img);
    // lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_img_set_angle);
    // lv_anim_set_values(&a, 0, 3600); // clockwise: (0, 3600); anti-clockwise: (3600, 0)
    // lv_anim_set_time(&a, 3000);      // full rotation in 3 seconds
    // lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    // lv_anim_start(&a);

    lv_obj_t *step_arc = lv_arc_create(bg);
    lv_obj_set_size(step_arc, 466 - 2 * 21, 466 - 2 * 21);
    lv_obj_align(step_arc, LV_ALIGN_CENTER, 0, 0);
    lv_arc_set_bg_angles(step_arc, 90 + 45, 90 - 45);
    lv_arc_set_range(step_arc, 0, 100);

    // Disable interaction and remove the ugly default knob
    lv_obj_clear_flag(step_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_style(step_arc, NULL, LV_PART_KNOB);

    lv_obj_set_style_arc_width(step_arc, 40, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(step_arc, 0, LV_PART_MAIN);
    lv_obj_set_style_arc_width(step_arc, 40, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(step_arc, lv_color_hex(0x222222), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(step_arc, true, LV_PART_INDICATOR);

    set_arc_value(step_arc, 100);
}

void loop()
{
    static uint32_t last_tick = 0;
    uint32_t current_tick = millis();

    lv_tick_inc(current_tick - last_tick);
    last_tick = current_tick;

    lv_timer_handler();
    delay(5);
}