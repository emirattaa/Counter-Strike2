#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <Psapi.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>
#include <cstdint>
#include <cstdarg>
#include <cstring>
#include <string>

#include "imgui/imgui.h"
#include "imgui/imgui_impl_dx11.h"
#include "imgui/imgui_impl_win32.h"
#include "minhook/include/MinHook.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "Psapi.lib")

// ============================================================
// tipler
// ============================================================
using PresentFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT);
using ResizeBuffersFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using ClipCursorFn = BOOL(__stdcall*)(const RECT*);

static PresentFn       g_origPresent = nullptr;
static ResizeBuffersFn g_origResizeBuffers = nullptr;
static ClipCursorFn    g_origClipCursor = nullptr;

static HWND                     g_window = nullptr;
static WNDPROC                  g_origWndProc = nullptr;
static ID3D11Device* g_device = nullptr;
static ID3D11DeviceContext* g_context = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static bool                     g_imguiReady = false;
static bool                     g_menuVisible = true;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// ============================================================
// log
// ============================================================
static FILE* g_log = nullptr;

static void OpenLog() {
    char path[MAX_PATH]{};
    DWORD n = GetTempPathA(MAX_PATH, path);
    if (n == 0 || n >= MAX_PATH) return;
    strcat_s(path, "cs2int.log");
    fopen_s(&g_log, path, "w");
}

static void Log(const char* fmt, ...) {
    if (!g_log) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

// ============================================================
// wndproc
// ============================================================
static LRESULT __stdcall hkWndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN && wp == VK_INSERT) {
        g_menuVisible = !g_menuVisible;
        Log("[!] menu visible=%d", (int)g_menuVisible);
        if (g_imguiReady) ImGui::GetIO().ClearInputKeys();
        if (g_menuVisible) ReleaseCapture();
        return CallWindowProcA(g_origWndProc, hWnd, msg, wp, lp);
    }

    switch (msg) {
    case WM_ACTIVATE: case WM_ACTIVATEAPP: case WM_NCACTIVATE:
    case WM_SETFOCUS: case WM_KILLFOCUS:
    case WM_SYSCOMMAND: case WM_SIZE: case WM_MOVE:
    case WM_WINDOWPOSCHANGED: case WM_PAINT: case WM_ERASEBKGND:
    case WM_SETCURSOR:
        return CallWindowProcA(g_origWndProc, hWnd, msg, wp, lp);
    }

    if (g_menuVisible && g_imguiReady) {
        ImGuiIO& io = ImGui::GetIO();
        switch (msg) {
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN: case WM_LBUTTONUP:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP:
        case WM_MOUSEWHEEL:  case WM_MOUSEHWHEEL:
            ImGui_ImplWin32_WndProcHandler(hWnd, msg, wp, lp);
            if (io.WantCaptureMouse) return 1;
            break;
        case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR:
        case WM_SYSKEYDOWN: case WM_SYSKEYUP:
            ImGui_ImplWin32_WndProcHandler(hWnd, msg, wp, lp);
            if (io.WantCaptureKeyboard) return 1;
            break;
        }
    }
    return CallWindowProcA(g_origWndProc, hWnd, msg, wp, lp);
}

// ============================================================
// rtv
// ============================================================
static void CreateRenderTarget(IDXGISwapChain* sc) {
    ID3D11Texture2D* back = nullptr;
    HRESULT hr = sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back);
    if (FAILED(hr) || !back) {
        Log("[-] GetBuffer(0) fail hr=0x%08X", (unsigned)hr);
        return;
    }
    hr = g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
    back->Release();
    Log("[+] RTV=%p hr=0x%08X", g_rtv, (unsigned)hr);
}

static void ReleaseRenderTarget() {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}

// ============================================================
// imgui
// ============================================================
static void InitImGui(IDXGISwapChain* sc) {
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&g_device)) || !g_device) {
        Log("[-] GetDevice fail");
        return;
    }
    g_device->GetImmediateContext(&g_context);

    DXGI_SWAP_CHAIN_DESC desc{};
    sc->GetDesc(&desc);
    g_window = desc.OutputWindow;
    Log("[+] swapchain hwnd=%p", g_window);

    g_origWndProc = (WNDPROC)SetWindowLongPtrA(g_window, GWLP_WNDPROC, (LONG_PTR)hkWndProc);
    Log("[+] wndproc=%p", g_origWndProc);

    CreateRenderTarget(sc);

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(g_window);
    ImGui_ImplDX11_Init(g_device, g_context);

    g_imguiReady = true;
    Log("[+] imgui hazir");
}

// ============================================================
// hooks
// ============================================================

static int g_frameCount = 0;

static HRESULT __stdcall hkPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (!g_imguiReady) InitImGui(sc);

    if (g_imguiReady) {
        if (!g_rtv) CreateRenderTarget(sc);

        g_frameCount++;
        if (g_frameCount <= 3 || (g_frameCount % 600 == 0))
            Log("[+] frame=%d rtv=%p menu=%d", g_frameCount, g_rtv, (int)g_menuVisible);

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        ImGuiIO& io = ImGui::GetIO();
        io.MouseDrawCursor = g_menuVisible;

        if (g_menuVisible) {
            ImGui::Begin("CS2 Internal", &g_menuVisible);
            ImGui::Text("client.dll  : %p", (void*)GetModuleHandleA("client.dll"));
            ImGui::Text("engine2.dll : %p", (void*)GetModuleHandleA("engine2.dll"));
            ImGui::Text("FPS         : %.1f", io.Framerate);
            ImGui::Separator();
            static bool dummy = false;
            ImGui::Checkbox("dummy", &dummy);
            ImGui::End();
        }

        ImGui::Render();

        // CS2 state kaydet
        ID3D11RenderTargetView* oldRTV = nullptr;
        ID3D11DepthStencilView* oldDSV = nullptr;
        g_context->OMGetRenderTargets(1, &oldRTV, &oldDSV);

        // kendi RTV'ne çiz
        if (g_rtv) g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        // CS2 state geri yükle
        g_context->OMSetRenderTargets(1, &oldRTV, oldDSV);
        if (oldRTV) oldRTV->Release();
        if (oldDSV) oldDSV->Release();
    }

    return g_origPresent(sc, sync, flags);
}


static HRESULT __stdcall hkResizeBuffers(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT fmt, UINT fl) {
    ReleaseRenderTarget();
    if (g_imguiReady) ImGui_ImplDX11_InvalidateDeviceObjects();
    HRESULT hr = g_origResizeBuffers(sc, n, w, h, fmt, fl);
    if (g_imguiReady) {
        ImGui_ImplDX11_CreateDeviceObjects();
        CreateRenderTarget(sc);
    }
    return hr;
}

static BOOL __stdcall hkClipCursor(const RECT* r) {
    if (g_menuVisible) return g_origClipCursor(nullptr);
    return g_origClipCursor(r);
}

// ============================================================
// dummy pencere — GetDesktopWindow yerine
// ============================================================
static HWND CreateDummyWindow() {
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "cs2int_dummy";

    if (!RegisterClassExA(&wc)) {
        DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            Log("[-] RegisterClassExA err=%lu", err);
            return nullptr;
        }
    }

    HWND hwnd = CreateWindowExA(
        0, "cs2int_dummy", "cs2int",
        WS_OVERLAPPED,
        0, 0, 8, 8,
        nullptr, nullptr, wc.hInstance, nullptr);

    if (!hwnd) Log("[-] CreateWindowExA err=%lu", GetLastError());
    else      Log("[+] dummy hwnd=%p", hwnd);
    return hwnd;
}

static void DestroyDummyWindow(HWND hwnd) {
    if (hwnd) DestroyWindow(hwnd);
    UnregisterClassA("cs2int_dummy", GetModuleHandleA(nullptr));
}

// ============================================================
// hook setup
// ============================================================
static bool SetupHooks() {
    HWND dummyHwnd = CreateDummyWindow();
    if (!dummyHwnd) return false;

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 8;
    sd.BufferDesc.Height = 8;
    sd.BufferDesc.RefreshRate.Numerator = 0;
    sd.BufferDesc.RefreshRate.Denominator = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
    sd.BufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = dummyHwnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    sd.Flags = 0;

    ID3D11Device* dummyDev = nullptr;
    ID3D11DeviceContext* dummyCtx = nullptr;
    IDXGISwapChain* dummySc = nullptr;
    D3D_FEATURE_LEVEL    fl{};

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &sd,
        &dummySc, &dummyDev, &fl, &dummyCtx);

    Log("[+] D3D11CreateDeviceAndSwapChain hr=0x%08X", (unsigned)hr);

    if (FAILED(hr)) {
        // fallback: device + factory
        Log("[!] combined fail, fallback D3D11CreateDevice + IDXGIFactory");
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION,
            &dummyDev, &fl, &dummyCtx);

        if (SUCCEEDED(hr) && dummyDev) {
            IDXGIDevice* dxgiDev = nullptr;
            IDXGIAdapter* adapter = nullptr;
            IDXGIFactory* factory = nullptr;

            if (SUCCEEDED(dummyDev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev)) && dxgiDev) {
                dxgiDev->GetAdapter(&adapter);
                if (adapter) adapter->GetParent(__uuidof(IDXGIFactory), (void**)&factory);
                if (factory) hr = factory->CreateSwapChain(dummyDev, &sd, &dummySc);
            }
            if (factory)  factory->Release();
            if (adapter)  adapter->Release();
            if (dxgiDev)  dxgiDev->Release();
        }
    }

    if (FAILED(hr) || !dummySc) {
        Log("[-] dummy swapchain fail hr=0x%08X", (unsigned)hr);
        if (dummyCtx) dummyCtx->Release();
        if (dummyDev) dummyDev->Release();
        DestroyDummyWindow(dummyHwnd);
        return false;
    }

    void** vt = *(void***)dummySc;
    void* presentPtr = vt[8];
    void* resizePtr = vt[13];

    Log("[+] vtable Present       = %p", presentPtr);
    Log("[+] vtable ResizeBuffers = %p", resizePtr);

    if (MH_CreateHook(presentPtr, &hkPresent, (LPVOID*)&g_origPresent) != MH_OK) { Log("[-] Present hook fail");       dummySc->Release(); dummyCtx->Release(); dummyDev->Release(); DestroyDummyWindow(dummyHwnd); return false; }
    if (MH_CreateHook(resizePtr, &hkResizeBuffers, (LPVOID*)&g_origResizeBuffers) != MH_OK) { Log("[-] ResizeBuffers hook fail"); dummySc->Release(); dummyCtx->Release(); dummyDev->Release(); DestroyDummyWindow(dummyHwnd); return false; }

    HMODULE u32 = GetModuleHandleA("user32.dll");
    if (u32) {
        void* clip = (void*)GetProcAddress(u32, "ClipCursor");
        if (clip && MH_CreateHook(clip, &hkClipCursor, (LPVOID*)&g_origClipCursor) == MH_OK)
            Log("[+] ClipCursor hook kuruldu");
        else
            Log("[!] ClipCursor hook opsiyonel, kurulamadi");
    }

    // dummy objeleri temizle
    dummySc->Release();
    dummyCtx->Release();
    dummyDev->Release();
    DestroyDummyWindow(dummyHwnd);

    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) { Log("[-] MH_EnableHook fail"); return false; }
    Log("[+] hooks aktif");
    return true;
}

// ============================================================
// shutdown
// ============================================================
static void Shutdown() {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();

    if (g_imguiReady) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        g_imguiReady = false;
    }

    if (g_origWndProc && g_window)
        SetWindowLongPtrA(g_window, GWLP_WNDPROC, (LONG_PTR)g_origWndProc);

    ReleaseRenderTarget();
    if (g_context) { g_context->Release(); g_context = nullptr; }
    if (g_device) { g_device->Release();  g_device = nullptr; }
}

// ============================================================
// thread
// ============================================================
static DWORD WINAPI MainThread(LPVOID lpParam) {
    HMODULE hMod = (HMODULE)lpParam;
    OpenLog();
    Log("[+] CS2-Internal menu-only yuklendi");

    if (MH_Initialize() != MH_OK) { Log("[-] MH_Initialize fail"); goto cleanup; }
    Log("[+] MinHook init");

    if (!SetupHooks()) { Log("[-] SetupHooks fail"); goto cleanup; }
    Log("[+] INSERT = menu, DELETE = unload");

    while (!(GetAsyncKeyState(VK_DELETE) & 0x8000)) Sleep(100);

cleanup:
    Log("[!] kapatiliyor");
    Shutdown();
    if (g_log) { fclose(g_log); g_log = nullptr; }
    FreeLibraryAndExitThread(hMod, 0);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hMod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hMod);
        CreateThread(nullptr, 0, MainThread, hMod, 0, nullptr);
    }
    return TRUE;
}