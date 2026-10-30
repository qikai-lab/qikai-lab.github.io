/*
 * oled_status.c - 0.91" OLED 状态屏（SSD1306 / 128×32 / I2C）
 * ===========================================================================
 * 一屏 4 行 × 21 字符，只显示「网关自己的状态」：
 *
 *   ┌──────────────────────┐
 *   │ 10.236.232.204       │  ← IP（一眼看出挂在哪个网段）
 *   │ -29dB M:OK t1534 d8  │  ← 信号 / MQTT通没通 / 已发 / 丢弃
 *   │ 18.3C 55% L663 G28 P0│  ← 温 / 湿 / 光照 / 烟雾 / 人体
 *   │ #161 bad0 RLY-OFF    │  ← 帧数 / 坏帧 / 继电器
 *   └──────────────────────┘
 *
 * 为什么不做成"好看的分栏界面"：
 *   128×32 只有 4096 个像素点，任何花哨排版都会挤掉信息。演示时别人看这块屏
 *   只需要三个答案——网通不通、数据在不在来、继电器什么状态。四行字全给了。
 *
 * 关于硬件差异（踩过的坑，写在这省得下次重查）：
 *   ① SSD1306 的 128×32 和 128×64 **初始化序列不一样**，关键两处：
 *        0xA8 后跟 0x1F（32 行）而不是 0x3F（64 行）
 *        0xDA 后跟 0x02（sequential）而不是 0x12（alternative）
 *      写错了不报错，但会显示成上下两半重复或者只亮半屏，很难反推。
 *   ② 四针模块地址 99% 是 0x3C，剩下 1% 是 0x3D —— 所以这里先探测再用，
 *      不写死，省得拿到一块不响应的屏要改代码重烧。
 *   ③ 必须开电荷泵（0x8D, 0x14），否则屏全黑但 I2C 一切正常，最难查。
 * ===========================================================================
 */

#include <stdio.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "app_cfg.h"
#include "oled_status.h"

static const char *TAG = "OLED";

#define OLED_W      128
#define OLED_H      32
#define OLED_PAGES  (OLED_H / 8)            /* 4 页，每页 8 像素高 */
#define OLED_BUF_SZ (OLED_W * OLED_PAGES)   /* 512 字节显存 */

/* 6×8 字距：每个字符 5 列点阵 + 1 列空白，128 / 6 = 21 字符一行 */
#define CHAR_W      6
#define CHARS_PER_LINE (OLED_W / CHAR_W)    /* 21 */

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static bool                    s_ok;
static int64_t                 s_next_us;
static uint8_t                 s_buf[OLED_BUF_SZ];

/* ---------------------------------------------------------------------------
 * 5×7 ASCII 字库（0x20 ~ 0x7E，共 95 个）
 *
 * 每个字符 5 个字节 = 5 列，每字节的低 7 位是这一列的 7 个像素（bit0 在最上）。
 * 这是最常见的 5×7 点阵排布，和 Adafruit glcdfont 一致，可以直接替换。
 * 只放可打印字符，控制字符省掉，省 95×5=475 字节 flash（现在只剩 13% 余量）。
 * ------------------------------------------------------------------------ */
static const uint8_t FONT5X7[95][5] = {
    {0x00,0x00,0x00,0x00,0x00}, /* 0x20 ' '  */  {0x00,0x00,0x5F,0x00,0x00}, /* '!' */
    {0x00,0x07,0x00,0x07,0x00}, /* '"'       */  {0x14,0x7F,0x14,0x7F,0x14}, /* '#' */
    {0x24,0x2A,0x7F,0x2A,0x12}, /* '$'       */  {0x23,0x13,0x08,0x64,0x62}, /* '%' */
    {0x36,0x49,0x55,0x22,0x50}, /* '&'       */  {0x00,0x05,0x03,0x00,0x00}, /* '\'' */
    {0x00,0x1C,0x22,0x41,0x00}, /* '('       */  {0x00,0x41,0x22,0x1C,0x00}, /* ')' */
    {0x14,0x08,0x3E,0x08,0x14}, /* '*'       */  {0x08,0x08,0x3E,0x08,0x08}, /* '+' */
    {0x00,0x50,0x30,0x00,0x00}, /* ','       */  {0x08,0x08,0x08,0x08,0x08}, /* '-' */
    {0x00,0x60,0x60,0x00,0x00}, /* '.'       */  {0x20,0x10,0x08,0x04,0x02}, /* '/' */
    {0x3E,0x51,0x49,0x45,0x3E}, /* '0'       */  {0x00,0x42,0x7F,0x40,0x00}, /* '1' */
    {0x42,0x61,0x51,0x49,0x46}, /* '2'       */  {0x21,0x41,0x45,0x4B,0x31}, /* '3' */
    {0x18,0x14,0x12,0x7F,0x10}, /* '4'       */  {0x27,0x45,0x45,0x45,0x39}, /* '5' */
    {0x3C,0x4A,0x49,0x49,0x30}, /* '6'       */  {0x01,0x71,0x09,0x05,0x03}, /* '7' */
    {0x36,0x49,0x49,0x49,0x36}, /* '8'       */  {0x06,0x49,0x49,0x29,0x1E}, /* '9' */
    {0x00,0x36,0x36,0x00,0x00}, /* ':'       */  {0x00,0x56,0x36,0x00,0x00}, /* ';' */
    {0x08,0x14,0x22,0x41,0x00}, /* '<'       */  {0x14,0x14,0x14,0x14,0x14}, /* '=' */
    {0x00,0x41,0x22,0x14,0x08}, /* '>'       */  {0x02,0x01,0x51,0x09,0x06}, /* '?' */
    {0x32,0x49,0x79,0x41,0x3E}, /* '@'       */  {0x7E,0x11,0x11,0x11,0x7E}, /* 'A' */
    {0x7F,0x49,0x49,0x49,0x36}, /* 'B'       */  {0x3E,0x41,0x41,0x41,0x22}, /* 'C' */
    {0x7F,0x41,0x41,0x22,0x1C}, /* 'D'       */  {0x7F,0x49,0x49,0x41,0x41}, /* 'E' */
    {0x7F,0x09,0x09,0x09,0x01}, /* 'F'       */  {0x3E,0x41,0x49,0x49,0x7A}, /* 'G' */
    {0x7F,0x08,0x08,0x08,0x7F}, /* 'H'       */  {0x00,0x41,0x7F,0x41,0x00}, /* 'I' */
    {0x20,0x40,0x41,0x3F,0x01}, /* 'J'       */  {0x7F,0x08,0x14,0x22,0x41}, /* 'K' */
    {0x7F,0x40,0x40,0x40,0x40}, /* 'L'       */  {0x7F,0x02,0x0C,0x02,0x7F}, /* 'M' */
    {0x7F,0x04,0x08,0x10,0x7F}, /* 'N'       */  {0x3E,0x41,0x41,0x41,0x3E}, /* 'O' */
    {0x7F,0x09,0x09,0x09,0x06}, /* 'P'       */  {0x3E,0x41,0x51,0x21,0x5E}, /* 'Q' */
    {0x7F,0x09,0x19,0x29,0x46}, /* 'R'       */  {0x46,0x49,0x49,0x49,0x31}, /* 'S' */
    {0x01,0x01,0x7F,0x01,0x01}, /* 'T'       */  {0x3F,0x40,0x40,0x40,0x3F}, /* 'U' */
    {0x1F,0x20,0x40,0x20,0x1F}, /* 'V'       */  {0x7F,0x20,0x18,0x20,0x7F}, /* 'W' */
    {0x63,0x14,0x08,0x14,0x63}, /* 'X'       */  {0x07,0x08,0x70,0x08,0x07}, /* 'Y' */
    {0x61,0x51,0x49,0x45,0x43}, /* 'Z'       */  {0x00,0x7F,0x41,0x41,0x00}, /* '[' */
    {0x02,0x04,0x08,0x10,0x20}, /* '\\'      */  {0x00,0x41,0x41,0x7F,0x00}, /* ']' */
    {0x04,0x02,0x01,0x02,0x04}, /* '^'       */  {0x40,0x40,0x40,0x40,0x40}, /* '_' */
    {0x00,0x01,0x02,0x04,0x00}, /* '`'       */  {0x20,0x54,0x54,0x54,0x78}, /* 'a' */
    {0x7F,0x48,0x44,0x44,0x38}, /* 'b'       */  {0x38,0x44,0x44,0x44,0x20}, /* 'c' */
    {0x38,0x44,0x44,0x48,0x7F}, /* 'd'       */  {0x38,0x54,0x54,0x54,0x18}, /* 'e' */
    {0x08,0x7E,0x09,0x01,0x02}, /* 'f'       */  {0x0C,0x52,0x52,0x52,0x3E}, /* 'g' */
    {0x7F,0x08,0x04,0x04,0x78}, /* 'h'       */  {0x00,0x44,0x7D,0x40,0x00}, /* 'i' */
    {0x20,0x40,0x44,0x3D,0x00}, /* 'j'       */  {0x7F,0x10,0x28,0x44,0x00}, /* 'k' */
    {0x00,0x41,0x7F,0x40,0x00}, /* 'l'       */  {0x7C,0x04,0x18,0x04,0x78}, /* 'm' */
    {0x7C,0x08,0x04,0x04,0x78}, /* 'n'       */  {0x38,0x44,0x44,0x44,0x38}, /* 'o' */
    {0x7C,0x14,0x14,0x14,0x08}, /* 'p'       */  {0x08,0x14,0x14,0x18,0x7C}, /* 'q' */
    {0x7C,0x08,0x04,0x04,0x08}, /* 'r'       */  {0x48,0x54,0x54,0x54,0x20}, /* 's' */
    {0x04,0x3F,0x44,0x40,0x20}, /* 't'       */  {0x3C,0x40,0x40,0x20,0x7C}, /* 'u' */
    {0x1C,0x20,0x40,0x20,0x1C}, /* 'v'       */  {0x3C,0x40,0x30,0x40,0x3C}, /* 'w' */
    {0x44,0x28,0x10,0x28,0x44}, /* 'x'       */  {0x0C,0x50,0x50,0x50,0x3C}, /* 'y' */
    {0x44,0x64,0x54,0x4C,0x44}, /* 'z'       */  {0x00,0x08,0x36,0x41,0x00}, /* '{' */
    {0x00,0x00,0x7F,0x00,0x00}, /* '|'       */  {0x00,0x41,0x36,0x08,0x00}, /* '}' */
    {0x08,0x08,0x2A,0x1C,0x08}, /* '~'       */
};

/* ---------------------------------------------------------------------------
 * 底层 I2C —— 只有一个出口：oled_page() 负责「定位 + 写满一页 128 字节」
 * ------------------------------------------------------------------------ */

/* 写一页的 128 字节显存。控制字节 0x40 = 后面全是数据 */
static esp_err_t oled_page(uint8_t page, const uint8_t *data)
{
    uint8_t pkt[1 + OLED_W];
    /* 定位到「第 page 页、第 0 列」需要三条命令：0xB0|page、列低=0x00、列高=0x10。
     * 一个控制字节 0x00 后面可以连续跟多条命令，所以三条合成一次事务发。 */
    uint8_t loc[4] = {0x00, (uint8_t)(0xB0 | (page & 0x0F)), 0x00, 0x10};

    if (i2c_master_transmit(s_dev, loc, sizeof(loc), 20) != ESP_OK) {
        return ESP_FAIL;
    }

    pkt[0] = 0x40;
    memcpy(&pkt[1], data, OLED_W);
    return i2c_master_transmit(s_dev, pkt, sizeof(pkt), 50);
}

/* ---------------------------------------------------------------------------
 * 绘图
 * ------------------------------------------------------------------------ */
static void draw_char(int page, int col, char ch)
{
    int idx = (unsigned char)ch - 0x20;
    int x   = col * CHAR_W;
    int i;

    if (idx < 0 || idx >= 95 || page < 0 || page >= OLED_PAGES) {
        return;
    }
    if (x + CHAR_W > OLED_W) {
        return;                                  /* 出界就截断，不越界写 */
    }

    for (i = 0; i < 5; i++) {
        s_buf[page * OLED_W + x + i] = FONT5X7[idx][i];
    }
    s_buf[page * OLED_W + x + 5] = 0x00;          /* 字间距留白 */
}

static void draw_line(int page, const char *s)
{
    int col = 0;

    for (; *s && col < CHARS_PER_LINE; s++, col++) {
        draw_char(page, col, *s);
    }
}

/* 开机画面：init 成功后先亮一下，证明「屏是活的」再去管数据 */
static void show_boot(void)
{
    memset(s_buf, 0, sizeof(s_buf));
    draw_line(1, "ESP32 GW  boot..");
    draw_line(2, "OLED SSD1306 ok");
}

/* ---------------------------------------------------------------------------
 * 初始化
 * ------------------------------------------------------------------------ */
int oled_status_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port          = I2C_NUM_0,
        .sda_io_num        = APP_PIN_OLED_SDA,
        .scl_io_num        = APP_PIN_OLED_SCL,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,   /* 模块自带 4.7k，内部再并一个更抗干扰 */
    };
    uint8_t           addr;
    esp_err_t         err;
    int               i;

    if (s_ok) {
        return 0;
    }

    err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2C 总线初始化失败(%s) —— 没有状态屏，功能不受影响",
                 esp_err_to_name(err));
        return -1;
    }

    /* 先探地址：写死 0x3C 的话，万一拿到 0x3D 的模块会一声不响地全黑 */
    addr = APP_OLED_ADDR;
    if (i2c_master_probe(s_bus, addr, 60) != ESP_OK) {
        if (i2c_master_probe(s_bus, 0x3D, 60) == ESP_OK) {
            addr = 0x3D;
            ESP_LOGI(TAG, "屏地址是 0x3D（不是常见的 0x3C）");
        } else {
            ESP_LOGW(TAG, "0x3C/0x3D 都没应答 —— 查这三点："
                          "① SDA 是不是 %d 脚 ② SCL 是不是 %d 脚 ③ VCC 有没有接 3V3",
                     APP_PIN_OLED_SDA, APP_PIN_OLED_SCL);
            i2c_del_master_bus(s_bus);
            s_bus = NULL;
            return -1;
        }
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = APP_OLED_I2C_HZ,
    };
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "挂载 I2C 设备失败(%s)", esp_err_to_name(err));
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
        return -1;
    }

    /* SSD1306 上电初始化序列（128×32 专用，见文件头注释的两个关键参数）。
     * 写成一维字节流：不带参数的命令一个字节，带参数的命令紧跟一个参数字节。
     * SSD1306 允许一次事务里连发多条命令，所以整串一次发完（比逐条发快得多）。 */
    static const uint8_t init_seq[] = {
        0xAE,               /* 关显示 */
        0xD5, 0x80,         /* 时钟分频 */
        0xA8, 0x1F,         /* MUX = 31 → 32 行 ★128×32 必须 0x1F */
        0xD3, 0x00,         /* 显示偏移 0 */
        0x40,               /* 起始行 0 */
        0x8D, 0x14,         /* ★电荷泵开，不开就全黑但 I2C 一切正常，最难查 */
        0x20, 0x00,         /* 水平寻址模式 */
        0xA1,               /* 段重映射（左右方向） */
        0xC8,               /* COM 反向扫描（上下方向） */
        0xDA, 0x02,         /* ★COM 引脚配置 128×32 用 0x02，128×64 才是 0x12 */
        0x81, 0x8F,         /* 对比度 */
        0xD9, 0xF1,         /* 预充电周期 */
        0xDB, 0x40,         /* VCOMH */
        0xA4,               /* 显示跟随显存（不是全亮） */
        0xA6,               /* 正常显示（不反色） */
        0x2E,               /* 关滚动 */
        0xAF,               /* 开显示 */
    };

    {
        uint8_t pkt[1 + sizeof(init_seq)];

        pkt[0] = 0x00;                                  /* 控制字节：后续全是命令 */
        memcpy(&pkt[1], init_seq, sizeof(init_seq));
        err = i2c_master_transmit(s_dev, pkt, sizeof(pkt), 200);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "初始化命令发送失败(%s) —— 放弃状态屏，功能不受影响",
                 esp_err_to_name(err));
        i2c_master_bus_rm_device(s_dev);
        i2c_del_master_bus(s_bus);
        s_dev = NULL;
        s_bus = NULL;
        return -1;
    }

    /* 清屏 */
    memset(s_buf, 0, sizeof(s_buf));
    for (i = 0; i < OLED_PAGES; i++) {
        (void)oled_page((uint8_t)i, &s_buf[i * OLED_W]);
    }

    s_ok      = true;
    /* 开机画面先留 2 秒，别刚亮就被第一帧数据顶掉 —— 那 2 秒是给现场看的人
     * 确认「屏确实在工作」用的，跳过了反而分不清是屏没亮还是数据没来 */
    s_next_us = esp_timer_get_time() + 2000000;

    /* 开机画面停留一下，让人看见「屏是活的」 */
    show_boot();
    for (i = 0; i < OLED_PAGES; i++) {
        (void)oled_page((uint8_t)i, &s_buf[i * OLED_W]);
    }

    ESP_LOGI(TAG, "状态屏就绪：SSD1306 128x32 @0x%02X，SDA=%d SCL=%d",
             addr, APP_PIN_OLED_SDA, APP_PIN_OLED_SCL);
    return 0;
}

bool oled_status_ok(void)
{
    return s_ok;
}

bool oled_status_due(void)
{
    int64_t now;

    if (!s_ok) {
        return false;
    }
    now = esp_timer_get_time();
    if (now < s_next_us) {
        return false;
    }
    s_next_us = now + (int64_t)APP_OLED_REFRESH_MS * 1000;
    return true;
}

/* ---------------------------------------------------------------------------
 * 画一屏
 * ------------------------------------------------------------------------ */
void oled_status_render(const oled_info_t *in)
{
    /* 缓冲开得比 21 字符宽很多：draw_line() 只画前 21 个字符，
     * 但 snprintf 的 %d/%lu 在编译器眼里可能有 11 位数字，开小了会触发
     * -Werror=format-truncation。反正这 200 多字节是栈上临时量，不心疼。 */
    char l0[24];
    char l1[64];
    char l2[80];
    char l3[64];
    int  i;

    if (!s_ok || in == NULL) {
        return;
    }

    /* 行 0：IP。这是最有信息量的一行 —— 一眼看出挂在哪个网段 */
    if (in->wifi_ok && in->ip != NULL && in->ip[0] != '\0') {
        snprintf(l0, sizeof(l0), "%s", in->ip);
    } else if (in->wifi_ok) {
        snprintf(l0, sizeof(l0), "WiFi OK no IP");
    } else {
        snprintf(l0, sizeof(l0), "WiFi ..  connecting");
    }

    /* 行 1：信号 / MQTT / 已发 / 丢弃 */
    snprintf(l1, sizeof(l1), "%ddB M:%s t%lu d%lu",
             in->rssi,
             in->mqtt_ok ? "OK" : "--",
             (unsigned long)in->tx_ok,
             (unsigned long)in->dropped);

    if (in->frames == 0) {
        /* 一帧都没收到。这是最需要一眼看出来的故障，
         * 千万别把 0 当成「温度 0℃」显示出去 —— 那会让人以为传感器坏了 */
        snprintf(l2, sizeof(l2), "no frame from node");
        snprintf(l3, sizeof(l3), "check P0_3 -> GPIO%d", APP_PIN_RX);
    } else {
        /* 行 2：温 / 湿 / 光照 / 烟雾 / 人体
         * 注：室内场景不会到零下，所以没做负温的 '-' 处理；真要用在室外再说 */
        snprintf(l2, sizeof(l2), "%d.%dC %d%% L%u G%u P%d",
                 in->temp_x10 / 10, (in->temp_x10 % 10 + 10) % 10,
                 in->hum_x10 / 10,
                 (unsigned)in->lux, (unsigned)in->gas, in->pir);

        /* 行 3：帧数 / 坏帧 / 继电器 */
        snprintf(l3, sizeof(l3), "#%lu bad%lu RLY-%s",
                 (unsigned long)in->frames,
                 (unsigned long)in->bad,
                 in->relay ? "ON" : "OFF");
    }

    memset(s_buf, 0, sizeof(s_buf));
    draw_line(0, l0);
    draw_line(1, l1);
    draw_line(2, l2);
    draw_line(3, l3);

    for (i = 0; i < OLED_PAGES; i++) {
        if (oled_page((uint8_t)i, &s_buf[i * OLED_W]) != ESP_OK) {
            /* 屏掉了（线松、掉电）。**不重试、不打印**，否则串口会被刷爆，
             * 但把 s_ok 留着 —— 下次刷新若又能通就自己恢复 */
            return;
        }
    }
}
