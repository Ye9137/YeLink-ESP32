// Copyright (c) 2026 Ye
// ST7735 底层驱动：由 pet_main.c 提交 RGB565 画面，保留原有接线和颜色设置。
#include "display.h"
#include "sdkconfig.h"

// 标准整数、长度和内存复制函数。
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// ESP-IDF 的 GPIO、SPI、错误检查、日志和 FreeRTOS 延时接口。
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// 屏幕丝印 SCL/SDA 在本模块上分别代表 SPI 时钟和 SPI 数据输入，不是 I2C。
// GPIO 选择尽量集中在开发板 GPIO14 所在的一侧，接线时以开发板丝印为准。
#define TFT_SCLK GPIO_NUM_14
#define TFT_MOSI GPIO_NUM_13
#define TFT_RST  GPIO_NUM_25
#define TFT_DC   GPIO_NUM_26
#define TFT_CS   GPIO_NUM_27
// MADCTL 寄存器的颜色顺序选择：0 表示 RGB，1 表示 BGR。
// 当前设为 RGB，用于修正之前红蓝通道颠倒造成的整体偏蓝。
#define TFT_BGR 0
// 是否启用颜色反相命令：正常设为 0，只有黑白整体反相时才改成 1。
#define TFT_INVERT 0

// 不能假定所有 ST7735 模块都允许访问 132x162 地址范围。
// 按屏幕标注的 128x160 发送；有确切面板资料时再调整偏移。
#define LCD_WIDTH 128
#define LCD_HEIGHT 160
#define LCD_X_OFFSET 0
#define LCD_Y_OFFSET 0

// 日志标签会显示在串口输出中。
static const char *TAG = "tft_pet";
// SPI 设备句柄在初始化后用于发送所有屏幕命令和数据。
static spi_device_handle_t tft;


// 使用 FreeRTOS 的系统节拍实现毫秒延时。
static void delay_ms(unsigned ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

// 发送一段 SPI 数据。
// data_mode=0 表示屏幕命令，data_mode=1 表示参数或像素数据。
static void transfer(int data_mode, const void *bytes, size_t length)
{
    // 长度为 0 时无需发起 SPI 事务。
    if (length == 0) {
        return;
    }
    // DC 低电平表示命令，高电平表示数据。
    ESP_ERROR_CHECK(gpio_set_level(TFT_DC, data_mode));

    // ESP-IDF 的 length 使用“位”为单位，因此字节数需要乘以 8。
    spi_transaction_t transaction = {
        .length = length * 8,
        .tx_buffer = bytes,
    };
    // 同步传输，返回后才能改变 DC 或复用缓冲区。
    ESP_ERROR_CHECK(spi_device_polling_transmit(tft, &transaction));
}

// 发送一个 ST7735 命令以及该命令后面的全部参数。
static void command(uint8_t opcode, const uint8_t *data, size_t length)
{
    // 单任务独占 SPI2；命令及其全部数据发送完成前保持 CS 有效。
    ESP_ERROR_CHECK(gpio_set_level(TFT_CS, 0));
    transfer(0, &opcode, 1);   // 先发送 1 字节命令码。
    transfer(1, data, length); // 再发送参数；无参数时 transfer 会直接返回。
    ESP_ERROR_CHECK(gpio_set_level(TFT_CS, 1));
}

// 初始化 ESP32 GPIO、SPI 总线以及 ST7735 屏幕控制器。
void display_init(void)
{
    // DC、RST 和 CS 都由软件控制，配置为普通推挽输出。
    gpio_config_t control = {
        .pin_bit_mask = (1ULL << TFT_DC) | (1ULL << TFT_RST) | (1ULL << TFT_CS),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&control));
    // CS 空闲时保持高电平；RST 先拉低，开始硬件复位；DC 默认置低。
    ESP_ERROR_CHECK(gpio_set_level(TFT_CS, 1));
    ESP_ERROR_CHECK(gpio_set_level(TFT_RST, 0));
    ESP_ERROR_CHECK(gpio_set_level(TFT_DC, 0));
    // 配置 SPI2 总线。本屏只写不读，所以不使用 MISO。
    spi_bus_config_t bus = {
        .mosi_io_num = TFT_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = TFT_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        // 支持一次完整画面，DMA 驱动自动串联硬件描述符。
        .max_transfer_sz = SCREEN_BYTES,
    };
    // SPI_DMA_CH_AUTO 让 ESP-IDF 自动选择可用 DMA 通道。
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

    // 配置屏幕这个 SPI 设备。
    spi_device_interface_config_t device = {
        .clock_speed_hz = CONFIG_YE_LCD_SPI_MHZ * 1000 * 1000,
        .mode = 0,                     // ST7735 使用 SPI Mode 0。
        .spics_io_num = -1, // CS 由 command() 控制
        .queue_size = 1,
        // 屏幕只写不读，关闭读方向的 dummy 补偿/频率检查，允许最高 80MHz 输出。
        .flags = SPI_DEVICE_NO_DUMMY,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &device, &tft));

    // 保持复位低电平一段时间，然后释放复位并等待屏幕内部稳定。
    delay_ms(20);
    ESP_ERROR_CHECK(gpio_set_level(TFT_RST, 1));
    delay_ms(120);
    command(0x01, NULL, 0); // SWRESET：软件复位。
    delay_ms(150);
    command(0x11, NULL, 0); // SLPOUT：退出休眠模式。
    delay_ms(500);

    // 以下为 ST7735R/S 常用的帧率、电源和电压初始化参数。
    // 参数参考 Adafruit_ST7735.cpp 的 Rcmd1；第三方声明见 THIRD_PARTY_NOTICES.txt。
    // B1/B2/B3：普通、空闲和局部显示模式的帧率控制。
    command(0xB1, (const uint8_t[]){0x01, 0x2C, 0x2D}, 3);
    command(0xB2, (const uint8_t[]){0x01, 0x2C, 0x2D}, 3);
    command(0xB3, (const uint8_t[]){0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D}, 6);
    command(0xB4, (const uint8_t[]){0x07}, 1); // 显示反转控制方式。
    // C0～C5：内部升压、电源和 VCOM 电压设置。
    command(0xC0, (const uint8_t[]){0xA2, 0x02, 0x84}, 3);
    command(0xC1, (const uint8_t[]){0xC5}, 1);
    command(0xC2, (const uint8_t[]){0x0A, 0x00}, 2);
    command(0xC3, (const uint8_t[]){0x8A, 0x2A}, 2);
    command(0xC4, (const uint8_t[]){0x8A, 0xEE}, 2);
    command(0xC5, (const uint8_t[]){0x0E}, 1);
    // 0x21 开启颜色反相，0x20 关闭颜色反相。
    command(TFT_INVERT ? 0x21 : 0x20, NULL, 0);
    // MADCTL 控制扫描方向和 RGB/BGR 顺序；0xC0 保持当前屏幕方向。
    command(0x36, (const uint8_t[]){0xC0 | (TFT_BGR ? 0x08 : 0x00)}, 1);
    command(0x3A, (const uint8_t[]){0x05}, 1); // COLMOD：每像素使用 16 位 RGB565。
    command(0x13, NULL, 0);                   // NORON：使用完整显示区域。
    delay_ms(10);
}

// 将完整的 RGB565 帧缓冲发送到屏幕显存。
void display_frame(const uint8_t *pixels)
{
    display_rows(pixels, 0, LCD_HEIGHT);
}

// 参考官方 SPI LCD 示例的中断 DMA 事务模式，原生适配当前 ESP-IDF。
// 每次仅设一次窗口，去掉逐 8 行地址命令和内存复制；CS 保持到 DMA 完成。
void display_rows(const uint8_t *pixels, int first_row, int row_count)
{
    ESP_ERROR_CHECK(pixels && first_row >= 0 && row_count > 0 &&
                    first_row + row_count <= LCD_HEIGHT ? ESP_OK : ESP_ERR_INVALID_ARG);
    const uint8_t columns[] = {0, LCD_X_OFFSET, 0, LCD_X_OFFSET + LCD_WIDTH - 1};
    const uint8_t rows[] = {0, LCD_Y_OFFSET + first_row, 0, LCD_Y_OFFSET + first_row + row_count - 1};
    ESP_ERROR_CHECK(spi_device_acquire_bus(tft, portMAX_DELAY));
    command(0x2A, columns, sizeof(columns));
    command(0x2B, rows, sizeof(rows));
    uint8_t opcode = 0x2C;
    ESP_ERROR_CHECK(gpio_set_level(TFT_CS, 0));
    transfer(0, &opcode, 1);
    ESP_ERROR_CHECK(gpio_set_level(TFT_DC, 1));
    spi_transaction_t transaction = {
        .length = (size_t)row_count * LCD_WIDTH * 16,
        .tx_buffer = pixels + (size_t)first_row * LCD_WIDTH * 2,
    };
    // 等待中断时让出 CPU，Wi-Fi 和 HTTPS 可以继续运行。
    ESP_ERROR_CHECK(spi_device_queue_trans(tft, &transaction, portMAX_DELAY));
    spi_transaction_t *completed = NULL;
    ESP_ERROR_CHECK(spi_device_get_trans_result(tft, &completed, portMAX_DELAY));
    configASSERT(completed == &transaction);
    ESP_ERROR_CHECK(gpio_set_level(TFT_CS, 1));
    spi_device_release_bus(tft);
}

void display_enable(void)
{
    // 打印当前引脚和 SPI 速度，方便核对实际接线。
    int actual_khz = 0;
    ESP_ERROR_CHECK(spi_device_get_actual_freq(tft, &actual_khz));
    ESP_LOGI(TAG, "ST7735 DMA; SPI=%d kHz; target=%d FPS; SCL=14 SDA=13 RST=25 DC=26 CS=27",
             actual_khz, CONFIG_YE_LCD_TARGET_FPS);
    // 图片写完后再开启显示，避免上电时短暂显示随机显存内容。
    command(0x29, NULL, 0); // DISPON：开启屏幕显示。
    ESP_LOGI(TAG, "Display enabled (%d x %d RGB565), offset=(%d,%d)",
             LCD_WIDTH, LCD_HEIGHT, LCD_X_OFFSET, LCD_Y_OFFSET);
}
