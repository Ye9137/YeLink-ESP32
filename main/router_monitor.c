// Copyright (c) 2026 Ye；计数不发事件、不复制数据、不改变原收发返回值。
#include "router_monitor.h"
#include <stdbool.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "esp_netif_net_stack.h"
#include "esp_clk_tree.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"

static esp_netif_t *upstream_netif;
static portMUX_TYPE monitor_lock=portMUX_INITIALIZER_UNLOCKED;
// 包计数使用 32 位无锁原子操作，避免 Wi-Fi C0、lwIP C1 与日志复制共用临界区。
_Static_assert(ATOMIC_INT_LOCK_FREE==2,"Router byte counters require lock-free 32-bit atomics");
static _Atomic(uint32_t) uploaded, downloaded;
static uint32_t previous_upload, previous_download;
static int64_t previous_sample;
static uint32_t previous_idle[2];
static bool cpu_sample_started;
static unsigned log_next, log_count;
static char log_lines[ROUTER_LOG_ROWS][ROUTER_LOG_COLS+1];

void router_monitor_init(esp_netif_t *upstream)
{
    upstream_netif=upstream;
    previous_sample=esp_timer_get_time();
}

void router_monitor_log(const char *format, ...)
{
    if (!upstream_netif) return;
    char line[ROUTER_LOG_COLS+1]={0};
    unsigned seconds=(unsigned)(esp_timer_get_time()/1000000);
    // 固定宽度 MM:SS，分钟数每 100 分钟循环，避免挤占消息空间。
    int used=snprintf(line,sizeof(line),"%02u:%02u ",(seconds/60)%100,seconds%60);
    va_list args;
    va_start(args,format);
    vsnprintf(line+used,sizeof(line)-(size_t)used,format,args);
    va_end(args);
    portENTER_CRITICAL(&monitor_lock);
    memcpy(log_lines[log_next],line,sizeof(line));
    log_next=(log_next+1)%ROUTER_LOG_ROWS;
    if (log_count<ROUTER_LOG_ROWS) log_count++;
    portEXIT_CRITICAL(&monitor_lock);
}

void router_monitor_snapshot(router_snapshot_t *snapshot)
{
    memset(snapshot,0,sizeof(*snapshot));
    uint32_t up=atomic_load_explicit(&uploaded,memory_order_relaxed);
    uint32_t down=atomic_load_explicit(&downloaded,memory_order_relaxed);
    portENTER_CRITICAL(&monitor_lock);
    snapshot->log_count=log_count;
    unsigned oldest=(log_next+ROUTER_LOG_ROWS-log_count)%ROUTER_LOG_ROWS;
    for (unsigned i=0;i<log_count;i++)
        memcpy(snapshot->logs[i],log_lines[(oldest+i)%ROUTER_LOG_ROWS],sizeof(log_lines[0]));
    portEXIT_CRITICAL(&monitor_lock);
    int64_t now=esp_timer_get_time(), elapsed=now-previous_sample;
    uint32_t cpu_hz=0;
    if (esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_CPU,ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED,&cpu_hz)==ESP_OK)
        snapshot->cpu_mhz=cpu_hz/1000000;
    snapshot->heap_free=esp_get_free_heap_size();
    snapshot->heap_min=esp_get_minimum_free_heap_size();
    // 读取每个核的空闲任务累计微秒数；只作差分，不枚举或扫描所有任务。
    unsigned total_busy=0;
    for (int core=0;core<configNUMBER_OF_CORES && core<2;core++) {
#if CONFIG_FREERTOS_SMP
        TaskStatus_t idle_status;
        vTaskGetInfo(xTaskGetIdleTaskHandleForCore(core),&idle_status,pdFALSE,eInvalid);
        uint32_t idle=(uint32_t)idle_status.ulRunTimeCounter;
#else
        uint32_t idle=(uint32_t)ulTaskGetIdleRunTimeCounterForCore(core);
#endif
        uint32_t idle_delta=idle-previous_idle[core];
        if (cpu_sample_started && elapsed>0) {
            uint64_t idle_percent=(uint64_t)idle_delta*100/(uint64_t)elapsed;
            unsigned busy=idle_percent>=100?0:100-(unsigned)idle_percent;
            snapshot->cpu_percent[core]=(uint8_t)busy;
            total_busy+=busy;
        }
        previous_idle[core]=idle;
    }
    snapshot->cpu_sample_valid=cpu_sample_started && elapsed>0;
    snapshot->cpu_total_percent=(uint8_t)(total_busy/configNUMBER_OF_CORES);
    cpu_sample_started=true;
    if (elapsed>0) {
        // 无符号减法允许累计计数器回绕；64 位中间值避免乘法溢出。
        snapshot->upload_bytes_per_sec=(uint64_t)(uint32_t)(up-previous_upload)*1000000/elapsed;
        snapshot->download_bytes_per_sec=(uint64_t)(uint32_t)(down-previous_download)*1000000/elapsed;
    }
    previous_upload=up; previous_download=down; previous_sample=now;
}

static void count_bytes(esp_netif_t *netif, size_t length, bool transmit, esp_err_t result)
{
    if (netif!=upstream_netif || !upstream_netif || result!=ESP_OK) return;
    if (transmit) atomic_fetch_add_explicit(&uploaded,(uint32_t)length,memory_order_relaxed);
    else atomic_fetch_add_explicit(&downloaded,(uint32_t)length,memory_order_relaxed);
}

// 通过链接器 wrap 观察现有 ESP-NETIF API，保留原始数据及错误处理。
// 只统计 STA 一侧，避免 AP/STA 转发重复计数；包括链路协议开销。
extern esp_err_t __real_esp_netif_transmit(esp_netif_t *,void *,size_t);
extern esp_err_t __real_esp_netif_transmit_wrap(esp_netif_t *,void *,size_t,void *);
extern esp_err_t __real_esp_netif_receive(esp_netif_t *,void *,size_t,void *);

esp_err_t __wrap_esp_netif_transmit(esp_netif_t *netif,void *data,size_t length)
{
    esp_err_t result=__real_esp_netif_transmit(netif,data,length);
    count_bytes(netif,length,true,result);
    return result;
}
esp_err_t __wrap_esp_netif_transmit_wrap(esp_netif_t *netif,void *data,size_t length,void *buffer)
{
    esp_err_t result=__real_esp_netif_transmit_wrap(netif,data,length,buffer);
    count_bytes(netif,length,true,result);
    return result;
}
esp_err_t __wrap_esp_netif_receive(esp_netif_t *netif,void *data,size_t length,void *buffer)
{
    esp_err_t result=__real_esp_netif_receive(netif,data,length,buffer);
    count_bytes(netif,length,false,result);
    return result;
}
