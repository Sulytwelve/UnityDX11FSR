#include "common.h"
#include "il2cpp_mvec.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

typedef void* (*Pfn_domain_get)();
typedef void** (*Pfn_domain_get_assemblies)(void*, size_t*);
typedef void* (*Pfn_assembly_get_image)(void*);
typedef void* (*Pfn_class_from_name)(void* image, const char* ns, const char* name);
typedef void* (*Pfn_thread_attach)(void* domain);
typedef void* (*Pfn_runtime_invoke)(void* method, void* obj, void** params, void** exc);
typedef void* (*Pfn_string_new)(const char* s);
typedef void* (*Pfn_value_box)(void* klass, void* value);
typedef int32_t (*Pfn_gchandle_new)(void* obj, bool pinned);
typedef void (*Pfn_gchandle_free)(int32_t handle);

static Pfn_domain_get        il2cpp_domain_get;
static Pfn_class_from_name   il2cpp_class_from_name;
static Pfn_thread_attach     il2cpp_thread_attach;
static Pfn_runtime_invoke    il2cpp_runtime_invoke;
static Pfn_string_new        il2cpp_string_new;
static Pfn_value_box         il2cpp_value_box;
static Pfn_gchandle_new      il2cpp_gchandle_new;
static Pfn_gchandle_free     il2cpp_gchandle_free;

static void* s_shaderClass   = nullptr;
static void* s_getGlobalTex  = nullptr;
static void* s_camClass      = nullptr;
static void* s_getMain       = nullptr;
static void* s_setDepthMode  = nullptr;
static void* s_nativePtr     = nullptr;
static void* s_boxIntClass   = nullptr;
static void* s_strMvec   = nullptr;
static void* s_strDepth  = nullptr;
static int32_t s_strMvecHandle = 0, s_strDepthHandle = 0;

static ID3D11Device* s_dev11 = nullptr;

// 主线程写, Present 线程读 (Interlocked 交接)
static ID3D11ShaderResourceView* volatile g_mvecSRV = nullptr;  // SRV 持有纹理引用
static ID3D11Texture2D* volatile        g_depthTex = nullptr;   // AddRef 持有
static void* g_lastMvecNative = nullptr;
static volatile LONG s_resolved = 0;   // 元数据已解析
static volatile LONG s_rendering = 0;  // Present hook 通知: 渲染已开始
static volatile LONG s_modeSet = 0;    // depthTextureMode 已设置
static volatile LONG s_texSeen = 0;    // 纹理已到手

static HANDLE s_thread = nullptr;
static volatile bool s_stop = false;

static void* invoke(void* method, void* obj, void** params) {
    if (!method) return nullptr;
    void* exc = nullptr;
    void* r = il2cpp_runtime_invoke(method, obj, params, &exc);
    if (exc) { static bool once = false; if (!once) { LOG_W("[mvec] invoke threw"); once = true; } return nullptr; }
    return r;
}

namespace lillia {

void mvec_notify_rendering() { InterlockedExchange(&s_rendering, 1); }

void mvec_set_device(void* d) { s_dev11 = (ID3D11Device*)d; }

// ===== 主线程 (Unity 主线程, demosaic Update hook 调用) =====
void mvec_poll_mainthread() {
    if (!s_resolved) return;
    static uint32_t counter = 0;
    bool doMode = (counter % 120) == 0;    // ~2s 重设 mode (场景切换防御)
    bool doFetch = (counter % 30) == 0;    // ~0.5s 刷新纹理指针 (RT 重分配防御)
    counter++;
    if (!doMode && !doFetch) return;

        // GC 根: il2cpp 字符串若不 gchandle 固定, 场景加载触发 GC 时被回收 → GetGlobalTexture 崩
        if (!s_strMvec) {
            s_strMvec  = il2cpp_string_new("_CameraMotionVectorsTexture");
            s_strDepth = il2cpp_string_new("_CameraDepthTexture");
            if (s_strMvec)  s_strMvecHandle  = il2cpp_gchandle_new(s_strMvec, false);
            if (s_strDepth) s_strDepthHandle = il2cpp_gchandle_new(s_strDepth, false);
            if (!s_strMvec || !s_strDepth || !s_strMvecHandle || !s_strDepthHandle) return;
        }

    if (doMode) {
        void* cam = invoke(s_getMain, nullptr, nullptr);
        if (cam) {
            int32_t ch = il2cpp_gchandle_new(cam, false);   // 临时 GC 根
            int32_t v = 5;  // Depth|MotionVectors
            void* boxed = il2cpp_value_box(s_boxIntClass, &v);
            if (boxed) {
                void* params[1] = { boxed };
                if (invoke(s_setDepthMode, cam, params)) {
                    if (!s_modeSet) { s_modeSet = 1; LOG_I("[mvec] depthTextureMode set (Depth|MotionVectors)"); }
                }
            }
            if (ch) il2cpp_gchandle_free(ch);
        }
    }
    if (!doFetch || !s_dev11) return;

    // mvec: 纹理指针变化时建 SRV (SRV AddRef 保活)
    void* params[1] = { s_strMvec };
    void* tex = invoke(s_getGlobalTex, nullptr, params);
    if (tex) {
        int32_t th = il2cpp_gchandle_new(tex, false);       // 临时 GC 根
        void* p = invoke(s_nativePtr, tex, nullptr);
        if (th) il2cpp_gchandle_free(th);
        if (p) {
            void* native = *(void**)((uint8_t*)p + 0x10);   // boxed IntPtr -> m_value
            if (native && native != g_lastMvecNative) {
                D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
                sv.Format = DXGI_FORMAT_R16G16_FLOAT;
                sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                ID3D11ShaderResourceView* srv = nullptr;
                if (SUCCEEDED(s_dev11->CreateShaderResourceView((ID3D11Resource*)native, &sv, &srv))) {
                    ID3D11ShaderResourceView* old = g_mvecSRV;
                    g_mvecSRV = srv;
                    if (old) old->Release();
                    g_lastMvecNative = native;
                    if (!s_texSeen) { s_texSeen = 1; LOG_I("[mvec] motion vector texture acquired (SRV cached)"); }
                } else {
                    LOG_W("[mvec] CreateSRV(mvec) failed");
                }
            }
        }
    }
    // depth: AddRef 持有
    params[0] = s_strDepth;
    tex = invoke(s_getGlobalTex, nullptr, params);
    if (tex) {
        int32_t th = il2cpp_gchandle_new(tex, false);       // 临时 GC 根
        void* p = invoke(s_nativePtr, tex, nullptr);
        if (th) il2cpp_gchandle_free(th);
        if (p) {
            ID3D11Texture2D* native = *(ID3D11Texture2D**)((uint8_t*)p + 0x10);
            if (native && native != (ID3D11Texture2D*)g_depthTex) {
                native->AddRef();
                ID3D11Texture2D* old = g_depthTex;
                g_depthTex = native;
                if (old) old->Release();
            }
        }
    }
}

// ===== Present 线程: 纯缓存读 =====
void mvec_fetch(void** outMvecSRV, void** outDepthTex) {
    *outMvecSRV = (void*)g_mvecSRV;
    *outDepthTex = (void*)g_depthTex;
}

int mvec_status() {
    if (!s_resolved) return 0;
    if (s_texSeen) return 1;
    if (s_modeSet) return 2;
    return 3;
}

// ===== worker: 仅元数据解析 =====
static DWORD WINAPI mvec_worker(LPVOID) {
    void* GA = nullptr;
    for (int i = 0; i < 1200 && !s_stop; i++) {
        GA = (void*)GetModuleHandleW(L"GameAssembly.dll");
        if (GA) break;
        Sleep(500);
    }
    if (!GA || s_stop) return 0;
    auto imp = [GA](const char* n) -> void* { return (void*)GetProcAddress((HMODULE)GA, n); };
    il2cpp_domain_get      = (Pfn_domain_get)imp("il2cpp_domain_get");
    il2cpp_class_from_name = (Pfn_class_from_name)imp("il2cpp_class_from_name");
    il2cpp_thread_attach   = (Pfn_thread_attach)imp("il2cpp_thread_attach");
    il2cpp_runtime_invoke  = (Pfn_runtime_invoke)imp("il2cpp_runtime_invoke");
    il2cpp_string_new      = (Pfn_string_new)imp("il2cpp_string_new");
    il2cpp_value_box       = (Pfn_value_box)imp("il2cpp_value_box");
    il2cpp_gchandle_new    = (Pfn_gchandle_new)imp("il2cpp_gchandle_new");
    il2cpp_gchandle_free   = (Pfn_gchandle_free)imp("il2cpp_gchandle_free");
    if (!il2cpp_gchandle_new) { LOG_W("[mvec] il2cpp_gchandle_new missing"); s_resolved = -1; return 0; }
    typedef void** (*PfnAsm)(void*, size_t*);
    typedef void* (*PfnImg)(void*);
    typedef void* (*PfnMethod)(void* klass, const char* name, int32_t argc);
    auto getAsm = (PfnAsm)imp("il2cpp_domain_get_assemblies");
    auto getImg = (PfnImg)imp("il2cpp_assembly_get_image");
    auto getMethod = (PfnMethod)imp("il2cpp_class_get_method_from_name");
    if (!il2cpp_domain_get || !il2cpp_class_from_name || !il2cpp_runtime_invoke ||
        !il2cpp_string_new || !il2cpp_value_box || !getAsm || !getImg || !getMethod) {
        s_resolved = -1; return 0;
    }
    for (int i = 0; i < 600 && !s_stop; i++) {
        if (il2cpp_domain_get()) break;
        Sleep(500);
    }
    void* domain = il2cpp_domain_get();
    if (!domain || s_stop) { s_resolved = -1; return 0; }
    il2cpp_thread_attach(domain);

    // v20: 等渲染开始再解析 (元数据已初始化; 顺带等 demosaic 完成, 见 dllmain 串行化)
    for (int spin = 0; spin < 1200 && !s_stop && !s_rendering; spin++) Sleep(500);
    if (s_stop) return 0;

    auto findClass = [&](const char* ns, const char* name) -> void* {
        size_t n = 0;
        void** asms = getAsm(domain, &n);
        for (size_t i = 0; i < n; i++) {
            void* k = il2cpp_class_from_name(getImg(asms[i]), ns, name);
            if (k) return k;
        }
        return nullptr;
    };

    s_shaderClass = findClass("UnityEngine", "Shader");
    s_camClass    = findClass("UnityEngine", "Camera");
    void* texClass = findClass("UnityEngine", "Texture");
    void* intClass = findClass("System", "Int32");
    if (!s_shaderClass || !s_camClass || !texClass || !intClass) { s_resolved = -1; return 0; }
    s_getGlobalTex = getMethod(s_shaderClass, "GetGlobalTexture", 1);
    s_getMain      = getMethod(s_camClass, "get_main", 0);
    s_setDepthMode = getMethod(s_camClass, "set_depthTextureMode", 1);
    s_nativePtr    = getMethod(texClass, "GetNativeTexturePtr", 0);
    s_boxIntClass  = intClass;
    if (!s_getGlobalTex || !s_getMain || !s_setDepthMode || !s_nativePtr) {
        LOG_W("[mvec] methods missing (gtex=%p main=%p mode=%p nat=%p)",
              s_getGlobalTex, s_getMain, s_setDepthMode, s_nativePtr);
        s_resolved = -1; return 0;
    }
    s_resolved = 1;
    LOG_I("[mvec] metadata resolved, waiting main-thread poll");
    return 0;
}

void mvec_init() {
    if (s_thread) return;
    s_thread = CreateThread(nullptr, 0, mvec_worker, nullptr, 0, nullptr);
}

void mvec_stop() {
    s_stop = true;
    if (s_thread) { WaitForSingleObject(s_thread, 2000); CloseHandle(s_thread); s_thread = nullptr; }
    if (g_mvecSRV) { g_mvecSRV->Release(); g_mvecSRV = nullptr; }
    if (g_depthTex) { g_depthTex->Release(); g_depthTex = nullptr; }
    if (s_strMvecHandle)  { il2cpp_gchandle_free(s_strMvecHandle);  s_strMvecHandle = 0; }
    if (s_strDepthHandle) { il2cpp_gchandle_free(s_strDepthHandle); s_strDepthHandle = 0; }
}

} // namespace lillia
