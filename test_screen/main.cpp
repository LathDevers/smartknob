#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <XPowersLib.h>
#include <lvgl.h>

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

// Fix the GFX library bounds AND expand the canvas to allow hardware offsets
class Waveshare_CO5300 : public Arduino_CO5300
{
public:
    Waveshare_CO5300(Arduino_DataBus *bus, int8_t rst)
        : Arduino_CO5300(bus, rst, 0 /* rotation */, false /* IPS */) {}

    bool begin(int32_t speed = GFX_NOT_DEFINED) override
    {
        bool res = Arduino_CO5300::begin(speed);
        // Lock the bounds strictly to the 466x466 physical pixels
        _width = 466;
        _height = 466;
        _max_x = 465;
        _max_y = 465;
        WIDTH = 466;
        HEIGHT = 466;
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

    gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)color_p, w, h);
    lv_disp_flush_ready(disp_drv);
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

    // Wipe the 466x466 GRAM buffer to kill the static noise
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

    // Draw a clean UI to prove the pipeline is flawless
    lv_obj_t *bg = lv_obj_create(lv_scr_act());
    lv_obj_set_size(bg, 466, 466);
    lv_obj_set_pos(bg, 0, 0);
    lv_obj_set_style_bg_color(bg, lv_color_hex(0x000000), 0); // Black background
    lv_obj_set_style_border_width(bg, 0, 0);                  // Remove default theme border (white lines)
    lv_obj_set_style_pad_all(bg, 0, 0);                       // Remove default padding
    lv_obj_set_style_radius(bg, 0, 0);                        // Remove corner radius
    lv_obj_set_scrollbar_mode(bg, LV_SCROLLBAR_MODE_OFF);     // Remove scrollbar (right-side line)

    lv_obj_t *label = lv_label_create(bg);
    lv_obj_set_style_text_color(label, lv_color_hex(0x00FF00), 0); // Green text
    lv_label_set_text(label, "LVGL Pipeline: SUCCESS");
    lv_obj_align(label, LV_ALIGN_CENTER, 0, -20);

    lv_obj_t *btn = lv_btn_create(bg);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 20);
    lv_obj_t *btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "Waveshare S3");
    lv_obj_center(btn_label);
}

void loop()
{
    lv_timer_handler();
    delay(5);
}