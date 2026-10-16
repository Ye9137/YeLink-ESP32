// Copyright (c) 2026 Ye
// 小屏伙伴：独立显示任务、开机配网门户、HTTPS 指令轮询及图片下载。
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "cJSON.h"
#include "display.h"
#include "moon_cat.h"
#include "moon_cat_gif.h"
#include "fps_badge.h"
#include "custom_animation.h"
#include "wifi_share.h"
#include "router_monitor.h"
#include "esp_crt_bundle.h"
#include "esp_clk_tree.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "nvs_flash.h"

#define WIFI_READY BIT0
#define SERVER_READY BIT2
#define AP_READY BIT3
#define YE_FIRMWARE_VERSION 17
static const char *TAG = "pet";

// 旧结构只用于升级迁移，避免新增模式字段后清空既有配置。
typedef struct {
    char ssid[33], password[64], server[192], token[128];
} legacy_settings_t;
enum { MODE_ROUTER=1, MODE_CLOUD=2 };
typedef struct {
    char ssid[33], password[64], server[192], token[128];
    uint8_t mode, sharing_enabled;
} settings_t;
static settings_t settings;
static EventGroupHandle_t events;
static SemaphoreHandle_t screen_lock;
static httpd_handle_t portal;
static uint8_t *frame;
// 热点使用固定名称和 WPA2 密码；WPA2-PSK 要求密码至少 8 字节。
static const char ap_name[] = "Ye的共享wifi";
static const char ap_password[] = "11111111";
static volatile uint8_t last_wifi_reason;
static bool image_mode, enabled = true, manual_blink;
static int expression, look, theme;
static int64_t revision = -1;
// 字幕与角色分层；动画资源仅在需要自定义 GIF 时占用堆内存。
#define CAPTION_BYTES (128 * 40 * 2)
static uint8_t caption_pixels[CAPTION_BYTES];
static int64_t caption_revision, animation_revision, scene_deadline;
static custom_animation_t custom_animation;
static int renderer, speed=100, eye_open=100, sway=1, paw_offset;
static uint16_t eye_color=0x3b14;
static bool paused, screen_dirty;
static char command_error[80];
// 高频指令查询与资源下载任务分离，队列仅保留最新指令。
#define COMMAND_CAPACITY 2048
static QueueHandle_t commands;
extern const char portal_html[] asm("_binary_portal_html_start");

// 5×7 基础字形用于配网说明；任意中文交由后台渲染成 RGB565。
static const uint8_t font[][5] = {
    {0x3e,0x51,0x49,0x45,0x3e},{0,0x42,0x7f,0x40,0},{0x42,0x61,0x51,0x49,0x46},
    {0x21,0x41,0x45,0x4b,0x31},{0x18,0x14,0x12,0x7f,0x10},{0x27,0x45,0x45,0x45,0x39},
    {0x3c,0x4a,0x49,0x49,0x30},{1,0x71,9,5,3},{0x36,0x49,0x49,0x49,0x36},{6,0x49,0x49,0x29,0x1e},
    {0x7e,0x11,0x11,0x11,0x7e},{0x7f,0x49,0x49,0x49,0x36},{0x3e,0x41,0x41,0x41,0x22},
    {0x7f,0x41,0x41,0x22,0x1c},{0x7f,0x49,0x49,0x49,0x41},{0x7f,9,9,9,1},
    {0x3e,0x41,0x49,0x49,0x7a},{0x7f,8,8,8,0x7f},{0,0x41,0x7f,0x41,0},
    {0x20,0x40,0x41,0x3f,1},{0x7f,8,0x14,0x22,0x41},{0x7f,0x40,0x40,0x40,0x40},
    {0x7f,2,0x0c,2,0x7f},{0x7f,4,8,0x10,0x7f},{0x3e,0x41,0x41,0x41,0x3e},
    {0x7f,9,9,9,6},{0x3e,0x41,0x51,0x21,0x5e},{0x7f,9,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31},{1,1,0x7f,1,1},{0x3f,0x40,0x40,0x40,0x3f},
    {0x1f,0x20,0x40,0x20,0x1f},{0x3f,0x40,0x38,0x40,0x3f},{0x63,0x14,8,0x14,0x63},
    {7,8,0x70,8,7},{0x61,0x51,0x49,0x45,0x43}
};

// 所有绘图函数仅由持有 screen_lock 的显示任务调用。
static void pixel(int x, int y, uint16_t color)
{
    if (x < 0 || x >= SCREEN_WIDTH || y < 0 || y >= SCREEN_HEIGHT) return;
    size_t index = (y * SCREEN_WIDTH + x) * 2;
    frame[index] = color >> 8;
    frame[index + 1] = color & 255;
}
static void text_scaled_at(int x, int y, const char *text, uint16_t color, int scale)
{
    static const uint8_t lowercase_e[5] = {0x38,0x54,0x54,0x54,0x18};
    for (; *text; text++, x += 6*scale) {
        int glyph = *text >= '0' && *text <= '9' ? *text - '0' :
                    *text >= 'A' && *text <= 'Z' ? *text - 'A' + 10 : -1;
        for (int col = 0; col < 5; col++) for (int row = 0; row < 7; row++) {
            if ((glyph >= 0 && (font[glyph][col] & (1 << row))) ||
                (*text == 'e' && (lowercase_e[col] & (1 << row))) ||
                (*text == '-' && row == 3) || (*text == '.' && col == 2 && row == 6) ||
                (*text == ':' && col == 2 && (row == 2 || row == 5)) ||
                (*text == '%' && ((col<2 && row<2) || (col>2 && row>4) || row==6-col)) ||
                (*text == '/' && row == 6-col))
                for (int dx=0;dx<scale;dx++) for (int dy=0;dy<scale;dy++)
                    pixel(x+col*scale+dx,y+row*scale+dy,color);
        }
    }
}
static void text_at(int x, int y, const char *text, uint16_t color)
{
    text_scaled_at(x,y,text,color,1);
}

static void format_rate(char *out, size_t capacity, uint32_t bytes_per_sec)
{
    if (bytes_per_sec<1024) snprintf(out,capacity,"%lu B/S",(unsigned long)bytes_per_sec);
    else {
        uint32_t divisor=bytes_per_sec<1024*1024?1024:1024*1024;
        unsigned long tenths=(uint64_t)bytes_per_sec*10/divisor;
        snprintf(out,capacity,"%lu.%lu %c/S",tenths/10,tenths%10,divisor==1024?'K':'M');
    }
}

static bool draw_pet(int64_t now, bool force);

static void router_display_loop(void)
{
    TickType_t tick=xTaskGetTickCount();
    bool first=true, was_connected=false;
    int64_t sampled_at=-1000000, status_at=0;
    router_snapshot_t snapshot={0};
    for (;;) {
        int64_t now=esp_timer_get_time();
        bool connected=(xEventGroupGetBits(events)&WIFI_READY)!=0;
        bool switched=first || connected!=was_connected;
        // 离线播放时也维持一秒采样，重连后不把整段断网时间算入瞬时网速。
        if (now-sampled_at>=1000000) {
            router_monitor_snapshot(&snapshot);
            sampled_at=now;
        }
        if (!connected) {
            // 复用原动画及配网提示；切换时强制重绘，清掉状态屏内容。
            if (draw_pet(now,switched)) display_frame(frame);
        } else if (switched || now-status_at>=1000000) {
            status_at=now;
            memset(frame,0,SCREEN_BYTES);
            char rate[16],metrics[32];
            text_at(4,3,"UP",0x5ef7);
            format_rate(rate,sizeof(rate),snapshot.upload_bytes_per_sec);
            text_scaled_at(4,14,rate,0xffff,2);
            text_at(4,34,"DOWN",0x5ef7);
            format_rate(rate,sizeof(rate),snapshot.download_bytes_per_sec);
            text_scaled_at(4,45,rate,0xffff,2);
            for (int x=4;x<124;x++) pixel(x,65,0x4208);
            if (snapshot.cpu_sample_valid)
                snprintf(metrics,sizeof(metrics),"CPU %u%% %luMHZ",(unsigned)snapshot.cpu_total_percent,(unsigned long)snapshot.cpu_mhz);
            else snprintf(metrics,sizeof(metrics),"CPU --%% %luMHZ",(unsigned long)snapshot.cpu_mhz);
            text_at(4,70,metrics,0xffff);
            if (snapshot.cpu_sample_valid)
                snprintf(metrics,sizeof(metrics),"C0 %u%% C1 %u%%",(unsigned)snapshot.cpu_percent[0],(unsigned)snapshot.cpu_percent[1]);
            else strlcpy(metrics,"C0 --% C1 --%",sizeof(metrics));
            text_at(4,82,metrics,0xad55);
            snprintf(metrics,sizeof(metrics),"FREE %luK MIN %luK",(unsigned long)(snapshot.heap_free/1024),(unsigned long)(snapshot.heap_min/1024));
            text_at(4,94,metrics,0xad55);
            for (int x=4;x<124;x++) pixel(x,106,0x4208);
            text_at(4,112,"LOGS",0x5ef7);
            for (unsigned i=0;i<snapshot.log_count;i++)
                text_at(4,124+(int)i*9,snapshot.logs[i],i+1==snapshot.log_count?0xffff:0xad55);
            display_frame(frame);
        }
        if (first) { display_enable(); first=false; }
        was_connected=connected;
        // 在线仅检查切换、每秒刷新状态；离线按原 GIF 的 40ms 帧间隔播放。
        // 不创建动画定时器或新任务；在线不解码 GIF，也不绘制 FPS。
        vTaskDelayUntil(&tick,pdMS_TO_TICKS(connected?100:40));
    }
}
// 角色绘制与联网逻辑分离，保持原有配网和后台控制协议。
static bool draw_pet(int64_t now, bool force)
{
    static int last_gif_frame = -1;
    static int old_renderer=-1;
    static int64_t previous_time, animation_time;
    if (old_renderer!=renderer) { animation_time=0; last_gif_frame=-1; force=true; old_renderer=renderer; }
    if (previous_time && !paused) animation_time+=(now-previous_time)*speed/100;
    previous_time=now;
    if (renderer != 1) {
        if (manual_blink) { animation_time=0; force=true; }
        int index=renderer==2 && custom_animation.data ? custom_animation_frame(&custom_animation,animation_time) : moon_cat_gif_frame(animation_time);
        manual_blink = false;
        if (!force && index == last_gif_frame) return false;
        if (renderer==2 && custom_animation.data) {
            custom_animation_draw(&custom_animation,frame,index); last_gif_frame=index;
        } else if (moon_cat_gif_render(frame, index)) last_gif_frame = index;
        else {
            // 资源损坏时仍能显示角色和配网提示。
            last_gif_frame = -1;
            moon_cat_render(frame,now,expression,look,theme,false);
        }
    } else {
        last_gif_frame = -1;
        if (paused && !force && !manual_blink) return false;
        moon_cat_set_style(eye_open,sway,paw_offset,eye_color);
        moon_cat_render(frame,animation_time,expression,look,theme,manual_blink);
    }
    manual_blink=false;
    // 米白画布使用深棕文字，确保配网信息在亮背景上清晰可读。
    EventBits_t bits=xEventGroupGetBits(events);
    if (!(bits&WIFI_READY)) {
        // 顶部左侧留给 FPS，配网标识右移，详细连接信息仍在底部。
        text_at(70,7,"WIFI",0x6a26);
        // 5×7 字库只含 ASCII，中文 SSID 在配网页完整显示。
        text_at(7,120,"YE SHARE WIFI",0x6a26);
        text_at(7,132,"PASS 11111111",0x6a26);
        text_at(7,145,"192.168.4.1",0x6a26);
    } else if (caption_revision) {
        // Wi-Fi 连通后清掉配网/连接状态字，只保留用户发送的底部字幕。
        memcpy(frame+120*128*2,caption_pixels,CAPTION_BYTES);
    }
    return true;
}
// 高分辨率定时器通知显示任务，避免 FreeRTOS 100Hz 节拍把 60FPS 量化到 50FPS。
static void frame_tick(void *arg)
{
    xTaskNotifyGive((TaskHandle_t)arg);
}
static void display_task(void *arg)
{
    display_init();
    if (settings.mode==MODE_ROUTER) router_display_loop();
    esp_timer_handle_t timer;
    esp_timer_create_args_t timer_config={.callback=frame_tick,.arg=xTaskGetCurrentTaskHandle(),
        .dispatch_method=ESP_TIMER_TASK,.name="pet_frame",.skip_unhandled_events=true};
    ESP_ERROR_CHECK(esp_timer_create(&timer_config,&timer));
    const int target_fps=CONFIG_YE_LCD_TARGET_FPS;
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer,1000000/target_fps));
    bool first=true, old_image=false, old_enabled=true;
    EventBits_t old_bits=0;
    int64_t old_revision=-2, measured_at=esp_timer_get_time(), logged_at=measured_at;
    int shown_fps=-1;
    double measured_fps=0;
    unsigned frames=0;
    for (;;) {
        // 合并错过的节拍，不在网络繁忙后补绘一串过时帧。
        ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
        xSemaphoreTake(screen_lock, portMAX_DELAY);
        EventBits_t bits=xEventGroupGetBits(events);
        // 二十秒倒计时在显示任务本地执行；断网也能自动回到主画面。
        if (scene_deadline && esp_timer_get_time()>=scene_deadline) {
            scene_deadline=0; image_mode=false; renderer=0; enabled=true; paused=false;
            speed=100; caption_revision=0; manual_blink=false; screen_dirty=true;
            // 保留至多 48KiB 的 GIF 缓存供重播；仅联网任务在替换资源时释放，
            // 避免到期线程与正在处理下一条指令的下载线程同时修改资源指针。
        }
        bool full=first || screen_dirty || old_image!=image_mode || old_enabled!=enabled || old_bits!=bits || old_revision!=revision;
        screen_dirty=false;
        int64_t sample_at=esp_timer_get_time();
        bool fps_changed=false;
        // 切换图片/动画或开关显示时重新采样，避免沿用上一模式的数值。
        if (old_image!=image_mode || old_enabled!=enabled) {
            frames=0; measured_at=sample_at; shown_fps=-1; measured_fps=0;
        }
        if (sample_at-measured_at>=1000000) {
            measured_fps=frames*1000000.0/(sample_at-measured_at);
            int next_fps=(int)(measured_fps+0.5);
            fps_changed=next_fps!=shown_fps;
            shown_fps=next_fps; frames=0; measured_at=sample_at;
        }
        bool changed = enabled && !image_mode && draw_pet(esp_timer_get_time(),full);
        if (!enabled && full) memset(frame,0,SCREEN_BYTES);
        // 动画渲染会重建整个帧缓冲，因此每次重绘都补上徽标；关屏保持全黑。
        if (enabled && (full || changed || fps_changed)) fps_badge_draw(frame,shown_fps);
        // GIF 现在铺满屏幕；必须提交所有行，否则四周会残留上一帧。
        if (full || (changed && renderer!=1)) display_frame(frame);
        else if (changed) display_rows(frame,16,104);
        // 数字变更时单独提交顶部，避免为一个角标刷新整屏。
        if (!full && !(changed && renderer!=1) && enabled && fps_changed) display_rows(frame,0,16);
        // 只统计已经完成 DMA 的内容画面，角标自身更新不计入，静态图可降至 0。
        if (enabled && (full || changed)) frames++;
        if (first) { display_enable(); first=false; }
        old_image=image_mode; old_enabled=enabled; old_bits=bits; old_revision=revision;
        int64_t now=esp_timer_get_time();
        if (now-logged_at>=5000000) {
            // 这是已完成 SPI 传输的提交速率，不宣称等于面板内部扫描频率。
            ESP_LOGI(TAG,"Display updates %.1f FPS (target %d, %s)",
                measured_fps,target_fps,image_mode?"static":"pet");
            logged_at=now;
        }
        xSemaphoreGive(screen_lock);
    }
}

// 从 NVS 读取一整个配置结构，防止部分字段更新后产生混合配置。
static void load_settings(void)
{
    memset(&settings,0,sizeof(settings));
    settings.mode=MODE_CLOUD;
    nvs_handle_t handle;
    if (nvs_open("pet", NVS_READONLY, &handle) != ESP_OK) return;
    size_t length=0;
    if (nvs_get_blob(handle,"settings",NULL,&length)==ESP_OK) {
        if (length==sizeof(settings)) {
            nvs_get_blob(handle,"settings",&settings,&length);
        } else if (length==sizeof(legacy_settings_t)) {
            legacy_settings_t old={0};
            nvs_get_blob(handle,"settings",&old,&length);
            memcpy(settings.ssid,old.ssid,sizeof(old.ssid));
            memcpy(settings.password,old.password,sizeof(old.password));
            memcpy(settings.server,old.server,sizeof(old.server));
            memcpy(settings.token,old.token,sizeof(old.token));
            settings.mode=old.server[0]&&old.token[0]?MODE_CLOUD:MODE_ROUTER;
            settings.sharing_enabled=settings.mode==MODE_ROUTER;
        }
    }
    nvs_close(handle);
    if (settings.mode!=MODE_ROUTER && settings.mode!=MODE_CLOUD) settings.mode=MODE_CLOUD;
    settings.ssid[32] = settings.password[63] = settings.server[191] = settings.token[127] = 0;
}
static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_START) {
        xEventGroupSetBits(events, AP_READY);
        router_monitor_log("HOTSPOT READY");
    }
    if (base==WIFI_EVENT && id==WIFI_EVENT_AP_STACONNECTED && data)
        router_monitor_log("CLIENT JOIN %u",(unsigned)((wifi_event_ap_staconnected_t *)data)->aid);
    if (base==WIFI_EVENT && id==WIFI_EVENT_AP_STADISCONNECTED && data)
        router_monitor_log("CLIENT LEFT %u",(unsigned)((wifi_event_ap_stadisconnected_t *)data)->aid);
    if (base==IP_EVENT && id==IP_EVENT_AP_STAIPASSIGNED) router_monitor_log("CLIENT IP OK");
    // Wi-Fi 驱动启动后立即连接上游；失败时网络任务定时重试。
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START && settings.ssid[0]) {
        router_monitor_log("WIFI CONNECT");
        esp_wifi_connect();
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        last_wifi_reason=0;
        xEventGroupSetBits(events, WIFI_READY);
        router_monitor_log("UPLINK IP OK");
        wifi_share_update(settings.mode==MODE_ROUTER && settings.sharing_enabled);
        if (settings.mode==MODE_ROUTER && settings.sharing_enabled)
            router_monitor_log(wifi_share_ready()?"NAT DNS READY":wifi_share_conflict()?"SUBNET CLASH":"SHARE FAILED");
    }
    if ((base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) ||
        (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP)) {
        if (base==WIFI_EVENT && data) last_wifi_reason=((wifi_event_sta_disconnected_t *)data)->reason;
        if (base==WIFI_EVENT) router_monitor_log("WIFI LOST %u",(unsigned)last_wifi_reason);
        else router_monitor_log("UPLINK IP LOST");
        xEventGroupClearBits(events, WIFI_READY | SERVER_READY);
        wifi_share_update(false);
    }
}

static const char *wifi_reason_text(uint8_t reason)
{
    // 给常见错误明确提示；未知代码仍随 API 返回，便于串口进一步定位。
    if (!reason) return "";
    if (reason==WIFI_REASON_NO_AP_FOUND) return "找不到上游 Wi-Fi，请检查名称和 2.4GHz 信号";
    if (reason==WIFI_REASON_AUTH_FAIL || reason==WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT)
        return "上游 Wi-Fi 认证失败，请检查密码或加密方式";
    if (reason==WIFI_REASON_ASSOC_FAIL) return "上游路由器拒绝连接";
    if (reason==WIFI_REASON_BEACON_TIMEOUT) return "上游 Wi-Fi 信号中断";
    return "上游 Wi-Fi 尚未连接";
}

static const char *reset_reason_text(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "上电启动";
    case ESP_RST_SW: return "软件重启";
    case ESP_RST_PANIC: return "程序异常";
    case ESP_RST_INT_WDT: return "中断看门狗";
    case ESP_RST_TASK_WDT: return "任务看门狗";
    case ESP_RST_WDT: return "看门狗复位";
    case ESP_RST_BROWNOUT: return "供电电压不足";
    case ESP_RST_DEEPSLEEP: return "深睡眠唤醒";
    default: return "其他复位原因";
    }
}

// HTTP 门户只接受从 SoftAP 地址进入的请求，避免局域网侧修改配置。
static bool from_ap(httpd_req_t *req)
{
    struct sockaddr_storage local = {0}; socklen_t length = sizeof(local);
    if (getsockname(httpd_req_to_sockfd(req), (struct sockaddr *)&local, &length) != 0) return false;
    if (local.ss_family == AF_INET)
        return ((struct sockaddr_in *)&local)->sin_addr.s_addr == inet_addr("192.168.4.1");
    // 同时兼容双栈监听器中 IPv4 映射为 IPv6 的 socket 地址。
    if (local.ss_family == AF_INET6) {
        static const uint8_t mapped[16]={0,0,0,0,0,0,0,0,0,0,255,255,192,168,4,1};
        return memcmp(&((struct sockaddr_in6 *)&local)->sin6_addr,mapped,sizeof(mapped)) == 0;
    }
    return false;
}
static esp_err_t send_json(httpd_req_t *req, const char *status, const char *json)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}
static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1600));
    esp_restart();
}
static bool get_string(cJSON *json, const char *key, char *out, size_t capacity, bool required)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(json, key);
    if (!cJSON_IsString(item) || strlen(item->valuestring) >= capacity ||
        (required && !item->valuestring[0])) return false;
    strlcpy(out, item->valuestring, capacity);
    return true;
}
static esp_err_t config_post(httpd_req_t *req)
{
    char header[8];
    if (!from_ap(req) || httpd_req_get_hdr_value_str(req, "X-Pet-Setup", header, sizeof(header)) != ESP_OK || strcmp(header, "1"))
        return send_json(req, "403 Forbidden", "{\"error\":\"请连接设备热点后操作\"}");
    if (req->content_len <= 0 || req->content_len > 1500)
        return send_json(req, "400 Bad Request", "{\"error\":\"配置过长\"}");
    char body[1501]; size_t offset = 0;
    while (offset < req->content_len) {
        int n = httpd_req_recv(req, body + offset, req->content_len - offset);
        if (n <= 0) return ESP_FAIL;
        offset += n;
    }
    body[offset] = 0;
    cJSON *json = cJSON_Parse(body);
    settings_t next = {0};
    bool valid = json && get_string(json,"ssid",next.ssid,sizeof(next.ssid),true) &&
        get_string(json,"password",next.password,sizeof(next.password),false) &&
        get_string(json,"server",next.server,sizeof(next.server),false) &&
        get_string(json,"token",next.token,sizeof(next.token),false);
    cJSON *mode=json?cJSON_GetObjectItemCaseSensitive(json,"mode"):NULL;
    if (!cJSON_IsString(mode) || (strcmp(mode->valuestring,"router") && strcmp(mode->valuestring,"cloud"))) valid=false;
    next.mode=cJSON_IsString(mode)&&!strcmp(mode->valuestring,"router")?MODE_ROUTER:MODE_CLOUD;
    next.sharing_enabled=next.mode==MODE_ROUTER && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json,"sharing"));
    cJSON_Delete(json);
    // 云端模式要求后台信息；路由模式保留旧云端配置，但不发起云端请求。
    size_t n = strlen(next.server);
    while (n > 8 && next.server[n-1] == '/') next.server[--n] = 0;
    if (valid && next.server[0] && !next.token[0] && !strcmp(next.server,settings.server))
        strlcpy(next.token,settings.token,sizeof(next.token));
    bool backend_valid=!strncmp(next.server,"https://",8) && strlen(next.server)>8 && next.token[0] &&
        !strpbrk(next.server,"\r\n ?#@") && !strpbrk(next.token,"\r\n ");
    valid=valid && (next.mode==MODE_ROUTER || backend_valid) && (!next.password[0] || strlen(next.password)>=8);
    if (!valid) return send_json(req,"400 Bad Request","{\"error\":\"检查 Wi-Fi 密码、运行模式以及云端地址和设备令牌\"}");
    nvs_handle_t handle;
    esp_err_t error = nvs_open("pet",NVS_READWRITE,&handle);
    if (error == ESP_OK) {
        error = nvs_set_blob(handle,"settings",&next,sizeof(next));
        if (error == ESP_OK) error = nvs_commit(handle);
        nvs_close(handle);
    }
    if (error != ESP_OK) return send_json(req,"500 Internal Server Error","{\"error\":\"无法保存配置\"}");
    esp_err_t result = send_json(req,"200 OK","{\"ok\":true}");
    if (xTaskCreate(restart_task,"restart",2048,NULL,3,NULL) != pdPASS)
        ESP_LOGE(TAG,"Saved; manual restart required");
    return result;
}
static esp_err_t portal_get(httpd_req_t *req)
{
    if (!from_ap(req)) return send_json(req,"403 Forbidden","{}");
    if (!strcmp(req->uri,"/api/info")) {
        cJSON *json = cJSON_CreateObject();
        if (!json) return ESP_ERR_NO_MEM;
        cJSON_AddStringToObject(json,"ssid",settings.ssid);
        cJSON_AddNumberToObject(json,"firmware",YE_FIRMWARE_VERSION);
        cJSON_AddNumberToObject(json,"wifi_task_core",WIFI_TASK_CORE_ID);
        cJSON_AddNumberToObject(json,"tcpip_task_core",CONFIG_LWIP_TCPIP_TASK_AFFINITY);
        uint32_t cpu_hz=0;
        if (esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_CPU,ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED,&cpu_hz)==ESP_OK)
            cJSON_AddNumberToObject(json,"cpu_mhz",cpu_hz/1000000);
        cJSON_AddNumberToObject(json,"reset_reason_code",esp_reset_reason());
        cJSON_AddStringToObject(json,"reset_reason",reset_reason_text());
        cJSON_AddNumberToObject(json,"heap_free",esp_get_free_heap_size());
        cJSON_AddNumberToObject(json,"heap_min",esp_get_minimum_free_heap_size());
        cJSON_AddStringToObject(json,"server",settings.server);
        cJSON_AddStringToObject(json,"mode",settings.mode==MODE_ROUTER?"router":"cloud");
        cJSON_AddBoolToObject(json,"sharing_enabled",settings.mode==MODE_ROUTER&&settings.sharing_enabled);
        cJSON_AddBoolToObject(json,"connected",(xEventGroupGetBits(events)&WIFI_READY)!=0);
        cJSON_AddBoolToObject(json,"sharing",wifi_share_ready());
        cJSON_AddBoolToObject(json,"subnet_conflict",wifi_share_conflict());
        if (settings.mode==MODE_ROUTER) {
            wifi_share_stats_t stats;
            if (wifi_share_get_stats(&stats)) {
                cJSON_AddNumberToObject(json,"napt_capacity",CONFIG_YE_NAPT_CAPACITY);
                cJSON_AddNumberToObject(json,"napt_active",stats.active_tcp+stats.active_udp+stats.active_icmp);
                cJSON_AddNumberToObject(json,"napt_tcp",stats.active_tcp);
                cJSON_AddNumberToObject(json,"napt_udp",stats.active_udp);
                cJSON_AddNumberToObject(json,"napt_forced_evictions",stats.forced_evictions);
                cJSON_AddNumberToObject(json,"udp_timeout_seconds",CONFIG_YE_NAPT_UDP_TIMEOUT_SECONDS);
                cJSON_AddNumberToObject(json,"tcp_transient_timeout_seconds",CONFIG_YE_NAPT_TCP_TRANSIENT_SECONDS);
                cJSON_AddNumberToObject(json,"ipv4_fragments",stats.ipv4_fragments);
                cJSON_AddNumberToObject(json,"ipv4_reassembled",stats.ipv4_reassembled);
                cJSON_AddNumberToObject(json,"ipv4_fragment_input_drops",stats.ipv4_fragment_input_drops);
                cJSON_AddNumberToObject(json,"tcp_mss_limit",CONFIG_YE_ROUTER_TCP_MSS);
                cJSON_AddNumberToObject(json,"tcp_mss_clamped",stats.tcp_mss_clamped);
            }
        }
        cJSON_AddNumberToObject(json,"wifi_reason",last_wifi_reason);
        cJSON_AddStringToObject(json,"wifi_error",wifi_reason_text(last_wifi_reason));
        wifi_sta_list_t clients={0};
        if (esp_wifi_ap_get_sta_list(&clients)==ESP_OK) {
            cJSON_AddNumberToObject(json,"clients",clients.num);
            cJSON *peers=cJSON_AddArrayToObject(json,"client_links");
            for (int i=0;peers && i<clients.num;i++) {
                cJSON *peer=cJSON_CreateObject();
                if (!peer) break;
                cJSON_AddNumberToObject(peer,"rssi",clients.sta[i].rssi);
                cJSON_AddBoolToObject(peer,"supports_11n",clients.sta[i].phy_11n);
                cJSON_AddItemToArray(peers,peer);
            }
        }
        esp_netif_t *station=esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t address={0};
        char ip[16]="";
        if (station && esp_netif_get_ip_info(station,&address)==ESP_OK && address.ip.addr)
            snprintf(ip,sizeof(ip),IPSTR,IP2STR(&address.ip));
        cJSON_AddStringToObject(json,"upstream_ip",ip);
        uint32_t dns_address=wifi_share_dns();
        char dns[16]="";
        if (dns_address) {
            esp_ip4_addr_t dns_ip={.addr=dns_address};
            snprintf(dns,sizeof(dns),IPSTR,IP2STR(&dns_ip));
        }
        cJSON_AddStringToObject(json,"client_dns",dns);
        wifi_ap_record_t upstream={0};
        if (esp_wifi_sta_get_ap_info(&upstream)==ESP_OK) {
            cJSON_AddNumberToObject(json,"wifi_rssi",upstream.rssi);
            cJSON_AddNumberToObject(json,"wifi_channel",upstream.primary);
        }
        uint8_t primary=0;
        wifi_second_chan_t second=WIFI_SECOND_CHAN_NONE;
        if (esp_wifi_get_channel(&primary,&second)==ESP_OK)
            cJSON_AddNumberToObject(json,"wifi_bandwidth_mhz",second==WIFI_SECOND_CHAN_NONE?20:40);
        char *encoded = cJSON_PrintUnformatted(json);
        esp_err_t result = encoded ? send_json(req,"200 OK",encoded) : ESP_ERR_NO_MEM;
        free(encoded); cJSON_Delete(json); return result;
    }
    // 手机探测地址统一跳转到门户，根地址返回实际表单。
    if (strcmp(req->uri,"/")) {
        httpd_resp_set_status(req,"302 Found");
        httpd_resp_set_hdr(req,"Location","http://192.168.4.1/");
        return httpd_resp_sendstr(req,"");
    }
    httpd_resp_set_type(req,"text/html; charset=utf-8");
    httpd_resp_set_hdr(req,"Cache-Control","no-store");
    return httpd_resp_sendstr(req,portal_html);
}

static void network_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta=esp_netif_create_default_wifi_sta();
    esp_netif_t *ap=esp_netif_create_default_wifi_ap();
    if (settings.mode==MODE_ROUTER) {
        router_monitor_init(sta);
        router_monitor_log("ROUTER BOOT");
        router_monitor_log("WIFI C%d IP C%d",WIFI_TASK_CORE_ID,CONFIG_LWIP_TCPIP_TASK_AFFINITY);
        router_monitor_log("RESET CODE %u",(unsigned)esp_reset_reason());
        router_monitor_log(settings.sharing_enabled?"SHARING ON":"SHARING OFF");
        if (!settings.ssid[0]) router_monitor_log("WIFI NOT SET");
    }
    wifi_share_init(ap,sta);
    // 初始租约使用公共 DNS；路由连通后会改成上游 DHCP 提供的 DNS。
    esp_netif_dns_info_t dns={.ip.type=ESP_IPADDR_TYPE_V4};
    dns.ip.u_addr.ip4.addr=ESP_IP4TOADDR(223,5,5,5);
    ESP_ERROR_CHECK(esp_netif_dhcps_stop(ap));
    ESP_ERROR_CHECK(esp_netif_set_dns_info(ap,ESP_NETIF_DNS_MAIN,&dns));
    uint8_t offer=0x02; // DHCP OFFER_DNS，与 wifi_share.c 的 DNS 下发选项一致。
    ESP_ERROR_CHECK(esp_netif_dhcps_option(ap,ESP_NETIF_OP_SET,ESP_NETIF_DOMAIN_NAME_SERVER,&offer,sizeof(offer)));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(ap));
    wifi_init_config_t init=WIFI_INIT_CONFIG_DEFAULT();
    // 撤回版本 11 的缓冲/聚合覆盖，恢复驱动默认值作为回退对照。
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_LOGI(TAG,"Network profile: firmware %d, CPU config %d MHz, RX buffers %d, BA window %d; reset %d",
        YE_FIRMWARE_VERSION,CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,init.static_rx_buf_num,init.rx_ba_win,(int)esp_reset_reason());
    ESP_LOGI(TAG,"Task cores: Wi-Fi %d, TCP/IP %d; low-priority router UI/management on CPU0",
        init.wifi_task_core_id,CONFIG_LWIP_TCPIP_TASK_AFFINITY);
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,ESP_EVENT_ANY_ID,wifi_event,NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,ESP_EVENT_ANY_ID,wifi_event,NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    // b/g/n 原本就是驱动默认值，显式保留 11n 和旧设备兼容性。
    const uint8_t protocols=WIFI_PROTOCOL_11B|WIFI_PROTOCOL_11G|WIFI_PROTOCOL_11N;
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA,protocols));
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_AP,protocols));
#if CONFIG_YE_WIFI_HT40
    const wifi_bandwidth_t bandwidth=WIFI_BW40;
#else
    const wifi_bandwidth_t bandwidth=WIFI_BW20;
#endif
    // HT40 只是允许值；实际信道及带宽由上游/客户端协商，AP 跟随 STA。
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_STA,bandwidth));
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_AP,bandwidth));
    // 固定 WPA2 热点；不使用 WPA3 兼容模式，减少旧手机连接失败。
    wifi_config_t access={.ap={.channel=1,.max_connection=2,.authmode=WIFI_AUTH_WPA2_PSK,
        .pairwise_cipher=WIFI_CIPHER_TYPE_CCMP}};
    strlcpy((char *)access.ap.ssid,ap_name,sizeof(access.ap.ssid));
    strlcpy((char *)access.ap.password,ap_password,sizeof(access.ap.password));
    access.ap.ssid_len=strlen(ap_name);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP,&access));
    wifi_config_t station={0};
    // ssid 字段允许恰好 32 个字节，不依赖 wifi_config_t 中的结尾零。
    memcpy(station.sta.ssid,settings.ssid,strlen(settings.ssid));
    memcpy(station.sta.password,settings.password,strlen(settings.password));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA,&station));
    ESP_ERROR_CHECK(esp_wifi_start());
    // 关闭 STA 省电，避免高频请求等待无线省电唤醒窗口。
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    // 等待 AP 接口就绪后启动本地配网页；DNS 直接通过 DHCP 下发。
    configASSERT(xEventGroupWaitBits(events,AP_READY,pdFALSE,pdTRUE,pdMS_TO_TICKS(10000))&AP_READY);
    httpd_config_t http=HTTPD_DEFAULT_CONFIG();
    http.uri_match_fn=httpd_uri_match_wildcard; http.stack_size=6144;
    ESP_ERROR_CHECK(httpd_start(&portal,&http));
    httpd_uri_t post={.uri="/api/config",.method=HTTP_POST,.handler=config_post};
    httpd_uri_t get={.uri="/*",.method=HTTP_GET,.handler=portal_get};
    ESP_ERROR_CHECK(httpd_register_uri_handler(portal,&post));
    ESP_ERROR_CHECK(httpd_register_uri_handler(portal,&get));
}

// HTTP 回调有严格容量上限，不会将损坏响应写到帧缓冲区之外。
typedef struct { uint8_t *data; size_t capacity, length; bool overflow; } response_t;
static esp_err_t http_event(esp_http_client_event_t *event)
{
    response_t *response=event->user_data;
    if (!response) return ESP_OK;
    if (event->event_id==HTTP_EVENT_ON_DATA && event->data_len>0) {
        if ((size_t)event->data_len>response->capacity-response->length) {
            response->overflow=true; return ESP_FAIL;
        }
        memcpy(response->data+response->length,event->data,event->data_len);
        response->length+=event->data_len;
    }
    return ESP_OK;
}
static bool request(const char *path, const char *post, uint8_t *output, size_t capacity, size_t *length)
{
    char url[288], authorization[144];
    snprintf(url,sizeof(url),"%s%s",settings.server,path);
    snprintf(authorization,sizeof(authorization),"Bearer %s",settings.token);
    response_t response={.data=output,.capacity=capacity};
    esp_http_client_config_t config={.url=url,.timeout_ms=12000,.crt_bundle_attach=esp_crt_bundle_attach,
        .event_handler=http_event,.user_data=&response,.disable_auto_redirect=true,.keep_alive_enable=true};
    // 仅联网任务调用；资源与状态复用同一个 HTTPS 连接，省去反复 TLS 握手。
    static esp_http_client_handle_t client;
    if (!client) client=esp_http_client_init(&config);
    if (!client) return false;
    esp_http_client_set_url(client,url);
    esp_http_client_set_user_data(client,&response);
    esp_http_client_set_method(client,post?HTTP_METHOD_POST:HTTP_METHOD_GET);
    esp_http_client_set_post_field(client,post?post:"",post?strlen(post):0);
    esp_http_client_set_header(client,"Authorization",authorization);
    if (post) {
        esp_http_client_set_method(client,HTTP_METHOD_POST);
        esp_http_client_set_header(client,"Content-Type","application/json");
        esp_http_client_set_post_field(client,post,strlen(post));
    }
    esp_err_t result=esp_http_client_perform(client);
    int status=esp_http_client_get_status_code(client);
    esp_http_client_set_user_data(client,NULL);
    if (result!=ESP_OK) { esp_http_client_cleanup(client); client=NULL; }
    *length=response.length;
    return result==ESP_OK && status==200 && !response.overflow;
}
// 速率按真实成功响应统计，供后台显示；目标 30Hz 不代表网络一定能达到。
static unsigned command_rate_milli;
static void poll_task(void *arg)
{
    const int target_hz=30;
    esp_timer_handle_t timer;
    esp_timer_create_args_t timer_config={.callback=frame_tick,.arg=xTaskGetCurrentTaskHandle(),
        .dispatch_method=ESP_TIMER_TASK,.name="command_poll",.skip_unhandled_events=true};
    ESP_ERROR_CHECK(esp_timer_create(&timer_config,&timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer,1000000/target_hz));
    esp_http_client_handle_t client=NULL;
    int64_t seen=-1, measured_at=esp_timer_get_time();
    unsigned completed=0;
    static uint8_t buffer[COMMAND_CAPACITY];
    for (;;) {
        // 只保留最新节拍；网络慢时不积压请求，不创建并行 TLS 连接。
        ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
        if (settings.mode!=MODE_CLOUD || !(xEventGroupGetBits(events)&WIFI_READY) ||
            time(NULL)<1735689600 || !settings.server[0] || !settings.token[0]) {
            if (client) { esp_http_client_cleanup(client); client=NULL; }
            seen=-1; completed=0; measured_at=esp_timer_get_time();
            xSemaphoreTake(screen_lock,portMAX_DELAY); command_rate_milli=0; xSemaphoreGive(screen_lock);
            vTaskDelay(pdMS_TO_TICKS(500)); continue;
        }
        char url[320],authorization[144];
        snprintf(url,sizeof(url),"%s/device/command?since=%lld&compact=1",settings.server,(long long)seen);
        response_t response={.data=buffer,.capacity=sizeof(buffer)-1};
        if (!client) {
            snprintf(authorization,sizeof(authorization),"Bearer %s",settings.token);
            esp_http_client_config_t config={.url=url,.timeout_ms=2000,.crt_bundle_attach=esp_crt_bundle_attach,
                .event_handler=http_event,.disable_auto_redirect=true,.keep_alive_enable=true};
            client=esp_http_client_init(&config);
            if (client) esp_http_client_set_header(client,"Authorization",authorization);
        }
        bool ok=false;
        if (client) {
            esp_http_client_set_url(client,url);
            esp_http_client_set_user_data(client,&response);
            esp_err_t result=esp_http_client_perform(client);
            int status=esp_http_client_get_status_code(client);
            esp_http_client_set_user_data(client,NULL);
            ok=result==ESP_OK && !response.overflow && (status==200 || status==204);
            if (ok && status==200) {
                buffer[response.length]=0;
                cJSON *json=cJSON_Parse((char *)buffer);
                cJSON *rev=json?cJSON_GetObjectItemCaseSensitive(json,"revision"):NULL;
                ok=cJSON_IsNumber(rev) && rev->valuedouble>=0 && rev->valuedouble<=9007199254740991.0;
                if (ok) {
                    seen=(int64_t)rev->valuedouble;
                    // 只把新版本交给资源任务，未变化的空响应不触发下载或重播。
                    xQueueOverwrite(commands,buffer);
                }
                cJSON_Delete(json);
            }
        }
        int64_t now=esp_timer_get_time();
        if (ok) completed++;
        if (now-measured_at>=1000000 || !ok) {
            unsigned rate=ok?(unsigned)((uint64_t)completed*1000000000ULL/(now-measured_at)):0;
            xSemaphoreTake(screen_lock,portMAX_DELAY); command_rate_milli=rate; xSemaphoreGive(screen_lock);
            completed=0; measured_at=now;
        }
        if (!ok) {
            if (client) { esp_http_client_cleanup(client); client=NULL; }
            seen=-1; xEventGroupClearBits(events,SERVER_READY);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        // 清掉传输期间已错过的节拍，下一次请求等待下一个 33ms 时隙。
        ulTaskNotifyTake(pdTRUE,0);
    }
}
static int choice(cJSON *json,const char *key,const char *const *options,int count)
{
    cJSON *value=cJSON_GetObjectItemCaseSensitive(json,key);
    for (int i=0;cJSON_IsString(value) && i<count;i++) if (!strcmp(value->valuestring,options[i])) return i;
    return 0;
}
static int number(cJSON *json,const char *key,int fallback,int low,int high)
{
    cJSON *value=cJSON_GetObjectItemCaseSensitive(json,key);
    return cJSON_IsNumber(value) && value->valuedouble>=low && value->valuedouble<=high?(int)value->valuedouble:fallback;
}
static int64_t version(cJSON *json,const char *key)
{
    cJSON *value=cJSON_GetObjectItemCaseSensitive(json,key);
    return cJSON_IsNumber(value) && value->valuedouble>=0 && value->valuedouble<=9007199254740991.0?(int64_t)value->valuedouble:0;
}
static bool apply_command(cJSON *json)
{
    cJSON *rev=cJSON_GetObjectItemCaseSensitive(json,"revision");
    cJSON *mode=cJSON_GetObjectItemCaseSensitive(json,"mode");
    if (!cJSON_IsNumber(rev) || !cJSON_IsString(mode) || rev->valuedouble<0 || rev->valuedouble>9007199254740991.0) return false;
    int64_t next=(int64_t)rev->valuedouble;
    xSemaphoreTake(screen_lock,portMAX_DELAY);
    int64_t current_revision=revision, cached_caption=caption_revision;
    xSemaphoreGive(screen_lock);
    if (next==current_revision) return true;
    bool next_image=!strcmp(mode->valuestring,"image");
    if (!next_image && strcmp(mode->valuestring,"pet")) return false;
    const char *const renderers[]={"original","interactive","custom"};
    int next_renderer=choice(json,"renderer",renderers,3);
    int64_t cap_version=version(json,"caption_revision"), anim_version=version(json,"animation_revision");
    int64_t expires=version(json,"expires_at");
    int64_t remaining=expires?expires-(int64_t)time(NULL)*1000:0;
    if (remaining>20000) remaining=20000;
    if (expires && remaining<=0) { next_image=false; next_renderer=0; cap_version=0; }
    uint8_t *download=NULL,*new_caption=NULL,*new_animation=NULL;
    custom_animation_t candidate={0};
    size_t length=0; char path[96]; bool ok=false;
    // 新资源开始下载前释放已经不用的旧动画，避免内存压力阻塞控制。
    xSemaphoreTake(screen_lock,portMAX_DELAY);
    if (custom_animation.data && (next_image || next_renderer!=2 || anim_version!=animation_revision)) {
        free(custom_animation.data); memset(&custom_animation,0,sizeof(custom_animation));
        animation_revision=0; renderer=0; screen_dirty=true;
    }
    xSemaphoreGive(screen_lock);
    if (next_image) {
        download=malloc(SCREEN_BYTES);
        if (!download) goto done;
        snprintf(path,sizeof(path),"/device/image?revision=%lld",(long long)next);
        if (!request(path,NULL,download,SCREEN_BYTES,&length) || length!=SCREEN_BYTES) goto done;
    }
    if (!next_image && cap_version && cap_version!=cached_caption) {
        new_caption=malloc(CAPTION_BYTES);
        if (!new_caption) goto done;
        snprintf(path,sizeof(path),"/device/caption?revision=%lld",(long long)cap_version);
        if (!request(path,NULL,new_caption,CAPTION_BYTES,&length) || length!=CAPTION_BYTES) goto done;
    }
    if (!next_image && next_renderer==2 && (!custom_animation.data || anim_version!=animation_revision)) {
        int size=number(json,"animation_size",0,1,CUSTOM_ANIMATION_MAX);
        if (!size || !anim_version) goto done;
        new_animation=malloc(size);
        if (!new_animation) goto done;
        snprintf(path,sizeof(path),"/device/animation?revision=%lld",(long long)anim_version);
        if (!request(path,NULL,new_animation,size,&length) || length!=(size_t)size ||
            !custom_animation_load(&candidate,new_animation,length)) goto done;
    }
    const char *const expressions[]={"normal","happy","sleepy","surprised"};
    const char *const looks[]={"auto","center","left","right","up","down"};
    const char *const themes[]={"cyan","pink","amber","green","purple"};
    xSemaphoreTake(screen_lock,portMAX_DELAY);
    if (download) memcpy(frame,download,SCREEN_BYTES);
    if (new_caption) memcpy(caption_pixels,new_caption,CAPTION_BYTES);
    if (new_animation) { custom_animation=candidate; new_animation=NULL; animation_revision=anim_version; }
    caption_revision=cap_version; renderer=next_renderer;
    speed=number(json,"speed",100,25,300); paused=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json,"paused"));
    eye_open=number(json,"eye_open",100,10,160); sway=number(json,"sway",1,0,5); paw_offset=number(json,"paw",0,-8,10);
    cJSON *color=cJSON_GetObjectItemCaseSensitive(json,"eye_color");
    unsigned red=58,green=98,blue=163;
    if (cJSON_IsString(color) && strlen(color->valuestring)==7) sscanf(color->valuestring,"#%2x%2x%2x",&red,&green,&blue);
    eye_color=((red&248)<<8)|((green&252)<<3)|(blue>>3);
    expression=choice(json,"expression",expressions,4); look=choice(json,"look",looks,6); theme=choice(json,"theme",themes,5);
    enabled=!cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(json,"enabled"));
    // 配网窗口期间保持可见，用户才能看到热点名称和配网地址。
    if (!(xEventGroupGetBits(events)&WIFI_READY)) enabled=true;
    manual_blink=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json,"blink"));
    image_mode=next_image; revision=next;
    // 从成功显示时起至多二十秒；下载慢时也不会让已过期内容长时间留在屏幕。
    remaining=expires?expires-(int64_t)time(NULL)*1000:0;
    if (remaining>20000) remaining=20000;
    scene_deadline=expires?esp_timer_get_time()+(remaining>0?remaining:1)*1000:0;
    screen_dirty=true;
    xSemaphoreGive(screen_lock); ok=true;
done:
    free(download); free(new_caption); free(new_animation);
    strlcpy(command_error,ok?"":"Resource download, format or memory error",sizeof(command_error));
    return ok;
}
// 路由模式独立维护上游重连，避免为不会运行的 TLS/下载逻辑预留 8 KiB 栈。
static void router_network_task(void *arg)
{
    int64_t last_connect=esp_timer_get_time(),last_stats=0;
    for (;;) {
        int64_t now=esp_timer_get_time();
        if (wifi_share_ready() && now-last_stats>=5000000) {
            wifi_share_poll_stats();
            last_stats=now;
        }
        if (!(xEventGroupGetBits(events)&WIFI_READY) && settings.ssid[0] && now-last_connect>=10000000) {
            router_monitor_log("WIFI RETRY");
            esp_err_t result=esp_wifi_connect();
            if (result!=ESP_OK) router_monitor_log("CONNECT ERR %d",(int)result);
            last_connect=now;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void network_task(void *arg)
{
    bool sntp_started=false;
    static uint8_t retry_command[COMMAND_CAPACITY];
    bool retry_pending=false;
    // STA_START 已触发第一次连接；十秒后仍未成功才开始重试。
    int64_t last_connect=esp_timer_get_time(),last_status=-15000000;
    for (;;) {
        int64_t now=esp_timer_get_time();
        EventBits_t bits=xEventGroupGetBits(events);
        if (!(bits&WIFI_READY)) {
            if (settings.ssid[0] && now-last_connect>=10000000) {
                router_monitor_log("WIFI RETRY");
                esp_wifi_connect(); last_connect=now;
            }
            vTaskDelay(pdMS_TO_TICKS(1000)); continue;
        }
        // 路由模式仅负责上游重连，不启动校时、TLS 或云端资源处理。
        if (settings.mode!=MODE_CLOUD) {
            vTaskDelay(pdMS_TO_TICKS(1000)); continue;
        }
        if (!sntp_started) {
            esp_sntp_config_t sntp=ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            sntp_started=esp_netif_sntp_init(&sntp)==ESP_OK;
        }
        // 等待校时，避免 ESP32 上电的 1970 时间导致 HTTPS 证书校验失败。
        if (time(NULL)<1735689600 || !settings.server[0] || !settings.token[0]) {
            vTaskDelay(pdMS_TO_TICKS(1000)); continue;
        }
        uint8_t buffer[2048]; size_t length=0;
        xSemaphoreTake(screen_lock,portMAX_DELAY);
        int64_t before_revision=revision;
        xSemaphoreGive(screen_lock);
        bool received=xQueueReceive(commands,buffer,pdMS_TO_TICKS(1000))==pdTRUE;
        if (received) { memcpy(retry_command,buffer,sizeof(retry_command)); retry_pending=true; }
        else if (retry_pending) memcpy(buffer,retry_command,sizeof(retry_command));
        bool attempted=retry_pending;
        bool ok=(xEventGroupGetBits(events)&SERVER_READY)!=0;
        if (attempted) {
            cJSON *json=cJSON_Parse((char *)buffer);
            ok=json && apply_command(json); cJSON_Delete(json);
            retry_pending=!ok;
        }
        if (ok) {
            xEventGroupSetBits(events,SERVER_READY);
            // 热点与本地配置页面常开，共享网络不依赖后台是否在线。
        } else xEventGroupClearBits(events,SERVER_READY);
        now=esp_timer_get_time();
        if ((now-last_status>=5000000 || revision!=before_revision || (attempted && !ok)) && !uxQueueMessagesWaiting(commands)) {
            wifi_ap_record_t ap={0}; esp_wifi_sta_get_ap_info(&ap);
            char body[512];
            xSemaphoreTake(screen_lock,portMAX_DELAY);
            snprintf(body,sizeof(body),"{\"firmware\":%d,\"transport\":\"poll\",\"request_target_hz\":30,\"request_rate_milli\":%u,\"mode\":\"%s\",\"renderer\":%d,\"remaining_ms\":%lld,\"rssi\":%d,\"revision\":%lld,\"heap\":%lu,\"uptime\":%lld,\"error\":\"%s\"}",
                YE_FIRMWARE_VERSION,command_rate_milli,image_mode?"image":"pet",renderer,(long long)(scene_deadline>now?(scene_deadline-now)/1000:0),
                ap.rssi,(long long)revision,(unsigned long)esp_get_free_heap_size(),(long long)(now/1000000),command_error);
            xSemaphoreGive(screen_lock);
            request("/device/status",body,buffer,sizeof(buffer),&length); last_status=now;
        }
        // 指令轮询在独立任务执行，资源下载和状态上报不会暂停 30Hz 请求调度。
    }
}
void app_main(void)
{
    // 不自动擦除 NVS：配置分区异常时保留凭据以便人工处理。
    ESP_ERROR_CHECK(nvs_flash_init());
    events=xEventGroupCreate(); screen_lock=xSemaphoreCreateMutex();
    load_settings();
    if (settings.mode==MODE_CLOUD) commands=xQueueCreate(1,COMMAND_CAPACITY);
    // 帧缓冲直接供 SPI DMA 使用，免去逐块复制和隐藏的临时 DMA 分配。
    frame=heap_caps_calloc(1,SCREEN_BYTES,MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL);
    configASSERT(events && screen_lock && frame && (settings.mode!=MODE_CLOUD || commands));
    network_init();
    if (settings.mode==MODE_ROUTER) {
        // CPU1 留给高优先级 TCP/IP/NAPT，低优先级屏幕和管理任务放 CPU0。
        // Wi-Fi 驱动优先级为 23，始终优先于下面两个优先级为 1 的任务。
        configASSERT(xTaskCreatePinnedToCore(display_task,"router_display",4096,NULL,1,NULL,
            0)==pdPASS);
        configASSERT(xTaskCreatePinnedToCore(router_network_task,"router_network",4096,NULL,1,NULL,
            0)==pdPASS);
    } else {
        configASSERT(xTaskCreate(display_task,"pet_display",4096,NULL,3,NULL)==pdPASS);
        configASSERT(xTaskCreate(network_task,"pet_network",8192,NULL,2,NULL)==pdPASS);
        configASSERT(xTaskCreate(poll_task,"pet_poll",6144,NULL,2,NULL)==pdPASS);
    }
    ESP_LOGI(TAG,"Pet ready; configure using credentials on screen");
}
