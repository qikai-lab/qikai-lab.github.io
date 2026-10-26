/*
 * gw_filter.c - 中值滤波 + 自适应基线（实现）
 */
#include <string.h>

#include "gw_filter.h"

/*------------------------------------------------------------------------------
 * 中值滤波
 *----------------------------------------------------------------------------*/

void gw_medfilt_init(gw_medfilt_t *f)
{
    if (f == NULL) {
        return;
    }
    memset(f, 0, sizeof(*f));
}

uint16_t gw_medfilt_push(gw_medfilt_t *f, uint16_t v)
{
    uint16_t tmp[GW_MED_WINDOW];
    int      i;
    int      j;
    int      n;

    if (f == NULL) {
        return v;
    }

    f->buf[f->idx] = v;
    f->idx = (uint8_t)((f->idx + 1) % GW_MED_WINDOW);
    if (f->n < GW_MED_WINDOW) {
        f->n++;
    }

    /* 拷贝 n 个有效元素做插入排序。
     * n 最多 5，最多比较 10 次，1 秒才跑一次，性能完全不是问题；
     * 相比"维护有序表"的写法，这个几乎不可能写错。 */
    n = (int)f->n;
    for (i = 0; i < n; i++) {
        tmp[i] = f->buf[i];
    }
    for (i = 1; i < n; i++) {
        uint16_t key = tmp[i];

        j = i - 1;
        while (j >= 0 && tmp[j] > key) {
            tmp[j + 1] = tmp[j];
            j--;
        }
        tmp[j + 1] = key;
    }

    f->med = tmp[n / 2];        /* 奇数 n 时正好是正中间 */
    return f->med;
}

uint16_t gw_medfilt_value(const gw_medfilt_t *f)
{
    return (f != NULL) ? f->med : 0;
}

/*------------------------------------------------------------------------------
 * 自适应基线
 *----------------------------------------------------------------------------*/

void gw_baseline_init(gw_baseline_t *b, uint16_t tau)
{
    if (b == NULL) {
        return;
    }
    memset(b, 0, sizeof(*b));
    b->tau   = (tau > 0) ? tau : 600;
    b->valid = 0;
}

void gw_baseline_update(gw_baseline_t *b, uint16_t gas, int freeze)
{
    int32_t target;

    if (b == NULL) {
        return;
    }

    /* 第一帧直接取该值当基线：不然要等好久才收敛 */
    if (!b->valid) {
        b->base_x100 = (int32_t)gas * 100;
        b->acc       = 0;
        b->valid     = 1;
        return;
    }

    if (freeze) {
        return;     /* 告警中冻结，见头文件里的说明 */
    }

    /* 余数累积版 EMA：
     *   acc += (目标 - 当前)
     *   每 tau 帧才真正挪一格，余数留着下次用
     * 这样即使单帧差值远小于 tau，长期平均也是对的（不会卡死）。 */
    target = (int32_t)gas * 100;
    b->acc += target - b->base_x100;

    {
        int32_t step = b->acc / (int32_t)b->tau;

        if (step != 0) {
            b->base_x100 += step;
            b->acc       -= step * (int32_t)b->tau;
        }
    }
}

uint16_t gw_baseline_value(const gw_baseline_t *b)
{
    if (b == NULL || !b->valid) {
        return 0;
    }
    /* 四舍五入到整数，给上层当 0~1023 用 */
    return (uint16_t)((b->base_x100 + 50) / 100);
}

int gw_baseline_valid(const gw_baseline_t *b)
{
    return (b != NULL && b->valid) ? 1 : 0;
}
