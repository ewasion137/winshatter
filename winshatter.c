#include <d3d11.h>
#include <d3dcompiler.h>
#include <dwmapi.h>
#include <dxgi1_2.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>


#ifndef EVENT_OBJECT_CLOAKED
#define EVENT_OBJECT_CLOAKED 0x8017
#endif

STDAPI DCompositionCreateDevice(IDXGIDevice *dxgiDevice, REFIID iid,
                                void **dcompositionDevice);

typedef void(STDMETHODCALLTYPE *com_fn)(void);
#define VCALL(obj, idx, fn_t) ((fn_t)((*(com_fn **)(obj))[idx]))

typedef ULONG(STDMETHODCALLTYPE *com_release_fn)(void *);
typedef HRESULT(STDMETHODCALLTYPE *dcomp_commit_fn)(void *);
typedef HRESULT(STDMETHODCALLTYPE *dcomp_target_fn)(void *, HWND, BOOL,
                                                    void **);
typedef HRESULT(STDMETHODCALLTYPE *dcomp_visual_fn)(void *, void **);
typedef HRESULT(STDMETHODCALLTYPE *dcomp_set_root_fn)(void *, void *);
typedef HRESULT(STDMETHODCALLTYPE *dcomp_set_content_fn)(void *, IUnknown *);

static const GUID IID_DCompDevice = {
    0xC37EA93A,
    0xE7AA,
    0x450D,
    {0xB1, 0x6F, 0x97, 0x46, 0xCB, 0x04, 0x07, 0xF3}};

#define RING_CAP 16
#define MAX_TRACKED 256
#define SHARD_COLS 8
#define SHARD_ROWS 6
#define TRIS_PER_WIN (SHARD_COLS * SHARD_ROWS * 2)
#define MAX_GROUPS 12

struct ring_slot {
    ID3D11Texture2D *tex;
    uint64_t time_ms;
};

struct tracked_win {
    HWND hwnd;
    RECT rc;
};

struct shard_tri {
    float lx[3], ly[3];
    float u[3], v[3];
    float x, y;
    float vx, vy;
    float ang, ang_v;
    float alpha;
};

struct shard_group {
    int active;
    float life;
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
    struct shard_tri tris[TRIS_PER_WIN];
};

struct shard_vert {
    float x, y;
    float u, v;
    float alpha;
};

static const char HLSL_SRC[] = "Texture2D tex0 : register(t0);\n"
                               "SamplerState smp0 : register(s0);\n"
                               "struct VSIn { float2 pos : POSITION; float2 uv "
                               ": TEXCOORD0; float a : COLOR0; };\n"
                               "struct PSIn { float4 pos : SV_POSITION; float2 "
                               "uv : TEXCOORD0; float a : COLOR0; };\n"
                               "PSIn vs_main(VSIn i) {\n"
                               "    PSIn o; o.pos = float4(i.pos, 0.0, 1.0); "
                               "o.uv = i.uv; o.a = i.a; return o;\n"
                               "}\n"
                               "float4 ps_main(PSIn i) : SV_TARGET {\n"
                               "    float4 c = tex0.Sample(smp0, i.uv);\n"
                               "    return float4(c.rgb * i.a, i.a);\n"
                               "}\n";

static ID3D11Device *g_d3d;
static ID3D11DeviceContext *g_ctx;
static IDXGISwapChain1 *g_swapchain;
static ID3D11RenderTargetView *g_rtv;
static IDXGIOutput1 *g_output1;
static IDXGIOutputDuplication *g_dupl;
static RECT g_out_rc;
static int g_vx, g_vy, g_vw, g_vh;
static float g_work_bottom;

static ID3D11VertexShader *g_vs;
static ID3D11PixelShader *g_ps;
static ID3D11InputLayout *g_layout;
static ID3D11Buffer *g_vb;
static ID3D11BlendState *g_blend;
static ID3D11RasterizerState *g_rast;
static ID3D11SamplerState *g_samp;

static struct ring_slot g_ring[RING_CAP];
static int g_ring_head;
static uint64_t g_last_cap_ms;
static uint64_t g_qpc_freq;

static struct tracked_win g_wins[MAX_TRACKED];
static int g_win_count;
static struct shard_group g_groups[MAX_GROUPS];
static int g_need_clear;

static uint64_t now_ms(void) {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (uint64_t)((c.QuadPart * 1000ULL) / g_qpc_freq);
}

static float frand(float lo, float hi) {
    return lo + ((float)rand() / (float)RAND_MAX) * (hi - lo);
}

static int find_win(HWND hwnd) {
    for (int i = 0; i < g_win_count; i++) {
        if (g_wins[i].hwnd == hwnd)
            return i;
    }
    return -1;
}

static int is_ignored_win(HWND hwnd) {
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (style & WS_CHILD)
        return 1;
    if (ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE))
        return 1;
    if (!(style & (WS_CAPTION | WS_SYSMENU | WS_THICKFRAME)))
        return 1;

    WCHAR cls[64];
    if (GetClassNameW(hwnd, cls, 64) > 0) {
        static const WCHAR *const bad_cls[] = {L"Windows.UI.Core.CoreWindow",
                                               L"Xaml_WindowedPopupClass",
                                               L"Shell_TrayWnd",
                                               L"Shell_SecondaryTrayWnd",
                                               L"Progman",
                                               L"WorkerW",
                                               L"NotifyIconOverflowWindow",
                                               L"TaskListThumbnailWnd",
                                               L"MultitaskingViewFrame",
                                               L"ForegroundStaging"};
        for (size_t i = 0; i < sizeof(bad_cls) / sizeof(bad_cls[0]); i++) {
            if (lstrcmpW(cls, bad_cls[i]) == 0)
                return 1;
        }
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid) {
        HANDLE proc =
            OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (proc) {
            WCHAR path[MAX_PATH];
            DWORD sz = MAX_PATH;
            int skip = 0;
            if (QueryFullProcessImageNameW(proc, 0, path, &sz)) {
                const WCHAR *name = path;
                for (const WCHAR *p = path; *p; p++) {
                    if (*p == L'\\' || *p == L'/')
                        name = p + 1;
                }
                static const WCHAR *const bad_exe[] = {
                    L"SearchHost.exe",
                    L"SearchApp.exe",
                    L"StartMenuExperienceHost.exe",
                    L"ShellExperienceHost.exe",
                    L"ShellHost.exe",
                    L"TextInputHost.exe",
                    L"ScreenClippingHost.exe",
                    L"LockApp.exe"};
                for (size_t i = 0; i < sizeof(bad_exe) / sizeof(bad_exe[0]);
                     i++) {
                    if (lstrcmpiW(name, bad_exe[i]) == 0) {
                        skip = 1;
                        break;
                    }
                }
            }
            CloseHandle(proc);
            if (skip)
                return 1;
        }
    }
    return 0;
}

static void upsert_win(HWND hwnd) {
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd) || is_ignored_win(hwnd))
        return;
    if (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CHILD)
        return;
    if (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)
        return;

    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked,
                                        sizeof(cloaked))) &&
        cloaked)
        return;

    RECT rc;
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rc,
                                     sizeof(rc))))
        return;
    if (rc.right - rc.left < 40 || rc.bottom - rc.top < 40)
        return;

    BOOL disable = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &disable,
                          sizeof(disable));

    int idx = find_win(hwnd);
    if (idx >= 0) {
        g_wins[idx].rc = rc;
        return;
    }
    if (g_win_count < MAX_TRACKED) {
        g_wins[g_win_count].hwnd = hwnd;
        g_wins[g_win_count].rc = rc;
        g_win_count++;
    }
}

static int pick_ring_frame(uint64_t target_ms) {
    int best = -1;
    uint64_t best_t = 0;
    int oldest = -1;
    uint64_t oldest_t = UINT64_MAX;

    for (int i = 0; i < RING_CAP; i++) {
        uint64_t t = g_ring[i].time_ms;
        if (t == 0)
            continue;
        if (t < oldest_t) {
            oldest_t = t;
            oldest = i;
        }
        if (t <= target_ms && t >= best_t) {
            best_t = t;
            best = i;
        }
    }
    return best >= 0 ? best : oldest;
}

static void spawn_shatter(RECT rc) {
    RECT cl;
    cl.left = rc.left > g_out_rc.left ? rc.left : g_out_rc.left;
    cl.top = rc.top > g_out_rc.top ? rc.top : g_out_rc.top;
    cl.right = rc.right < g_out_rc.right ? rc.right : g_out_rc.right;
    cl.bottom = rc.bottom < g_out_rc.bottom ? rc.bottom : g_out_rc.bottom;

    int w = cl.right - cl.left;
    int h = cl.bottom - cl.top;
    if (w < 40 || h < 40)
        return;

    int ring_idx = pick_ring_frame(now_ms() - 45);
    if (ring_idx < 0)
        return;

    int slot = -1;
    for (int i = 0; i < MAX_GROUPS; i++) {
        if (!g_groups[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return;

    D3D11_TEXTURE2D_DESC td = {0};
    td.Width = (UINT)w;
    td.Height = (UINT)h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    ID3D11Texture2D *wtex = NULL;
    if (FAILED(ID3D11Device_CreateTexture2D(g_d3d, &td, NULL, &wtex)))
        return;

    D3D11_BOX box;
    box.left = (UINT)(cl.left - g_out_rc.left);
    box.top = (UINT)(cl.top - g_out_rc.top);
    box.right = box.left + (UINT)w;
    box.bottom = box.top + (UINT)h;
    box.front = 0;
    box.back = 1;
    ID3D11DeviceContext_CopySubresourceRegion(
        g_ctx, (ID3D11Resource *)wtex, 0, 0, 0, 0,
        (ID3D11Resource *)g_ring[ring_idx].tex, 0, &box);

    ID3D11ShaderResourceView *wsrv = NULL;
    if (FAILED(ID3D11Device_CreateShaderResourceView(
            g_d3d, (ID3D11Resource *)wtex, NULL, &wsrv))) {
        ID3D11Texture2D_Release(wtex);
        return;
    }

    POINT cur;
    GetCursorPos(&cur);
    float ix = (float)(cl.left + w / 2);
    float iy = (float)(cl.top + h / 2);
    if (cur.x >= cl.left - 32 && cur.x <= cl.right + 32 &&
        cur.y >= cl.top - 32 && cur.y <= cl.bottom + 32) {
        ix = (float)cur.x;
        iy = (float)cur.y;
    }

    float gx[SHARD_ROWS + 1][SHARD_COLS + 1];
    float gy[SHARD_ROWS + 1][SHARD_COLS + 1];
    float cw = (float)w / (float)SHARD_COLS;
    float ch = (float)h / (float)SHARD_ROWS;

    for (int r = 0; r <= SHARD_ROWS; r++) {
        for (int c = 0; c <= SHARD_COLS; c++) {
            float px = (float)c * cw;
            float py = (float)r * ch;
            if (c > 0 && c < SHARD_COLS)
                px += frand(-0.35f * cw, 0.35f * cw);
            if (r > 0 && r < SHARD_ROWS)
                py += frand(-0.35f * ch, 0.35f * ch);
            gx[r][c] = px;
            gy[r][c] = py;
        }
    }

    struct shard_group *grp = &g_groups[slot];
    grp->active = 1;
    grp->life = 1.6f;
    grp->tex = wtex;
    grp->srv = wsrv;

    int ti = 0;
    for (int r = 0; r < SHARD_ROWS; r++) {
        for (int c = 0; c < SHARD_COLS; c++) {
            int p_idx[2][3][2] = {{{r, c}, {r, c + 1}, {r + 1, c + 1}},
                                  {{r, c}, {r + 1, c + 1}, {r + 1, c}}};
            if ((r + c) & 1) {
                p_idx[0][2][0] = r + 1;
                p_idx[0][2][1] = c;
                p_idx[1][0][0] = r;
                p_idx[1][0][1] = c + 1;
            }
            for (int t = 0; t < 2; t++) {
                struct shard_tri *st = &grp->tris[ti++];
                float sx = 0.0f, sy = 0.0f;
                for (int k = 0; k < 3; k++) {
                    int pr = p_idx[t][k][0];
                    int pc = p_idx[t][k][1];
                    sx += (float)cl.left + gx[pr][pc];
                    sy += (float)cl.top + gy[pr][pc];
                }
                st->x = sx / 3.0f;
                st->y = sy / 3.0f;

                for (int k = 0; k < 3; k++) {
                    int pr = p_idx[t][k][0];
                    int pc = p_idx[t][k][1];
                    float wx = (float)cl.left + gx[pr][pc];
                    float wy = (float)cl.top + gy[pr][pc];
                    st->lx[k] = wx - st->x;
                    st->ly[k] = wy - st->y;
                    st->u[k] = gx[pr][pc] / (float)w;
                    st->v[k] = gy[pr][pc] / (float)h;
                }

                float dx = st->x - ix;
                float dy = st->y - iy;
                float dist = sqrtf(dx * dx + dy * dy) + 1.0f;
                float force = 520.0f / (1.0f + dist * 0.004f);
                st->vx = (dx / dist) * force * frand(0.6f, 1.35f) +
                         frand(-80.0f, 80.0f);
                st->vy = (dy / dist) * force * frand(0.6f, 1.35f) -
                         frand(60.0f, 220.0f);
                st->ang = 0.0f;
                st->ang_v = frand(-7.0f, 7.0f);
                st->alpha = 1.0f;
            }
        }
    }
}

static void CALLBACK on_win_event(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                  LONG id_object, LONG id_child, DWORD thread,
                                  DWORD time) {
    (void)hook;
    (void)thread;
    (void)time;
    if (!hwnd || id_object != OBJID_WINDOW || id_child != CHILDID_SELF)
        return;

    if (event == EVENT_OBJECT_SHOW || event == EVENT_SYSTEM_FOREGROUND ||
        event == EVENT_OBJECT_LOCATIONCHANGE) {
        upsert_win(hwnd);
        return;
    }

    if (event == EVENT_OBJECT_DESTROY || event == EVENT_OBJECT_HIDE ||
        event == EVENT_OBJECT_CLOAKED) {
        int idx = find_win(hwnd);
        if (idx < 0)
            return;
        RECT rc = g_wins[idx].rc;
        g_wins[idx] = g_wins[--g_win_count];
        if (event != EVENT_OBJECT_DESTROY && IsIconic(hwnd))
            return;
        spawn_shatter(rc);
    }
}

static BOOL CALLBACK init_enum_cb(HWND hwnd, LPARAM lp) {
    (void)lp;
    upsert_win(hwnd);
    return TRUE;
}

static void poll_capture(uint64_t t_ms) {
    if (!g_dupl) {
        if (FAILED(IDXGIOutput1_DuplicateOutput(g_output1, (IUnknown *)g_d3d,
                                                &g_dupl)))
            return;
    }

    DXGI_OUTDUPL_FRAME_INFO fi;
    IDXGIResource *res = NULL;
    HRESULT hr = IDXGIOutputDuplication_AcquireNextFrame(g_dupl, 0, &fi, &res);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT)
        return;
    if (hr == DXGI_ERROR_ACCESS_LOST) {
        IDXGIOutputDuplication_Release(g_dupl);
        g_dupl = NULL;
        return;
    }
    if (FAILED(hr))
        return;

    if (res && t_ms - g_last_cap_ms >= 25) {
        ID3D11Texture2D *frame_tex = NULL;
        if (SUCCEEDED(IDXGIResource_QueryInterface(res, &IID_ID3D11Texture2D,
                                                   (void **)&frame_tex))) {
            ID3D11DeviceContext_CopyResource(
                g_ctx, (ID3D11Resource *)g_ring[g_ring_head].tex,
                (ID3D11Resource *)frame_tex);
            g_ring[g_ring_head].time_ms = t_ms;
            g_ring_head = (g_ring_head + 1) % RING_CAP;
            g_last_cap_ms = t_ms;
            ID3D11Texture2D_Release(frame_tex);
        }
    }

    if (res)
        IDXGIResource_Release(res);
    IDXGIOutputDuplication_ReleaseFrame(g_dupl);
}

static int step_and_render(float dt) {
    int any_active = 0;
    for (int i = 0; i < MAX_GROUPS; i++) {
        if (g_groups[i].active) {
            any_active = 1;
            break;
        }
    }
    if (!any_active && !g_need_clear)
        return 0;

    float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &g_rtv, NULL);
    ID3D11DeviceContext_ClearRenderTargetView(g_ctx, g_rtv, clear);

    if (any_active) {
        D3D11_VIEWPORT vp = {0.0f, 0.0f, (float)g_vw, (float)g_vh, 0.0f, 1.0f};
        UINT stride = sizeof(struct shard_vert);
        UINT offset = 0;
        ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &vp);
        ID3D11DeviceContext_RSSetState(g_ctx, g_rast);
        ID3D11DeviceContext_IASetInputLayout(g_ctx, g_layout);
        ID3D11DeviceContext_IASetPrimitiveTopology(
            g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_IASetVertexBuffers(g_ctx, 0, 1, &g_vb, &stride,
                                               &offset);
        ID3D11DeviceContext_VSSetShader(g_ctx, g_vs, NULL, 0);
        ID3D11DeviceContext_PSSetShader(g_ctx, g_ps, NULL, 0);
        ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 1, &g_samp);
        ID3D11DeviceContext_OMSetBlendState(g_ctx, g_blend, NULL, 0xFFFFFFFF);

        for (int i = 0; i < MAX_GROUPS; i++) {
            struct shard_group *grp = &g_groups[i];
            if (!grp->active)
                continue;

            grp->life -= dt;
            if (grp->life <= 0.0f) {
                grp->active = 0;
                ID3D11ShaderResourceView_Release(grp->srv);
                ID3D11Texture2D_Release(grp->tex);
                continue;
            }

            float fade = grp->life < 0.45f ? grp->life / 0.45f : 1.0f;
            D3D11_MAPPED_SUBRESOURCE mapped;
            if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_vb, 0,
                                               D3D11_MAP_WRITE_DISCARD, 0,
                                               &mapped)))
                continue;

            struct shard_vert *verts = (struct shard_vert *)mapped.pData;
            int vc = 0;
            for (int t = 0; t < TRIS_PER_WIN; t++) {
                struct shard_tri *st = &grp->tris[t];
                st->vy += 1750.0f * dt;
                st->x += st->vx * dt;
                st->y += st->vy * dt;
                st->ang += st->ang_v * dt;
                if (st->y > g_work_bottom) {
                    st->y = g_work_bottom;
                    st->vy = -st->vy * 0.38f;
                    st->vx *= 0.82f;
                    st->ang_v *= 0.65f;
                }
                float cs = cosf(st->ang);
                float sn = sinf(st->ang);
                for (int k = 0; k < 3; k++) {
                    float wx = st->x + st->lx[k] * cs - st->ly[k] * sn;
                    float wy = st->y + st->lx[k] * sn + st->ly[k] * cs;
                    verts[vc].x =
                        ((wx - (float)g_vx) / (float)g_vw) * 2.0f - 1.0f;
                    verts[vc].y =
                        1.0f - ((wy - (float)g_vy) / (float)g_vh) * 2.0f;
                    verts[vc].u = st->u[k];
                    verts[vc].v = st->v[k];
                    verts[vc].alpha = fade;
                    vc++;
                }
            }
            ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_vb, 0);
            ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 1, &grp->srv);
            ID3D11DeviceContext_Draw(g_ctx, (UINT)vc, 0);
        }
        g_need_clear = 1;
    } else {
        g_need_clear = 0;
    }

    IDXGISwapChain1_Present(g_swapchain, 1, 0);
    return any_active;
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show) {
    (void)prev;
    (void)cmd;
    (void)show;
    srand((unsigned)GetTickCount());

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    g_qpc_freq = (uint64_t)freq.QuadPart;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    RECT work_rc;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_rc, 0);
    g_work_bottom = (float)work_rc.bottom;

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"winshatter_overlay";
    if (!RegisterClassExW(&wc))
        return 1;

    g_vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    g_vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    g_vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    g_vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    HWND overlay = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW |
            WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
        wc.lpszClassName, L"", WS_POPUP | WS_VISIBLE, g_vx, g_vy, g_vw, g_vh,
        NULL, NULL, inst, NULL);
    if (!overlay)
        return 1;

    IDXGIFactory1 *factory1 = NULL;
    IDXGIAdapter1 *adapter1 = NULL;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory1)))
        return 1;
    if (FAILED(IDXGIFactory1_EnumAdapters1(factory1, 0, &adapter1)))
        return 1;

    if (FAILED(D3D11CreateDevice((IDXGIAdapter *)adapter1,
                                 D3D_DRIVER_TYPE_UNKNOWN, NULL,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0,
                                 D3D11_SDK_VERSION, &g_d3d, NULL, &g_ctx)))
        return 1;

    IDXGIOutput *output = NULL;
    if (FAILED(IDXGIAdapter1_EnumOutputs(adapter1, 0, &output)))
        return 1;
    DXGI_OUTPUT_DESC out_desc;
    IDXGIOutput_GetDesc(output, &out_desc);
    g_out_rc = out_desc.DesktopCoordinates;
    IDXGIOutput_QueryInterface(output, &IID_IDXGIOutput1, (void **)&g_output1);
    IDXGIOutput_Release(output);

    D3D11_TEXTURE2D_DESC rd = {0};
    rd.Width = (UINT)(g_out_rc.right - g_out_rc.left);
    rd.Height = (UINT)(g_out_rc.bottom - g_out_rc.top);
    rd.MipLevels = 1;
    rd.ArraySize = 1;
    rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    rd.SampleDesc.Count = 1;
    rd.Usage = D3D11_USAGE_DEFAULT;
    rd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    for (int i = 0; i < RING_CAP; i++)
        ID3D11Device_CreateTexture2D(g_d3d, &rd, NULL, &g_ring[i].tex);

    IDXGIDevice *dxgi_dev = NULL;
    IDXGIFactory2 *factory2 = NULL;
    ID3D11Device_QueryInterface(g_d3d, &IID_IDXGIDevice, (void **)&dxgi_dev);
    IDXGIFactory1_QueryInterface(factory1, &IID_IDXGIFactory2,
                                 (void **)&factory2);

    DXGI_SWAP_CHAIN_DESC1 sc_desc = {0};
    sc_desc.Width = (UINT)g_vw;
    sc_desc.Height = (UINT)g_vh;
    sc_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sc_desc.SampleDesc.Count = 1;
    sc_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc_desc.BufferCount = 2;
    sc_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sc_desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    IDXGIFactory2_CreateSwapChainForComposition(factory2, (IUnknown *)dxgi_dev,
                                                &sc_desc, NULL, &g_swapchain);

    ID3D11Texture2D *backbuf = NULL;
    IDXGISwapChain1_GetBuffer(g_swapchain, 0, &IID_ID3D11Texture2D,
                              (void **)&backbuf);
    ID3D11Device_CreateRenderTargetView(g_d3d, (ID3D11Resource *)backbuf, NULL,
                                        &g_rtv);
    ID3D11Texture2D_Release(backbuf);

    void *dcomp = NULL, *target = NULL, *visual = NULL;
    DCompositionCreateDevice(dxgi_dev, &IID_DCompDevice, &dcomp);
    VCALL(dcomp, 6, dcomp_target_fn)(dcomp, overlay, TRUE, &target);
    VCALL(dcomp, 7, dcomp_visual_fn)(dcomp, &visual);
    VCALL(visual, 15, dcomp_set_content_fn)(visual, (IUnknown *)g_swapchain);
    VCALL(target, 3, dcomp_set_root_fn)(target, visual);
    VCALL(dcomp, 3, dcomp_commit_fn)(dcomp);

    ID3DBlob *vs_blob = NULL, *ps_blob = NULL;
    D3DCompile(HLSL_SRC, sizeof(HLSL_SRC) - 1, NULL, NULL, NULL, "vs_main",
               "vs_4_0", 0, 0, &vs_blob, NULL);
    D3DCompile(HLSL_SRC, sizeof(HLSL_SRC) - 1, NULL, NULL, NULL, "ps_main",
               "ps_4_0", 0, 0, &ps_blob, NULL);
    ID3D11Device_CreateVertexShader(g_d3d, ID3D10Blob_GetBufferPointer(vs_blob),
                                    ID3D10Blob_GetBufferSize(vs_blob), NULL,
                                    &g_vs);
    ID3D11Device_CreatePixelShader(g_d3d, ID3D10Blob_GetBufferPointer(ps_blob),
                                   ID3D10Blob_GetBufferSize(ps_blob), NULL,
                                   &g_ps);

    D3D11_INPUT_ELEMENT_DESC ied[] = {{"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,
                                       0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
                                      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,
                                       0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
                                      {"COLOR", 0, DXGI_FORMAT_R32_FLOAT, 0, 16,
                                       D3D11_INPUT_PER_VERTEX_DATA, 0}};
    ID3D11Device_CreateInputLayout(
        g_d3d, ied, 3, ID3D10Blob_GetBufferPointer(vs_blob),
        ID3D10Blob_GetBufferSize(vs_blob), &g_layout);
    ID3D10Blob_Release(vs_blob);
    ID3D10Blob_Release(ps_blob);

    D3D11_BUFFER_DESC bd = {0};
    bd.ByteWidth = sizeof(struct shard_vert) * TRIS_PER_WIN * 3;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Device_CreateBuffer(g_d3d, &bd, NULL, &g_vb);

    D3D11_BLEND_DESC bld = {0};
    bld.RenderTarget[0].BlendEnable = TRUE;
    bld.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bld.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bld.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bld.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ID3D11Device_CreateBlendState(g_d3d, &bld, &g_blend);

    D3D11_RASTERIZER_DESC rsd = {0};
    rsd.FillMode = D3D11_FILL_SOLID;
    rsd.CullMode = D3D11_CULL_NONE;
    ID3D11Device_CreateRasterizerState(g_d3d, &rsd, &g_rast);

    D3D11_SAMPLER_DESC smd = {0};
    smd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    smd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    smd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ID3D11Device_CreateSamplerState(g_d3d, &smd, &g_samp);

    EnumWindows(init_enum_cb, 0);
    DWORD hook_flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
    SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE, NULL, on_win_event,
                    0, 0, hook_flags);
    SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE,
                    NULL, on_win_event, 0, 0, hook_flags);
    SetWinEventHook(EVENT_OBJECT_CLOAKED, EVENT_OBJECT_CLOAKED, NULL,
                    on_win_event, 0, 0, hook_flags);
    SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, NULL,
                    on_win_event, 0, 0, hook_flags);

    uint64_t prev_ms = now_ms();
    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT)
                goto done;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        uint64_t cur_ms = now_ms();
        float dt = (float)(cur_ms - prev_ms) / 1000.0f;
        if (dt > 0.05f)
            dt = 0.05f;
        prev_ms = cur_ms;

        poll_capture(cur_ms);
        if (!step_and_render(dt))
            MsgWaitForMultipleObjects(0, NULL, FALSE, 20, QS_ALLINPUT);
    }

done:
    IDXGIFactory2_Release(factory2);
    IDXGIDevice_Release(dxgi_dev);
    IDXGIAdapter1_Release(adapter1);
    IDXGIFactory1_Release(factory1);
    return 0;
}
