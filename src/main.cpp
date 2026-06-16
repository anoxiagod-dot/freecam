#include <jni.h>
#include <android/log.h>
#include <android/input.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES3/gl3.h>
#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>
#include <string.h>
#include <sys/mman.h>
#include <fstream>
#include <string>
#include <cmath>

#include "pl/Hook.h"
#include "pl/Gloss.h"

#include "ImGui/imgui.h"
#include "ImGui/backends/imgui_impl_opengl3.h"
#include "ImGui/backends/imgui_impl_android.h"

#define TAG "FreeCam"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// ═══════════════════════════════════════════════════════
//  Состояние FreeCam
// ═══════════════════════════════════════════════════════
static bool  g_freecam_active = false;
static float g_speed          = 0.3f;

static float g_camX = 0.f, g_camY = 0.f, g_camZ = 0.f;
static float g_camYaw = 0.f, g_camPitch = 0.f;

static float g_moveF = 0.f;
static float g_moveR = 0.f;
static float g_moveU = 0.f;

// Значения слотов для отладки — выводим на экран
// slot_vals[N*2+0] и [N*2+1] — первые два float возвращаемого значения
static float g_slot_vals[32] = {};

// ═══════════════════════════════════════════════════════
//  Хуки VanillaCameraAPI
// ═══════════════════════════════════════════════════════
static int   (*g_orig_slot7)(void*)   = nullptr;
static void* (*g_orig_slot8)(void*)   = nullptr;
static void* (*g_orig_slot9)(void*)   = nullptr;
static void* (*g_orig_slot10)(void*)  = nullptr;
static void* (*g_orig_slot11)(void*)  = nullptr;
static void* (*g_orig_slot12)(void*)  = nullptr;

// Slot 7 = getPlayerViewPerspectiveOption (подтверждено snaplook.cpp)
static int hook_slot7(void* self) {
    if (g_freecam_active) return 0; // принудительно first-person
    return g_orig_slot7 ? g_orig_slot7(self) : 0;
}

// Slots 8-12 — логируем что возвращают (отображаем в ImGui)
// Если вернёт указатель на float — читаем первые 2 float
static void* hook_slot8(void* self) {
    void* r = g_orig_slot8 ? g_orig_slot8(self) : nullptr;
    if (r) { float* f = (float*)r; g_slot_vals[0] = f[0]; g_slot_vals[1] = f[1]; }
    return r;
}
static void* hook_slot9(void* self) {
    void* r = g_orig_slot9 ? g_orig_slot9(self) : nullptr;
    if (r) { float* f = (float*)r; g_slot_vals[2] = f[0]; g_slot_vals[3] = f[1]; }
    return r;
}
static void* hook_slot10(void* self) {
    void* r = g_orig_slot10 ? g_orig_slot10(self) : nullptr;
    if (r) { float* f = (float*)r; g_slot_vals[4] = f[0]; g_slot_vals[5] = f[1]; }
    return r;
}
static void* hook_slot11(void* self) {
    void* r = g_orig_slot11 ? g_orig_slot11(self) : nullptr;
    if (r) { float* f = (float*)r; g_slot_vals[6] = f[0]; g_slot_vals[7] = f[1]; }
    return r;
}
static void* hook_slot12(void* self) {
    void* r = g_orig_slot12 ? g_orig_slot12(self) : nullptr;
    if (r) { float* f = (float*)r; g_slot_vals[8] = f[0]; g_slot_vals[9] = f[1]; }
    return r;
}

// ═══════════════════════════════════════════════════════
//  Поиск vtable — ТОЧНЫЙ алгоритм из zoom.cpp/snaplook.cpp
// ═══════════════════════════════════════════════════════
static bool patchSlot(uintptr_t vt, int slot, void* hook, void** orig) {
    uintptr_t* ptr = (uintptr_t*)(vt + slot * sizeof(void*));
    *orig = (void*)(*ptr);
    uintptr_t pg = (uintptr_t)ptr & ~(uintptr_t)4095;
    if (mprotect((void*)pg, 4096, PROT_READ | PROT_WRITE) != 0) {
        LOGE("mprotect failed slot %d", slot);
        return false;
    }
    *ptr = (uintptr_t)hook;
    mprotect((void*)pg, 4096, PROT_READ);
    LOGI("Patched slot %d: orig=0x%lx hook=0x%lx", slot, (uintptr_t)*orig, (uintptr_t)hook);
    return true;
}

static bool findAndHookVanillaCameraAPI() {
    const char* typeinfoName = "16VanillaCameraAPI";
    size_t nameLen = strlen(typeinfoName);

    // STEP 1: найти строку имени typeinfo в r--p или r-xp секциях
    uintptr_t nameAddr = 0;
    {
        std::ifstream maps("/proc/self/maps");
        std::string line;
        while (std::getline(maps, line)) {
            if (line.find("libminecraftpe.so") == std::string::npos) continue;
            if (line.find("r--p") == std::string::npos &&
                line.find("r-xp") == std::string::npos) continue;
            uintptr_t s, e;
            if (sscanf(line.c_str(), "%lx-%lx", &s, &e) != 2) continue;
            for (uintptr_t a = s; a + nameLen < e; a++) {
                if (memcmp((void*)a, typeinfoName, nameLen) == 0 &&
                    ((char*)a)[nameLen] == '\0') {
                    nameAddr = a;
                    LOGI("typeinfo name at 0x%lx", nameAddr);
                    break;
                }
            }
            if (nameAddr) break;
        }
    }
    if (!nameAddr) { LOGE("typeinfo name not found"); return false; }

    // STEP 2: найти typeinfo (ссылается на имя) в r--p секциях
    uintptr_t tiAddr = 0;
    {
        std::ifstream maps("/proc/self/maps");
        std::string line;
        while (std::getline(maps, line)) {
            if (line.find("libminecraftpe.so") == std::string::npos) continue;
            if (line.find("r--p") == std::string::npos) continue;
            uintptr_t s, e;
            if (sscanf(line.c_str(), "%lx-%lx", &s, &e) != 2) continue;
            for (uintptr_t a = s; a + sizeof(void*) < e; a += sizeof(void*)) {
                if (*(uintptr_t*)a == nameAddr) {
                    tiAddr = a - sizeof(void*);
                    LOGI("typeinfo at 0x%lx", tiAddr);
                    break;
                }
            }
            if (tiAddr) break;
        }
    }
    if (!tiAddr) { LOGE("typeinfo not found"); return false; }

    // STEP 3: найти vtable (ссылается на typeinfo) в r--p секциях
    uintptr_t vtAddr = 0;
    {
        std::ifstream maps("/proc/self/maps");
        std::string line;
        while (std::getline(maps, line)) {
            if (line.find("libminecraftpe.so") == std::string::npos) continue;
            if (line.find("r--p") == std::string::npos) continue;
            uintptr_t s, e;
            if (sscanf(line.c_str(), "%lx-%lx", &s, &e) != 2) continue;
            for (uintptr_t a = s; a + sizeof(void*) < e; a += sizeof(void*)) {
                if (*(uintptr_t*)a == tiAddr) {
                    vtAddr = a + sizeof(void*);
                    LOGI("vtable at 0x%lx", vtAddr);
                    break;
                }
            }
            if (vtAddr) break;
        }
    }
    if (!vtAddr) { LOGE("vtable not found"); return false; }

    // STEP 4: патчим слоты
    patchSlot(vtAddr, 7,  (void*)hook_slot7,  (void**)&g_orig_slot7);
    patchSlot(vtAddr, 8,  (void*)hook_slot8,  (void**)&g_orig_slot8);
    patchSlot(vtAddr, 9,  (void*)hook_slot9,  (void**)&g_orig_slot9);
    patchSlot(vtAddr, 10, (void*)hook_slot10, (void**)&g_orig_slot10);
    patchSlot(vtAddr, 11, (void*)hook_slot11, (void**)&g_orig_slot11);
    patchSlot(vtAddr, 12, (void*)hook_slot12, (void**)&g_orig_slot12);

    return true;
}

// ═══════════════════════════════════════════════════════
//  ImGui / EGL
// ═══════════════════════════════════════════════════════
static bool       g_imgui_init  = false;
static int        g_width = 0, g_height = 0;
static EGLContext g_ctx   = EGL_NO_CONTEXT;
static EGLSurface g_surf  = EGL_NO_SURFACE;

static EGLBoolean (*orig_eglSwapBuffers)(EGLDisplay, EGLSurface) = nullptr;

static void tickMovement() {
    if (!g_freecam_active) return;
    if (g_moveF == 0.f && g_moveR == 0.f && g_moveU == 0.f) return;
    float yr = g_camYaw * (float)(M_PI / 180.0);
    float sy = sinf(yr), cy = cosf(yr);
    g_camX += (-g_moveF * sy + g_moveR * cy) * g_speed;
    g_camZ += ( g_moveF * cy + g_moveR * sy) * g_speed;
    g_camY += g_moveU * g_speed;
}

static void renderUI() {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)g_width, (float)g_height);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame(g_width, g_height);
    ImGui::NewFrame();

    // Кнопка
    ImGui::SetNextWindowPos(ImVec2(10.f, 10.f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(170.f, 55.f), ImGuiCond_Always);
    ImGui::Begin("##btn", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings);
    ImGui::SetWindowFontScale(1.4f);
    ImVec4 col = g_freecam_active
        ? ImVec4(0.1f, 0.6f, 0.2f, 0.92f)
        : ImVec4(0.1f, 0.1f, 0.1f, 0.85f);
    ImGui::PushStyleColor(ImGuiCol_Button, col);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
        ImVec4(col.x+0.1f, col.y+0.1f, col.z+0.1f, 1.f));
    if (ImGui::Button(g_freecam_active ? "FreeCam ON" : "FreeCam", ImVec2(150.f, 40.f)))
        g_freecam_active = !g_freecam_active;
    ImGui::PopStyleColor(2);
    ImGui::End();

    // Панель отладки слотов
    ImGui::SetNextWindowPos(ImVec2(10.f, 75.f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.78f);
    ImGui::Begin("##info", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
    ImGui::SetWindowFontScale(0.9f);
    ImGui::Text("Slot8:  %8.2f %8.2f", g_slot_vals[0], g_slot_vals[1]);
    ImGui::Text("Slot9:  %8.2f %8.2f", g_slot_vals[2], g_slot_vals[3]);
    ImGui::Text("Slot10: %8.2f %8.2f", g_slot_vals[4], g_slot_vals[5]);
    ImGui::Text("Slot11: %8.2f %8.2f", g_slot_vals[6], g_slot_vals[7]);
    ImGui::Text("Slot12: %8.2f %8.2f", g_slot_vals[8], g_slot_vals[9]);
    if (g_freecam_active) {
        ImGui::Separator();
        ImGui::Text("X:%.1f Y:%.1f Z:%.1f", g_camX, g_camY, g_camZ);
        ImGui::Text("Yaw:%.1f Pit:%.1f",     g_camYaw, g_camPitch);
        ImGui::SliderFloat("Spd", &g_speed, 0.05f, 3.f, "%.2f");
    }
    ImGui::End();

    // Джойстик и кнопки вверх/вниз (только при активном freecam)
    if (g_freecam_active) {
        float jR  = 65.f;
        float jCX = jR + 15.f;
        float jCY = io.DisplaySize.y - jR - 15.f;

        ImGui::SetNextWindowPos(ImVec2(0,0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.f);
        ImGui::Begin("##jbg", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddCircle(ImVec2(jCX,jCY), jR, IM_COL32(255,255,255,50), 32, 2.f);
        float kx = jCX + g_moveR*jR*0.5f;
        float ky = jCY - g_moveF*jR*0.5f;
        dl->AddCircleFilled(ImVec2(kx,ky), 16.f, IM_COL32(255,255,255,90));
        ImU32 ac = IM_COL32(255,255,255,150);
        dl->AddTriangleFilled(ImVec2(jCX,jCY-jR+6),ImVec2(jCX-10,jCY-jR+22),ImVec2(jCX+10,jCY-jR+22),ac);
        dl->AddTriangleFilled(ImVec2(jCX,jCY+jR-6),ImVec2(jCX-10,jCY+jR-22),ImVec2(jCX+10,jCY+jR-22),ac);
        dl->AddTriangleFilled(ImVec2(jCX-jR+6,jCY),ImVec2(jCX-jR+22,jCY-10),ImVec2(jCX-jR+22,jCY+10),ac);
        dl->AddTriangleFilled(ImVec2(jCX+jR-6,jCY),ImVec2(jCX+jR-22,jCY-10),ImVec2(jCX+jR-22,jCY+10),ac);
        ImGui::End();

        float bS  = 60.f;
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x-bS-15.f,
                                        io.DisplaySize.y-bS*2-20.f), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.f);
        ImGui::Begin("##ud", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f,0.1f,0.1f,0.75f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f,0.3f,0.3f,0.9f));
        ImGui::SetWindowFontScale(2.f);
        g_moveU = 0.f;
        if (ImGui::Button("^", ImVec2(bS,bS))) g_moveU =  1.f;
        if (ImGui::Button("v", ImVec2(bS,bS))) g_moveU = -1.f;
        ImGui::PopStyleColor(2);
        ImGui::End();
    }

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

static EGLBoolean hook_eglSwapBuffers(EGLDisplay dpy, EGLSurface surf) {
    if (!orig_eglSwapBuffers) return EGL_FALSE;

    EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT) return orig_eglSwapBuffers(dpy, surf);
    if (g_ctx != EGL_NO_CONTEXT && (ctx != g_ctx || surf != g_surf))
        return orig_eglSwapBuffers(dpy, surf);

    EGLint w = 0, h = 0;
    eglQuerySurface(dpy, surf, EGL_WIDTH,  &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    if (w < 100 || h < 100) return orig_eglSwapBuffers(dpy, surf);

    if (g_ctx == EGL_NO_CONTEXT) { g_ctx = ctx; g_surf = surf; }
    g_width = w; g_height = h;

    if (!g_imgui_init) {
        ImGui::CreateContext();
        ImGuiIO& io2 = ImGui::GetIO();
        io2.IniFilename     = nullptr;
        io2.FontGlobalScale = 1.3f;
        ImGui_ImplAndroid_Init();
        ImGui_ImplOpenGL3_Init("#version 300 es");
        g_imgui_init = true;
        LOGI("ImGui initialized %dx%d", w, h);
    }

    GLint lp, lfb, lvp[4];
    GLboolean lb, ld, ls;
    glGetIntegerv(GL_CURRENT_PROGRAM,     &lp);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &lfb);
    glGetIntegerv(GL_VIEWPORT,             lvp);
    lb = glIsEnabled(GL_BLEND);
    ld = glIsEnabled(GL_DEPTH_TEST);
    ls = glIsEnabled(GL_SCISSOR_TEST);

    tickMovement();
    renderUI();

    glUseProgram(lp);
    glBindFramebuffer(GL_FRAMEBUFFER, lfb);
    glViewport(lvp[0], lvp[1], lvp[2], lvp[3]);
    if (lb) glEnable(GL_BLEND);   else glDisable(GL_BLEND);
    if (ld) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (ls) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);

    return orig_eglSwapBuffers(dpy, surf);
}

// ═══════════════════════════════════════════════════════
//  Тач колбэк
// ═══════════════════════════════════════════════════════
typedef bool (*TouchCb)(int action, int pid, float x, float y);
struct PreloaderInput { void (*RegisterTouchCallback)(TouchCb); };
typedef PreloaderInput* (*GetPreloaderInput_t)();

static bool g_joyActive = false;
static float g_joyX0 = 0, g_joyY0 = 0;
static int   g_joyPid = -1;
static bool  g_rotActive = false;
static float g_rotLX = 0, g_rotLY = 0;
static int   g_rotPid = -1;

static bool onTouch(int action, int pid, float x, float y) {
    if (!g_imgui_init) return false;

    ImGuiIO& io = ImGui::GetIO();
    if (pid == 0) {
        io.AddMousePosEvent(x, y);
        if (action == AMOTION_EVENT_ACTION_DOWN)
            io.AddMouseButtonEvent(0, true);
        else if (action == AMOTION_EVENT_ACTION_UP ||
                 action == AMOTION_EVENT_ACTION_CANCEL)
            io.AddMouseButtonEvent(0, false);
    }
    if (io.WantCaptureMouse) return true;
    if (!g_freecam_active) return false;

    float jR  = 65.f;
    float jCX = jR + 15.f;
    float jCY = (float)(g_height > 0 ? g_height : 2400) - jR - 15.f;
    auto inJoy = [&](float px, float py) {
        float dx = px-jCX, dy = py-jCY;
        return dx*dx+dy*dy <= jR*jR;
    };

    if (action == AMOTION_EVENT_ACTION_DOWN ||
        action == AMOTION_EVENT_ACTION_POINTER_DOWN) {
        if (inJoy(x,y) && g_joyPid < 0) {
            g_joyActive=true; g_joyPid=pid;
            g_joyX0=x; g_joyY0=y;
            g_moveF=g_moveR=0.f;
            return true;
        }
        if (!inJoy(x,y) && g_rotPid < 0) {
            g_rotActive=true; g_rotPid=pid;
            g_rotLX=x; g_rotLY=y;
            return false;
        }
    }
    if (action == AMOTION_EVENT_ACTION_MOVE) {
        if (g_joyActive && pid == g_joyPid) {
            float dx = (x-g_joyX0)/jR;
            float dy = (y-g_joyY0)/jR;
            if (dx >  1.f) dx =  1.f; if (dx < -1.f) dx = -1.f;
            if (dy >  1.f) dy =  1.f; if (dy < -1.f) dy = -1.f;
            g_moveR =  dx;
            g_moveF = -dy;
            return true;
        }
        if (g_rotActive && pid == g_rotPid) {
            g_camYaw   += (x - g_rotLX) * 0.2f;
            g_camPitch += (y - g_rotLY) * 0.2f;
            if (g_camPitch >  89.f) g_camPitch =  89.f;
            if (g_camPitch < -89.f) g_camPitch = -89.f;
            g_rotLX=x; g_rotLY=y;
            return false;
        }
    }
    if (action == AMOTION_EVENT_ACTION_UP ||
        action == AMOTION_EVENT_ACTION_POINTER_UP ||
        action == AMOTION_EVENT_ACTION_CANCEL) {
        if (pid == g_joyPid) {
            g_joyActive=false; g_joyPid=-1;
            g_moveF=g_moveR=0.f;
        }
        if (pid == g_rotPid) {
            g_rotActive=false; g_rotPid=-1;
        }
    }
    return false;
}

// ═══════════════════════════════════════════════════════
//  Главный поток
// ═══════════════════════════════════════════════════════
static void* mainThread(void*) {
    LOGI("FreeCam v4 starting");

    // Ждём появления libminecraftpe.so в памяти
    for (int i = 0; i < 300; i++) { // max 30 секунд
        std::ifstream maps("/proc/self/maps");
        std::string line;
        bool found = false;
        while (std::getline(maps, line)) {
            if (line.find("libminecraftpe.so") != std::string::npos &&
                line.find("r--p") != std::string::npos) {
                found = true; break;
            }
        }
        if (found) break;
        usleep(100000);
    }
    sleep(1); // чуть ждём после появления

    LOGI("libminecraftpe.so ready, hooking...");
    GlossInit(true);

    // eglSwapBuffers
    void* eglLib = dlopen("libEGL.so", RTLD_NOW);
    if (eglLib) {
        void* sym = dlsym(eglLib, "eglSwapBuffers");
        if (sym) {
            GlossHook(sym, (void*)hook_eglSwapBuffers, (void**)&orig_eglSwapBuffers);
            LOGI("eglSwapBuffers hooked");
        }
    }

    // VanillaCameraAPI
    findAndHookVanillaCameraAPI();

    // Touch
    void* preLib = dlopen("libpreloader.so", RTLD_NOW);
    if (preLib) {
        auto getInput = (GetPreloaderInput_t)dlsym(preLib, "GetPreloaderInput");
        if (getInput) {
            PreloaderInput* inp = getInput();
            if (inp && inp->RegisterTouchCallback) {
                inp->RegisterTouchCallback(onTouch);
                LOGI("Touch registered");
            }
        }
    }

    LOGI("FreeCam v4 ready!");
    return nullptr;
}

__attribute__((constructor))
void freecam_init() {
    pthread_t t;
    pthread_create(&t, nullptr, mainThread, nullptr);
    pthread_detach(t);
}
