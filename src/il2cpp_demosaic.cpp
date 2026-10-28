// il2cpp_demosaic.cpp — 去马赛克 (只处理女主)
//
// 本作实现 (逆向自 dump.cs / GameAssembly.dll):
//   * DressController 持有 List<Renderer> _mosaicRenderers, 内容来自扫描 targetAvatar 下
//     所有 Renderer, 条件是材质的名字命中 mosaic 关键字 (本作为 "StrictMosaic")。
//   * UpdateMosaicVisibility() 每帧按 `IsDoingSex || !IsPartsWorn(Panties)` 决定这些
//     renderer 的 enabled —— 也就是说游戏自己在不停地打开/关闭它们。
//   * 这些 renderer 是贴在身体表面的独立贴片 (Pussy_Mosaic / Sphere), 不是全屏后期。
//
// 对策:
//   A. hook UpdateMosaicVisibility / Update: 跑完原函数后再把列表里的 renderer 全关掉,
//      每帧覆盖, 游戏翻不回来。
//   B. 这些 renderer 的材质只有 mosaic 一种 (专用贴片) 时, 顺手把材质换成透明 shader,
//      这样即使有其它逻辑把 renderer 打开, 也画不出东西。
//   C. 全局扫描: 没有 DressController 管的同类贴片 (其它女主/NPC) 一并处理。
//
// 已放弃的路径 (记录备查):
//   * male 的 LIDPatched shader: _LID_MosaicEnabled 等参数在材质里就是 0, 没有相关
//     shader 关键字, 也没有独立覆盖网格, 像素化是烤死在 shader/贴图里的; 替换 shader
//     只会变成品红色, 因此不做处理。

#include "common.h"
#include "config.h"
#include "il2cpp_demosaic.h"
#include "il2cpp_mvec.h"

#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "MinHook.h"

using lillia::cfg;

// ---------------------------------------------------------------- il2cpp ABI
typedef void* (*Pfn_domain_get)();
typedef void** (*Pfn_domain_get_assemblies)(void*, size_t*);
typedef void* (*Pfn_assembly_get_image)(void*);
typedef const char* (*Pfn_image_get_name)(void*);
typedef void* (*Pfn_class_from_name)(void*, const char*, const char*);
typedef const void* (*Pfn_class_get_type)(void*);
typedef void* (*Pfn_type_get_object)(const void*);
typedef void* (*Pfn_class_get_method_from_name)(void*, const char*, int32_t);
typedef void* (*Pfn_class_get_methods)(void*, void**);
typedef const char* (*Pfn_method_get_name)(void*);
typedef const void* (*Pfn_method_get_param)(const void*, uint32_t);
typedef const char* (*Pfn_type_get_name)(const void*);
typedef void* (*Pfn_thread_attach)(void*);
typedef void* (*Pfn_runtime_invoke)(void*, void*, void**, void**);
typedef void* (*Pfn_value_box)(void*, void*);
typedef const uint16_t* (*Pfn_string_chars)(void*);
typedef int32_t (*Pfn_string_length)(void*);
typedef void* (*Pfn_string_new)(const char*);
typedef uint32_t (*Pfn_gchandle_new)(void*, bool);
typedef void* (*Pfn_gchandle_target)(uint32_t);
typedef void* (*Pfn_class_get_field_from_name)(void*, const char*);
typedef int32_t (*Pfn_field_get_offset)(void*);

static Pfn_domain_get                  il2cpp_domain_get;
static Pfn_domain_get_assemblies       il2cpp_domain_get_assemblies;
static Pfn_assembly_get_image          il2cpp_assembly_get_image;
static Pfn_image_get_name              il2cpp_image_get_name;
static Pfn_class_from_name             il2cpp_class_from_name;
static Pfn_class_get_type              il2cpp_class_get_type;
static Pfn_type_get_object             il2cpp_type_get_object;
static Pfn_class_get_method_from_name  il2cpp_class_get_method_from_name;
static Pfn_class_get_methods           il2cpp_class_get_methods;
static Pfn_method_get_name             il2cpp_method_get_name;
static Pfn_method_get_param            il2cpp_method_get_param;
static Pfn_type_get_name               il2cpp_type_get_name;
static Pfn_thread_attach               il2cpp_thread_attach;
static Pfn_runtime_invoke              il2cpp_runtime_invoke;
static Pfn_value_box                   il2cpp_value_box;
static Pfn_string_chars                il2cpp_string_chars;
static Pfn_string_length               il2cpp_string_length;
static Pfn_string_new                  il2cpp_string_new;
static Pfn_gchandle_new                il2cpp_gchandle_new;
static Pfn_gchandle_target             il2cpp_gchandle_target;
static Pfn_class_get_field_from_name   il2cpp_class_get_field_from_name;
static Pfn_field_get_offset            il2cpp_field_get_offset;

// ---------------------------------------------------------------- 解析结果
static void* s_dcKlass       = nullptr;   // DressController
static void* s_dcType        = nullptr;   // 其 System.Type (FindObjectsOfType 用)
static void* s_visMethod     = nullptr;   // DressController::UpdateMosaicVisibility
static int32_t s_offList     = -1;        // _mosaicRenderers : List<Renderer>
static int32_t s_offAvatar   = -1;        // targetAvatar    : GameObject

static void* s_booleanKlass  = nullptr;
static void* s_transformKlass = nullptr;
static void* s_setEnabled    = nullptr;   // Renderer::set_enabled
static void* s_getSharedMats = nullptr;   // Renderer::get_sharedMaterials
static void* s_findObjects   = nullptr;   // Object::FindObjectsOfType(Type)
static void* s_getName       = nullptr;   // Object::get_name
static void* s_getShader     = nullptr;   // Material::get_shader
static void* s_setShader     = nullptr;   // Material::set_shader
static void* s_getMainTex    = nullptr;   // Material::get_mainTexture
static void* s_setMainTex    = nullptr;   // Material::set_mainTexture
static void* s_setColor      = nullptr;   // Material::set_color
static void* s_getTransform  = nullptr;   // Component::get_transform
static void* s_getParent     = nullptr;   // Transform::get_parent
static void* s_shaderFind    = nullptr;   // Shader::Find

static void* s_rendererType  = nullptr;   // Renderer 的 System.Type
static void* s_nukeShader    = nullptr;   // 透明替换 shader (Sprites/Default)

static bool  s_scanReady = false;

// ---------------------------------------------------------------- 运行状态
static HANDLE s_thread = nullptr;
static volatile LONG s_status = 0;
static volatile LONG s_rendering = 0;
static volatile bool s_stop = false;
static volatile LONG s_hits = 0;            // hook 命中次数
static volatile LONG s_renderersOff = 0;    // set_enabled(false) 调用次数
static volatile LONG s_dcCount = 0;         // 最近一次扫描到的 DressController 数

// 已换过 shader 的材质 (主线程独占, 无锁)
struct NukeRec { void* mat; void* shader; void* tex; };
static NukeRec s_nukes[256];
static int     s_nukeN = 0;

// ---------------------------------------------------------------- 小工具
static void* invoke(void* method, void* obj, void** params) {
    if (!method) return nullptr;
    void* exc = nullptr;
    void* r = il2cpp_runtime_invoke(method, obj, params, &exc);
    if (exc) { static bool once = false; if (!once) { LOG_W("[demosaic] invoke threw"); once = true; } return nullptr; }
    return r;
}

// Il2CppArray: klass(0) monitor(8) bounds(0x10) max_length(0x18) vector(0x20)
static inline uint64_t arr_len(void* a)  { return *(uint32_t*)((uint8_t*)a + 0x18); }
static inline void**   arr_data(void* a) { return (void**)((uint8_t*)a + 0x20); }

// List<T>: _items@0x10 (T[] 对象, 元素在 +0x20), _size@0x18 (int32), _version@0x1C
static inline uint64_t list_size(void* l) { return *(uint32_t*)((uint8_t*)l + 0x18); }
static inline void**   list_items(void* l) {
    void* arr = *(void**)((uint8_t*)l + 0x10);
    return arr ? (void**)((uint8_t*)arr + 0x20) : nullptr;
}

static void toAsciiName(void* il2cppStr, char* out, int cap) {
    if (!il2cppStr) { strcpy_s(out, (size_t)cap, "?"); return; }
    int32_t len = il2cpp_string_length(il2cppStr);
    if (len < 0) len = 0;
    const uint16_t* c = il2cpp_string_chars(il2cppStr);
    int w = 0;
    for (int32_t i = 0; i < len && w < cap - 1; i++)
        out[w++] = (c[i] >= 32 && c[i] < 127) ? (char)c[i] : '?';
    out[w] = 0;
}

static bool nameMatches(void* il2cppStr, const char* keyLower) {
    if (!il2cppStr) return false;
    int32_t n = il2cpp_string_length(il2cppStr);
    if (n <= 0 || n > 512) return false;
    const uint16_t* c = il2cpp_string_chars(il2cppStr);
    int32_t klen = (int32_t)strlen(keyLower);
    for (int32_t i = 0; i + klen <= n; i++) {
        int32_t j = 0;
        for (; j < klen; j++) {
            uint16_t ch = c[i + j];
            if (ch >= 'A' && ch <= 'Z') ch += 32;
            if (ch != (uint16_t)(unsigned char)keyLower[j]) break;
        }
        if (j == klen) return true;
    }
    return false;
}

// "StrictMosaic" 命中的就是女主那两块贴片; 韩文/日文工程也一并兼容
static bool nameIsMosaic(void* s) {
    static const char* keys[] = { "mosaic", "mozaic", "mozaik", "mosaik" };
    for (auto k : keys) if (nameMatches(s, k)) return true;
    return false;
}

// 对象名 → 层级路径 (Renderer/Transform 这类 Component 才可用)
static void objPath(void* obj, char* out, size_t cap) {
    out[0] = 0;
    if (!obj || !s_getTransform) return;
    char tmp[512]; tmp[0] = 0;
    void* tr = invoke(s_getTransform, obj, nullptr);
    int depth = 0;
    while (tr && depth < 8) {
        if (s_transformKlass && *(void**)tr != s_transformKlass) break;
        char nm[96] = "?";
        toAsciiName(invoke(s_getName, tr, nullptr), nm, sizeof(nm));
        char next[512];
        _snprintf_s(next, sizeof(next), _TRUNCATE, depth ? "%s/%s" : "%s", nm, tmp);
        strcpy_s(tmp, next);
        tr = invoke(s_getParent, tr, nullptr);
        depth++;
    }
    strcpy_s(out, cap, tmp);
}

// ---------------------------------------------------------------- 材质处理
static int findNuke(void* mat) {
    for (int i = 0; i < s_nukeN; i++) if (s_nukes[i].mat == mat) return i;
    return -1;
}

static void matNuke(void* mat) {
    if (!mat || !s_nukeShader || findNuke(mat) >= 0) return;
    void* origShader = invoke(s_getShader, mat, nullptr);
    void* origTex = s_getMainTex ? invoke(s_getMainTex, mat, nullptr) : nullptr;
    { void* p[1] = { s_nukeShader }; invoke(s_setShader, mat, p); }
    if (s_setMainTex) { void* p[1] = { nullptr }; invoke(s_setMainTex, mat, p); }
    if (s_setColor) { float clear[4] = { 0, 0, 0, 0 }; void* p[1] = { clear }; invoke(s_setColor, mat, p); }
    if (s_nukeN < 256) s_nukes[s_nukeN++] = { mat, origShader, origTex };
}

static void matRestoreAll() {
    for (int i = 0; i < s_nukeN; i++) {
        if (!s_nukes[i].mat) continue;
        if (s_nukes[i].shader) { void* p[1] = { s_nukes[i].shader }; invoke(s_setShader, s_nukes[i].mat, p); }
        if (s_nukes[i].tex && s_setMainTex) { void* p[1] = { s_nukes[i].tex }; invoke(s_setMainTex, s_nukes[i].mat, p); }
    }
    if (s_nukeN) LOG_I("[demosaic] restored %d material(s)", s_nukeN);
    s_nukeN = 0;
}

// 该 renderer 的所有材质都是 mosaic 材质 → 它是一个专用贴片
static bool rendererIsDedicated(void* r, void** outMats, uint64_t* outN) {
    void* arr = invoke(s_getSharedMats, r, nullptr);
    *outMats = arr; *outN = 0;
    if (!arr) return false;
    uint64_t n = arr_len(arr);
    if (n == 0 || n > 64) return false;
    *outN = n;
    void** d = arr_data(arr);
    for (uint64_t i = 0; i < n; i++) {
        if (!d[i] || !nameIsMosaic(invoke(s_getName, d[i], nullptr))) return false;
    }
    return true;
}

static void callSetEnabled(void* r, bool enabled) {
    if (!s_setEnabled || !r || !s_booleanKlass) return;
    uint8_t raw = enabled ? 1 : 0;
    void* boxed = il2cpp_value_box(s_booleanKlass, &raw);
    if (!boxed) return;
    void* p[1] = { boxed };
    void* exc = nullptr;
    il2cpp_runtime_invoke(s_setEnabled, r, p, &exc);
    if (!exc) InterlockedIncrement(&s_renderersOff);
}

// ---------------------------------------------------------------- A + B
static void forceHide(void* self) {
    if (!self || s_offList < 0 || *(void**)self != s_dcKlass) return;
    void* list = *(void**)((uint8_t*)self + s_offList);
    if (!list) return;
    uint64_t n = list_size(list);
    if (n == 0 || n > 128) return;
    void** items = list_items(list);
    if (!items) return;
    bool nuke = cfg().mosaicNukeShader;
    for (uint64_t i = 0; i < n; i++) {
        void* r = items[i];
        if (!r) continue;
        void* mats = nullptr; uint64_t mn = 0;
        if (rendererIsDedicated(r, &mats, &mn)) {
            void** md = arr_data(mats);
            for (uint64_t k = 0; k < mn; k++) if (md[k] && nuke) matNuke(md[k]);
        }
        callSetEnabled(r, false);
    }
}

// ---------------------------------------------------------------- C
static void scanAllRenderers() {
    if (!s_rendererType || !s_findObjects) return;
    void* p = s_rendererType;
    void* arr = invoke(s_findObjects, nullptr, &p);
    if (!arr) return;
    uint64_t n = arr_len(arr);
    if (n == 0 || n > 30000) return;
    void** d = arr_data(arr);
    int hits = 0;
    bool nuke = cfg().mosaicNukeShader;
    for (uint64_t i = 0; i < n && hits < 48; i++) {
        void* r = d[i];
        if (!r) continue;
        void* mats = nullptr; uint64_t mn = 0;
        if (!rendererIsDedicated(r, &mats, &mn)) continue;
        void** md = arr_data(mats);
        for (uint64_t k = 0; k < mn; k++) if (md[k] && nuke) matNuke(md[k]);
        callSetEnabled(r, false);
        if (hits++ < 4) {
            char rp[256]; objPath(r, rp, sizeof(rp));
            LOG_I("[demosaic] global mosaic renderer off: '%s' (%llu mat)", rp, (unsigned long long)mn);
        }
    }
}

// ---------------------------------------------------------------- 主线程 tick
static void scanMainthread() {
    if (!s_scanReady) return;
    if (!cfg().disableMosaic) { matRestoreAll(); return; }
    if (!s_nukeShader && s_shaderFind) {
        void* s = il2cpp_string_new("Sprites/Default");
        void* p[1] = { s };
        void* sh = invoke(s_shaderFind, nullptr, p);
        if (sh && *(void**)sh) { s_nukeShader = sh; LOG_I("[demosaic] nuke shader = Sprites/Default"); }
    }
    if (s_dcType) {
        void* p = s_dcType;
        void* arr = invoke(s_findObjects, nullptr, &p);
        if (arr) {
            uint64_t n = arr_len(arr);
            if (n <= 4096) {
                void** d = arr_data(arr);
                LONG cnt = 0;
                for (uint64_t i = 0; i < n; i++) {
                    void* o = d[i];
                    if (!o || *(void**)o != s_dcKlass) continue;
                    cnt++;
                    forceHide(o);
                }
                InterlockedExchange(&s_dcCount, cnt);
            }
        }
    }
    scanAllRenderers();
}

static void mainthreadTick() {
    static uint32_t tick = 0;
    tick++;
    if (cfg().upscaleEnabled) lillia::mvec_poll_mainthread();   // mvec 只服务 FSR
    if ((tick % 180) == 1) scanMainthread();
}

// ---------------------------------------------------------------- hooks
static void* s_updateOrig = nullptr;
typedef void (*UpdateFn)(void* self, const void* methodInfo);

static void WINAPI hk_Update(void* self, const void* methodInfo) {
    if (s_updateOrig) ((UpdateFn)s_updateOrig)(self, methodInfo);   // 先让游戏跑完
    if (self && *(void**)self == s_dcKlass && cfg().disableMosaic) {
        forceHide(self);                                            // 再覆盖本帧状态
        InterlockedIncrement(&s_hits);
    }
    mainthreadTick();
}

static void* s_visOrig = nullptr;
static void WINAPI hk_Vis(void* self, const void* methodInfo) {
    if (s_visOrig) ((UpdateFn)s_visOrig)(self, methodInfo);
    if (self && cfg().disableMosaic) {
        forceHide(self);
        InterlockedIncrement(&s_hits);
    }
}

// ---------------------------------------------------------------- 元数据解析
static bool resolveAll() {
    size_t count = 0;
    void* domain = il2cpp_domain_get();
    void** assemblies = il2cpp_domain_get_assemblies(domain, &count);
    void* coreImage = nullptr;
    void* gameImage = nullptr;
    for (size_t a = 0; a < count; a++) {
        void* img = assemblies[a] ? il2cpp_assembly_get_image(assemblies[a]) : nullptr;
        const char* in = il2cpp_image_get_name(img);
        if (!in) continue;
        if (strcmp(in, "UnityEngine.CoreModule.dll") == 0) coreImage = img;
        else if (strcmp(in, "Assembly-CSharp.dll") == 0) gameImage = img;
    }
    if (!coreImage || !gameImage) {
        LOG_W("[demosaic] image missing (core=%p game=%p)", coreImage, gameImage);
        return false;
    }

    void* rendererK = il2cpp_class_from_name(coreImage, "UnityEngine", "Renderer");
    void* objectK   = il2cpp_class_from_name(coreImage, "UnityEngine", "Object");
    void* materialK = il2cpp_class_from_name(coreImage, "UnityEngine", "Material");
    void* shaderK   = il2cpp_class_from_name(coreImage, "UnityEngine", "Shader");
    void* compK     = il2cpp_class_from_name(coreImage, "UnityEngine", "Component");
    void* transK    = il2cpp_class_from_name(coreImage, "UnityEngine", "Transform");
    if (!rendererK || !objectK || !materialK || !shaderK) return false;
    s_transformKlass = transK;

    s_setEnabled    = il2cpp_class_get_method_from_name(rendererK, "set_enabled", 1);
    s_getSharedMats = il2cpp_class_get_method_from_name(rendererK, "get_sharedMaterials", 0);
    s_findObjects   = il2cpp_class_get_method_from_name(objectK, "FindObjectsOfType", 1);
    s_getName       = il2cpp_class_get_method_from_name(objectK, "get_name", 0);
    s_getShader     = il2cpp_class_get_method_from_name(materialK, "get_shader", 0);
    s_setShader     = il2cpp_class_get_method_from_name(materialK, "set_shader", 1);
    s_getMainTex    = il2cpp_class_get_method_from_name(materialK, "get_mainTexture", 0);
    s_setMainTex    = il2cpp_class_get_method_from_name(materialK, "set_mainTexture", 1);
    s_setColor      = il2cpp_class_get_method_from_name(materialK, "set_color", 1);
    s_shaderFind    = il2cpp_class_get_method_from_name(shaderK, "Find", 1);
    s_getTransform  = compK  ? il2cpp_class_get_method_from_name(compK, "get_transform", 0) : nullptr;
    s_getParent     = transK ? il2cpp_class_get_method_from_name(transK, "get_parent", 0) : nullptr;

    if (!s_setEnabled || !s_getSharedMats || !s_findObjects || !s_getName || !s_setShader) {
        LOG_W("[demosaic] methods missing (en=%p mats=%p find=%p name=%p setShader=%p)",
              s_setEnabled, s_getSharedMats, s_findObjects, s_getName, s_setShader);
        return false;
    }

    {   // Boolean 装箱 (set_enabled)
        for (size_t a = 0; a < count && !s_booleanKlass; a++)
            s_booleanKlass = il2cpp_class_from_name(il2cpp_assembly_get_image(assemblies[a]), "System", "Boolean");
        if (!s_booleanKlass) return false;
    }
    {   // Renderer 的 System.Type (全局扫描用)
        const void* rt = il2cpp_class_get_type(rendererK);
        s_rendererType = rt ? il2cpp_type_get_object(rt) : nullptr;
    }

    s_dcKlass = il2cpp_class_from_name(gameImage, "", "DressController");
    if (!s_dcKlass) { LOG_W("[demosaic] DressController not found"); return false; }

    void* f = il2cpp_class_get_field_from_name(s_dcKlass, "_mosaicRenderers");
    s_offList = f ? il2cpp_field_get_offset(f) : -1;
    f = il2cpp_class_get_field_from_name(s_dcKlass, "targetAvatar");
    s_offAvatar = f ? il2cpp_field_get_offset(f) : -1;
    if (s_offList < 0) { LOG_W("[demosaic] _mosaicRenderers offset missing"); return false; }

    const void* t = il2cpp_class_get_type(s_dcKlass);
    s_dcType = t ? il2cpp_type_get_object(t) : nullptr;
    s_visMethod = il2cpp_class_get_method_from_name(s_dcKlass, "UpdateMosaicVisibility", 0);

    LOG_I("[demosaic] DressController list@%d avatar@%d vis=%p", s_offList, s_offAvatar, s_visMethod);
    s_scanReady = true;
    return true;
}

static void installHooks() {
    if (s_visMethod) {
        void* fn = *(void**)s_visMethod;
        if (fn && MH_CreateHook(fn, (void*)hk_Vis, &s_visOrig) == MH_OK && s_visOrig && MH_EnableHook(fn) == MH_OK)
            LOG_I("[demosaic] hooked UpdateMosaicVisibility @ %p", fn);
        else
            LOG_W("[demosaic] UpdateMosaicVisibility hook failed");
    }
    if (s_dcKlass) {
        void* m = il2cpp_class_get_method_from_name(s_dcKlass, "Update", 0);
        void* fn = m ? *(void**)m : nullptr;
        if (fn && MH_CreateHook(fn, (void*)hk_Update, &s_updateOrig) == MH_OK && s_updateOrig && MH_EnableHook(fn) == MH_OK)
            LOG_I("[demosaic] heartbeat hooked DressController::Update @ %p", fn);
        else
            LOG_W("[demosaic] Update hook failed");
    }
}

// ---------------------------------------------------------------- worker
static DWORD WINAPI demosaic_thread(LPVOID) {
    void* GA = nullptr;
    for (int spin = 0; spin < 1200 && !s_stop; spin++) {
        GA = (void*)GetModuleHandleW(L"GameAssembly.dll");
        if (GA) break;
        Sleep(500);
    }
    if (!GA || s_stop) return 0;

    auto imp = [GA](const char* n) -> void* { return (void*)GetProcAddress((HMODULE)GA, n); };
    il2cpp_domain_get                = (Pfn_domain_get)imp("il2cpp_domain_get");
    il2cpp_domain_get_assemblies     = (Pfn_domain_get_assemblies)imp("il2cpp_domain_get_assemblies");
    il2cpp_assembly_get_image        = (Pfn_assembly_get_image)imp("il2cpp_assembly_get_image");
    il2cpp_image_get_name            = (Pfn_image_get_name)imp("il2cpp_image_get_name");
    il2cpp_class_from_name           = (Pfn_class_from_name)imp("il2cpp_class_from_name");
    il2cpp_class_get_type            = (Pfn_class_get_type)imp("il2cpp_class_get_type");
    il2cpp_type_get_object           = (Pfn_type_get_object)imp("il2cpp_type_get_object");
    il2cpp_class_get_method_from_name= (Pfn_class_get_method_from_name)imp("il2cpp_class_get_method_from_name");
    il2cpp_class_get_methods         = (Pfn_class_get_methods)imp("il2cpp_class_get_methods");
    il2cpp_method_get_name           = (Pfn_method_get_name)imp("il2cpp_method_get_name");
    il2cpp_method_get_param          = (Pfn_method_get_param)imp("il2cpp_method_get_param");
    il2cpp_type_get_name             = (Pfn_type_get_name)imp("il2cpp_type_get_name");
    il2cpp_thread_attach             = (Pfn_thread_attach)imp("il2cpp_thread_attach");
    il2cpp_runtime_invoke            = (Pfn_runtime_invoke)imp("il2cpp_runtime_invoke");
    il2cpp_value_box                 = (Pfn_value_box)imp("il2cpp_value_box");
    il2cpp_string_chars              = (Pfn_string_chars)imp("il2cpp_string_chars");
    il2cpp_string_length             = (Pfn_string_length)imp("il2cpp_string_length");
    il2cpp_string_new                = (Pfn_string_new)imp("il2cpp_string_new");
    il2cpp_gchandle_new              = (Pfn_gchandle_new)imp("il2cpp_gchandle_new");
    il2cpp_gchandle_target           = (Pfn_gchandle_target)imp("il2cpp_gchandle_get_target");
    il2cpp_class_get_field_from_name = (Pfn_class_get_field_from_name)imp("il2cpp_class_get_field_from_name");
    il2cpp_field_get_offset          = (Pfn_field_get_offset)imp("il2cpp_field_get_offset");

    if (!il2cpp_domain_get || !il2cpp_domain_get_assemblies || !il2cpp_class_from_name ||
        !il2cpp_runtime_invoke || !il2cpp_value_box || !il2cpp_class_get_type ||
        !il2cpp_type_get_object || !il2cpp_string_chars || !il2cpp_string_length ||
        !il2cpp_class_get_field_from_name || !il2cpp_field_get_offset) {
        LOG_E("[demosaic] il2cpp exports missing");
        s_status = -1;
        return 0;
    }

    for (int spin = 0; spin < 600 && !s_stop; spin++) {
        if (il2cpp_domain_get()) break;
        Sleep(500);
    }
    void* domain = il2cpp_domain_get();
    if (!domain || s_stop) { s_status = -1; return 0; }
    il2cpp_thread_attach(domain);

    for (int spin = 0; spin < 1200 && !s_stop && !s_rendering; spin++) Sleep(500);
    if (s_stop) return 0;

    if (!resolveAll()) { LOG_W("[demosaic] resolve failed"); s_status = -1; return 0; }
    installHooks();
    s_status = 1;
    LOG_I("[demosaic] ready (female only: force-hide + transparent material)");
    return 0;
}

// ---------------------------------------------------------------- public API
namespace lillia {

void demosaic_notify_rendering() { InterlockedExchange(&s_rendering, 1); }

void demosaic_start() {
    if (s_thread) return;
    s_thread = CreateThread(nullptr, 0, demosaic_thread, nullptr, 0, nullptr);
}

void demosaic_stop() {
    s_stop = true;
    if (s_thread) { WaitForSingleObject(s_thread, 3000); CloseHandle(s_thread); s_thread = nullptr; }
}

int demosaic_status()            { return s_status; }
int demosaic_hits()              { return s_hits; }
int demosaic_renderers_disabled(){ return s_renderersOff; }
int demosaic_dc_count()          { return s_dcCount; }

} // namespace lillia
