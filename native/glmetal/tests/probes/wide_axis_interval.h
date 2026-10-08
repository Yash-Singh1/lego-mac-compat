#ifndef GLM_PROBE_WIDE_AXIS_INTERVAL_H
#define GLM_PROBE_WIDE_AXIS_INTERVAL_H
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <math.h>

struct glm_probe_axis_interval {
    int32_t first_pixel, last_pixel; /* canonical increasing order */
    uint32_t count;
    double draw_start, draw_end;    /* original traversal direction */
};

/* Exploratory constant-color/depth-off axis reconstruction. No clipping is
 * performed: moving an endpoint across the viewport would invalidate the
 * diamond-exit proof. False means retain the original implementation. */
static bool glm_probe_wide_axis_interval(double start, double end,
        int32_t viewport_origin, uint32_t viewport_size,
        struct glm_probe_axis_interval *out)
{
    if (!out) return false;
    *out = (struct glm_probe_axis_interval){0};
    if (!isfinite(start) || !isfinite(end) || viewport_size == 0 || viewport_size > INT32_MAX) return false;
    double edge0 = viewport_origin, edge1 = edge0 + viewport_size;
    if (!(start > edge0 && start < edge1 && end > edge0 && end < edge1)) return false;
    start = floor(start * 256 + 0.5) / 256;
    end = floor(end * 256 + 0.5) / 256;
    double low = fmin(start, end), high = fmax(start, end);
    double first = ceil(low - 0.5), last = ceil(high - 0.5) - 1;
    if (first > last) return true; /* Empty half-open sample-center interval. */
    if (first < INT32_MIN || first > INT32_MAX || last < INT32_MIN || last > INT32_MAX) return false;
    double count = last - first + 1;
    if (!(count > 0 && count <= UINT32_MAX)) return false;
    double draw_start = start < end ? first + 0.5 : last + 0.5;
    double draw_end = start < end ? last + 1.25 : first - 0.25;
    if (!(draw_start > edge0 && draw_start < edge1 && draw_end > edge0 && draw_end < edge1)) return false;
    out->first_pixel = (int32_t)first; out->last_pixel = (int32_t)last;
    out->count = (uint32_t)count; out->draw_start = draw_start; out->draw_end = draw_end;
    return true;
}
#endif
