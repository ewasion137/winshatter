#ifndef PHYSICS_H
#define PHYSICS_H

#include <math.h>
#include <stddef.h>

#define PHYS_MAX_CUBES 1024
#define PHYS_SUBSTEPS 2

struct phys_rect {
    float left;
    float top;
    float right;
    float bottom;
};

struct phys_cube {
    int active;
    int group_id;
    float x, y;
    float vx, vy;
    float hw, hh;
    float radius;
    float mass;
    float inertia;
    float ang;
    float ang_v;
    float u0, v0, u1, v1;
};

struct phys_world {
    struct phys_cube cubes[PHYS_MAX_CUBES];
    struct phys_rect bounds;
    float gravity;
    float cur_x, cur_y;
    float cur_vx, cur_vy;
    int cur_init;
};

static inline void phys_init(struct phys_world *w, struct phys_rect bounds) {
    for (int i = 0; i < PHYS_MAX_CUBES; i++)
        w->cubes[i].active = 0;
    w->bounds = bounds;
    w->gravity = 1650.0f;
    w->cur_init = 0;
}

static inline void phys_clear_group(struct phys_world *w, int group_id) {
    for (int i = 0; i < PHYS_MAX_CUBES; i++) {
        if (w->cubes[i].active && w->cubes[i].group_id == group_id)
            w->cubes[i].active = 0;
    }
}

static inline int phys_spawn_cube(struct phys_world *w, int group_id, float x,
                                  float y, float hw, float hh, float vx,
                                  float vy, float ang_v, float u0, float v0,
                                  float u1, float v1) {
    for (int i = 0; i < PHYS_MAX_CUBES; i++) {
        if (!w->cubes[i].active) {
            struct phys_cube *c = &w->cubes[i];
            c->active = 1;
            c->group_id = group_id;
            c->x = x;
            c->y = y;
            c->vx = vx;
            c->vy = vy;
            c->hw = hw;
            c->hh = hh;
            c->radius = (hw + hh) * 0.54f;
            c->mass = hw * hh * 0.01f;
            if (c->mass < 1.0f)
                c->mass = 1.0f;
            c->inertia = c->mass * (hw * hw + hh * hh) / 3.0f;
            c->ang = 0.0f;
            c->ang_v = ang_v;
            c->u0 = u0;
            c->v0 = v0;
            c->u1 = u1;
            c->v1 = v1;
            return 1;
        }
    }
    return 0;
}

static inline void phys_solve_wall(struct phys_cube *c, float rx, float ry,
                                   float nx, float ny, float pen, float e,
                                   float mu) {
    float inv_m = 1.0f / c->mass;
    float inv_i = 1.0f / c->inertia;

    c->x += nx * pen;
    c->y += ny * pen;

    float vpx = c->vx - c->ang_v * ry;
    float vpy = c->vy + c->ang_v * rx;
    float vn = vpx * nx + vpy * ny;
    if (vn >= 0.0f)
        return;

    float rest = (-vn < 55.0f) ? 0.0f : e;
    float rn = rx * ny - ry * nx;
    float denom = inv_m + rn * rn * inv_i;
    float jn = -(1.0f + rest) * vn / denom;

    c->vx += nx * jn * inv_m;
    c->vy += ny * jn * inv_m;
    c->ang_v += rn * jn * inv_i;

    vpx = c->vx - c->ang_v * ry;
    vpy = c->vy + c->ang_v * rx;
    float tx = -ny;
    float ty = nx;
    float vt = vpx * tx + vpy * ty;
    float rt = rx * ty - ry * tx;
    float denom_t = inv_m + rt * rt * inv_i;
    float jt = -vt / denom_t;
    float max_jt = mu * jn;
    if (jt > max_jt)
        jt = max_jt;
    if (jt < -max_jt)
        jt = -max_jt;

    c->vx += tx * jt * inv_m;
    c->vy += ty * jt * inv_m;
    c->ang_v += rt * jt * inv_i;
}

static inline void phys_solve_pair(struct phys_cube *a, struct phys_cube *b) {
    float dx = b->x - a->x;
    float dy = b->y - a->y;
    float min_d = a->radius + b->radius;
    float d2 = dx * dx + dy * dy;
    if (d2 >= min_d * min_d)
        return;

    float dist = sqrtf(d2);
    float nx, ny;
    if (dist < 0.001f) {
        nx = 0.0f;
        ny = -1.0f;
        dist = 0.001f;
    } else {
        nx = dx / dist;
        ny = dy / dist;
    }

    float inv_ma = 1.0f / a->mass;
    float inv_mb = 1.0f / b->mass;
    float inv_sum = inv_ma + inv_mb;
    float pen = min_d - dist;

    a->x -= nx * pen * (inv_ma / inv_sum);
    a->y -= ny * pen * (inv_ma / inv_sum);
    b->x += nx * pen * (inv_mb / inv_sum);
    b->y += ny * pen * (inv_mb / inv_sum);

    float rax = nx * a->radius, ray = ny * a->radius;
    float rbx = -nx * b->radius, rby = -ny * b->radius;
    float vax = a->vx - a->ang_v * ray, vay = a->vy + a->ang_v * rax;
    float vbx = b->vx - b->ang_v * rby, vby = b->vy + b->ang_v * rbx;
    float rvx = vbx - vax;
    float rvy = vby - vay;
    float vn = rvx * nx + rvy * ny;
    if (vn >= 0.0f)
        return;

    float rest = (-vn < 40.0f) ? 0.0f : 0.52f;
    float jn = -(1.0f + rest) * vn / inv_sum;
    a->vx -= nx * jn * inv_ma;
    a->vy -= ny * jn * inv_ma;
    b->vx += nx * jn * inv_mb;
    b->vy += ny * jn * inv_mb;

    float tx = -ny, ty = nx;
    float vt = rvx * tx + rvy * ty;
    float rta = a->radius;
    float rtb = -b->radius;
    float denom_t = inv_sum + rta * rta / a->inertia + rtb * rtb / b->inertia;
    float jt = -vt / denom_t;
    float max_jt = 0.35f * jn;
    if (jt > max_jt)
        jt = max_jt;
    if (jt < -max_jt)
        jt = -max_jt;

    a->vx -= tx * jt * inv_ma;
    a->vy -= ty * jt * inv_ma;
    b->vx += tx * jt * inv_mb;
    b->vy += ty * jt * inv_mb;
    a->ang_v -= (rta * jt) / a->inertia;
    b->ang_v += (rtb * jt) / b->inertia;
}

static inline void phys_solve_cursor(struct phys_world *w,
                                     struct phys_cube *c) {
    float cur_r = 28.0f;
    float dx = c->x - w->cur_x;
    float dy = c->y - w->cur_y;
    float min_d = cur_r + c->radius;
    float d2 = dx * dx + dy * dy;
    if (d2 >= min_d * min_d)
        return;

    float dist = sqrtf(d2);
    float nx = dist > 0.001f ? dx / dist : 0.0f;
    float ny = dist > 0.001f ? dy / dist : -1.0f;
    float pen = min_d - dist;
    c->x += nx * pen;
    c->y += ny * pen;

    float rvx = c->vx - w->cur_vx;
    float rvy = c->vy - w->cur_vy;
    float vn = rvx * nx + rvy * ny;
    if (vn < 0.0f) {
        float j = -1.35f * vn;
        c->vx += nx * j;
        c->vy += ny * j;
        float tx = -ny, ty = nx;
        float vt = rvx * tx + rvy * ty;
        c->ang_v += (vt * 0.015f) / (1.0f + c->hw * 0.02f);
    }
}

static inline void phys_step(struct phys_world *w, float dt, float cur_x,
                             float cur_y) {
    if (dt <= 0.0001f)
        return;

    if (!w->cur_init) {
        w->cur_x = cur_x;
        w->cur_y = cur_y;
        w->cur_vx = 0.0f;
        w->cur_vy = 0.0f;
        w->cur_init = 1;
    } else {
        float vx = (cur_x - w->cur_x) / dt;
        float vy = (cur_y - w->cur_y) / dt;
        if (vx > 2400.0f)
            vx = 2400.0f;
        if (vx < -2400.0f)
            vx = -2400.0f;
        if (vy > 2400.0f)
            vy = 2400.0f;
        if (vy < -2400.0f)
            vy = -2400.0f;
        w->cur_vx = vx;
        w->cur_vy = vy;
        w->cur_x = cur_x;
        w->cur_y = cur_y;
    }

    float sub_dt = dt / (float)PHYS_SUBSTEPS;
    float damp = expf(-0.45f * sub_dt);

    for (int step = 0; step < PHYS_SUBSTEPS; step++) {
        for (int i = 0; i < PHYS_MAX_CUBES; i++) {
            struct phys_cube *c = &w->cubes[i];
            if (!c->active)
                continue;

            c->vy += w->gravity * sub_dt;
            c->vx *= damp;
            c->ang_v *= damp;
            c->x += c->vx * sub_dt;
            c->y += c->vy * sub_dt;
            c->ang += c->ang_v * sub_dt;

            phys_solve_cursor(w, c);

            float cs = cosf(c->ang);
            float sn = sinf(c->ang);
            float signs[4][2] = {
                {-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f}};
            for (int k = 0; k < 4; k++) {
                float lx = signs[k][0] * c->hw;
                float ly = signs[k][1] * c->hh;
                float rx = lx * cs - ly * sn;
                float ry = lx * sn + ly * cs;
                float px = c->x + rx;
                float py = c->y + ry;

                if (px < w->bounds.left)
                    phys_solve_wall(c, rx, ry, 1.0f, 0.0f, w->bounds.left - px,
                                    0.65f, 0.25f);
                else if (px > w->bounds.right)
                    phys_solve_wall(c, rx, ry, -1.0f, 0.0f,
                                    px - w->bounds.right, 0.65f, 0.25f);

                if (py < w->bounds.top)
                    phys_solve_wall(c, rx, ry, 0.0f, 1.0f, w->bounds.top - py,
                                    0.65f, 0.25f);
                else if (py > w->bounds.bottom)
                    phys_solve_wall(c, rx, ry, 0.0f, -1.0f,
                                    py - w->bounds.bottom, 0.45f, 0.40f);
            }
        }

        for (int i = 0; i < PHYS_MAX_CUBES; i++) {
            if (!w->cubes[i].active)
                continue;
            for (int j = i + 1; j < PHYS_MAX_CUBES; j++) {
                if (!w->cubes[j].active)
                    continue;
                phys_solve_pair(&w->cubes[i], &w->cubes[j]);
            }
        }
    }
}

#endif
