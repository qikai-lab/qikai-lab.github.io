/*
 * gw_latest.c - 进程内最新值快照（实现）
 * ---------------------------------------------------------------------------
 * 就一个全局结构体。单线程程序，不需要原子操作。
 *
 * 唯一要注意的是 **别重复计数**：
 *   frames 只在 gw_latest_on_frame() 里 +1，
 *   以前 main.c 里那句 st->frames++ 必须删掉，否则统计翻倍。
 */
#include <string.h>
#include <time.h>

#include "gw_latest.h"

static gw_latest_t g_latest;

void gw_latest_init(void)
{
    memset(&g_latest, 0, sizeof(g_latest));
    g_latest.started_ts = (int64_t)time(NULL);
    g_latest.valid      = 0;
}

gw_latest_t *gw_latest(void)
{
    return &g_latest;
}

void gw_latest_on_frame(const gw_env_t *e, int64_t ts)
{
    gw_latest_t *L = &g_latest;

    if (e == NULL) {
        return;
    }

    L->valid    = 1;
    L->ts       = ts;
    L->node     = e->node;
    L->pir      = (uint8_t)(e->pir ? 1 : 0);
    L->relay    = (uint8_t)(e->relay ? 1 : 0);
    L->lux      = e->lux;
    L->gas      = e->gas;
    L->lux_pct  = (uint8_t)gw_env_lux_pct(e);
    L->temp_x10 = gw_env_temp_x10(e);
    L->hum_x10  = gw_env_humi_x10(e);

    L->frames++;
}
