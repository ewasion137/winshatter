#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dwmapi.h>

STDAPI DCompositionCreateDevice(
    IDXGIDevice *dxgiDevice, REFIID iid, void **dcompositionDevice);

typedef void (STDMETHODCALLTYPE *com_fn)(void);
#define VCALL(obj, idx, fn_t) ((fn_t)((*(com_fn **)(obj))[idx]))

typedef ULONG   (STDMETHODCALLTYPE *com_release_fn)(void *);
typedef HRESULT (STDMETHODCALLTYPE *dcomp_commit_fn)(void *);
typedef HRESULT (STDMETHODCALLTYPE *dcomp_target_fn)(void *, HWND, BOOL, void **);
typedef HRESULT (STDMETHODCALLTYPE *dcomp_visual_fn)(void *, void **);
typedef HRESULT (STDMETHODCALLTYPE *dcomp_set_root_fn)(void *, void *);
typedef HRESULT (STDMETHODCALLTYPE *dcomp_set_content_fn)(void *, IUnknown *);

static const GUID IID_DCompDevice = {
    0xC37EA93A, 0xE7AA, 0x450D,
    {0xB1, 0x6F, 0x97, 0x46, 0xCB, 0x04, 0x07, 0xF3}
};

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

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"winshatter_overlay";
    if (!RegisterClassExW(&wc))
        return 1;

    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    HWND overlay = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT |
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
        wc.lpszClassName, L"", WS_POPUP | WS_VISIBLE,
        vx, vy, vw, vh, NULL, NULL, inst, NULL
    );
    if (!overlay)
        return 1;

    ID3D11Device *d3d = NULL;
    ID3D11DeviceContext *ctx = NULL;
    if (FAILED(D3D11CreateDevice(
            NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0,
            D3D11_SDK_VERSION, &d3d, NULL, &ctx)))
        return 1;

    IDXGIDevice *dxgi_dev = NULL;
    IDXGIAdapter *adapter = NULL;
    IDXGIFactory2 *factory = NULL;
    ID3D11Device_QueryInterface(d3d, &IID_IDXGIDevice, (void **)&dxgi_dev);
    IDXGIDevice_GetAdapter(dxgi_dev, &adapter);
    IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory2, (void **)&factory);

    DXGI_SWAP_CHAIN_DESC1 sc_desc = {0};
    sc_desc.Width = (UINT)vw;
    sc_desc.Height = (UINT)vh;
    sc_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sc_desc.SampleDesc.Count = 1;
    sc_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc_desc.BufferCount = 2;
    sc_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sc_desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;

    IDXGISwapChain1 *swapchain = NULL;
    IDXGIFactory2_CreateSwapChainForComposition(
        factory, (IUnknown *)dxgi_dev, &sc_desc, NULL, &swapchain);

    void *dcomp = NULL;
    void *target = NULL;
    void *visual = NULL;
    DCompositionCreateDevice(dxgi_dev, &IID_DCompDevice, &dcomp);
    VCALL(dcomp, 6, dcomp_target_fn)(dcomp, overlay, TRUE, &target);
    VCALL(dcomp, 7, dcomp_visual_fn)(dcomp, &visual);
    VCALL(visual, 15, dcomp_set_content_fn)(visual, (IUnknown *)swapchain);
    VCALL(target, 3, dcomp_set_root_fn)(target, visual);
    VCALL(dcomp, 3, dcomp_commit_fn)(dcomp);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    VCALL(visual, 2, com_release_fn)(visual);
    VCALL(target, 2, com_release_fn)(target);
    VCALL(dcomp, 2, com_release_fn)(dcomp);
    IDXGISwapChain1_Release(swapchain);
    IDXGIFactory2_Release(factory);
    IDXGIAdapter_Release(adapter);
    IDXGIDevice_Release(dxgi_dev);
    ID3D11DeviceContext_Release(ctx);
    ID3D11Device_Release(d3d);
    return 0;
}
