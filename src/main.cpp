#include <jni.h>
#include <android/log.h>
#include <android/input.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES3/gl3.h>
#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <string.h>
#include <fstream>
#include <string>
#include <cmath>
#include <mutex>

#include "pl/Hook.h"
#include "pl/Gloss.h"

#include "ImGui/imgui.h"
#include "ImGui/backends/imgui_impl_opengl3.h"
#include "ImGui/backends/imgui_impl_android.h"

#define TAG "FreeCam"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// ── состояние FreeCam ─────────────────────────────────────────────────────────
static bool  g_freecam_active = false;
static float g_camX = 0.f, g_camY = 0.f, g_camZ = 0.f;
static float g_camYaw = 0.f, g_camPitch = 0.f;
static float g_speed = 0.3f;

// движение через джойстик (WASD на экране)
static float g_moveF = 0.f; // вперёд/назад  -1..1
static float g_moveR = 0.f; // влево/вправо  -1..1
static float g_moveU = 0.f; // вверх/вниз    -1..1

// ── ImGui / EGL ───────────────────────────────────────────────────────────────
static bool        g_initialized   = false;
static int         g_width = 0, g_height = 0;
static EGLContext  g_targetcontext = EGL_NO_CONTEXT;
static EGLSurface  g_targetsurface = EGL_NO_SURFACE;
static EGLBoolean (*orig_eglswapbuffers)(EGLDisplay, EGLSurface) = nullptr;

// ── хуки камеры ──────────────────────────────────────────────────────────────
static void (*g_orig_getCamPos)(void*, float*)  = nullptr;
static void (*g_orig_getCamRot)(void*, float*)  = nullptr;
static int  (*g_orig_getPerspective)(void*)     = nullptr;

static void hook_getCamPos(void* self, float* out) {
    if (g_freecam_active && out) {
        out[0] = g_camX; out[1] = g_camY; out[2] = g_camZ;
        return;
    }
    if (g_orig_getCamPos) g_orig_getCamPos(self, out);
}

static void hook_getCamRot(void* self, float* out) {
    if (g_freecam_active && out) {
        out[0] = g_camYaw; out[1] = g_camPitch;
        return;
    }
    if (g_orig_getCamRot) g_orig_getCamRot(self, out);
}

static int hook_getPerspective(void* self) {
    if (g_freecam_active) return 0;
    return g_orig_getPerspective ? g_orig_getPerspective(self) : 0;
}

// ── поиск vtable ──────────────────────────────────────────────────────────────
static uintptr_t findVtable(const char* name) {
    size_t len = strlen(name);
    std::ifstream maps("/proc/self/maps");
    std::string line;
    uintptr_t nameAddr = 0;

    while (std::getline(maps, line)) {
        if (line.find("libminecraftpe.so") == std::string::npos) continue;
        uintptr_t s, e;
        if (sscanf(line.c_str(), "%lx-%lx", &s, &e) != 2) continue;
        for (uintptr_t a = s; a + len < e; a++)
            if (!memcmp((void*)a, name, len)) { nameAddr = a; break; }
        if (nameAddr) break;
    }
    if (!nameAddr) return 0;

    std::ifstream maps2("/proc/self/maps");
    uintptr_t tiAddr = 0;
    while (std::getline(maps2, line)) {
        if (line.find("libminecraftpe.so") == std::string::npos) continue;
        uintptr_t s, e;
        if (sscanf(line.c_str(), "%lx-%lx", &s, &e) != 2) continue;
        for (uintptr_t a = s; a + 8 < e; a += 8)
            if (*(uintptr_t*)a == nameAddr) { tiAddr = a - 8; break; }
        if (tiAddr) break;
    }
    if (!tiAddr) return 0;

    std::ifstream maps3("/proc/self/maps");
    uintptr_t vt = 0;
    while (std::getline(maps3, line)) {
        if (line.find("libminecraftpe.so") == std::string::npos) continue;
        uintptr_t s, e;
        if (sscanf(line.c_str(), "%lx-%lx", &s, &e) != 2) continue;
        for (uintptr_t a = s; a + 8 < e; a += 8)
            if (*(uintptr_t*)a == tiAddr) { vt = a + 8; break; }
        if (vt) break;
    }
    return vt;
}

static bool patchSlot(uintptr_t vt, int slot, void* hook, void** orig) {
    if (!vt) return false;
    uintptr_t* p = (uintptr_t*)(vt + slot * 8);
    *orig = (void*)(*p);
    uintptr_t pg = (uintptr_t)p & ~4095UL;
    if (mprotect((void*)pg, 4096, PROT_READ | PROT_WRITE)) return false;
    *p = (uintptr_t)hook;
    mprotect((void*)pg, 4096, PROT_READ);
    return true;
}

// ── UI меню ───────────────────────────────────────────────────────────────────
static void drawMenu() {
    ImGuiIO& io = ImGui::GetIO();

    // Кнопка-триггер в левом верхнем углу
    ImGui::SetNextWindowPos(ImVec2(10.f, 10.f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(160.f, 60.f), ImGuiCond_Always);
    ImGui::Begin("FreeCamTrigger", nullptr,
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings);

    ImGui::SetWindowFontScale(1.4f);
    ImVec4 btnColor = g_freecam_active
        ? ImVec4(0.2f, 0.7f, 0.3f, 0.9f)
        : ImVec4(0.15f, 0.15f, 0.15f, 0.85f);
    ImGui::PushStyleColor(ImGuiCol_Button, btnColor);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(btnColor.x+0.1f, btnColor.y+0.1f, btnColor.z+0.1f, 1.f));

    if (ImGui::Button(g_freecam_active ? "FreeCam ON" : "FreeCam", ImVec2(140.f, 45.f))) {
        g_freecam_active = !g_freecam_active;
        LOGI("FreeCam %s", g_freecam_active ? "ON" : "OFF");
    }
    ImGui::PopStyleColor(2);
    ImGui::End();

    // Панель управления при активном FreeCam
    if (!g_freecam_active) return;

    // Координаты камеры
    ImGui::SetNextWindowPos(ImVec2(10.f, 80.f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.7f);
    ImGui::Begin("FreeCamInfo", nullptr,
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings);
    ImGui::SetWindowFontScale(1.1f);
    ImGui::Text("X: %.1f", g_camX);
    ImGui::Text("Y: %.1f", g_camY);
    ImGui::Text("Z: %.1f", g_camZ);
    ImGui::SliderFloat("Speed", &g_speed, 0.05f, 2.0f, "%.2f");
    ImGui::End();

    // Джойстик движения (левый нижний угол)
    float joyR  = 70.f;
    float joyX  = joyR + 20.f;
    float joyY  = io.DisplaySize.y - joyR - 20.f;

    ImGui::SetNextWindowPos(ImVec2(joyX - joyR, joyY - joyR), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(joyR*2, joyR*2), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::Begin("JoyBG", nullptr,
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoInputs);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddCircle(ImVec2(joyX, joyY), joyR, IM_COL32(255,255,255,60), 32, 2.f);
    dl->AddCircle(ImVec2(joyX, joyY), 20.f, IM_COL32(255,255,255,120), 32, 3.f);

    // Стрелки WASD
    ImU32 arrCol = IM_COL32(255,255,255,180);
    // W (вперёд)
    dl->AddTriangleFilled(
        ImVec2(joyX, joyY - joyR + 8),
        ImVec2(joyX - 12, joyY - joyR + 28),
        ImVec2(joyX + 12, joyY - joyR + 28), arrCol);
    // S (назад)
    dl->AddTriangleFilled(
        ImVec2(joyX, joyY + joyR - 8),
        ImVec2(joyX - 12, joyY + joyR - 28),
        ImVec2(joyX + 12, joyY + joyR - 28), arrCol);
    // A (влево)
    dl->AddTriangleFilled(
        ImVec2(joyX - joyR + 8, joyY),
        ImVec2(joyX - joyR + 28, joyY - 12),
        ImVec2(joyX - joyR + 28, joyY + 12), arrCol);
    // D (вправо)
    dl->AddTriangleFilled(
        ImVec2(joyX + joyR - 8, joyY),
        ImVec2(joyX + joyR - 28, joyY - 12),
        ImVec2(joyX + joyR - 28, joyY + 12), arrCol);
    ImGui::End();

    // Кнопки вверх/вниз (правый нижний угол)
    float btnSize = 70.f;
    float btnRX = io.DisplaySize.x - btnSize - 20.f;
    float btnRY = io.DisplaySize.y - btnSize * 2 - 30.f;

    ImGui::SetNextWindowPos(ImVec2(btnRX, btnRY), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::Begin("UpDown", nullptr,
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f,0.15f,0.15f,0.75f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f,0.3f,0.3f,0.9f));
    ImGui::SetWindowFontScale(2.0f);
    g_moveU = 0.f;
    if (ImGui::Button("^", ImVec2(btnSize, btnSize))) g_moveU =  1.f;
    if (ImGui::Button("v", ImVec2(btnSize, btnSize))) g_moveU = -1.f;
    ImGui::PopStyleColor(2);
    ImGui::End();
}

// ── тик движения ─────────────────────────────────────────────────────────────
static void tickMovement() {
    if (!g_freecam_active) return;
    if (g_moveF == 0.f && g_moveR == 0.f && g_moveU == 0.f) return;

    float yr = g_camYaw * (float)M_PI / 180.f;
    float sy = sinf(yr), cy = cosf(yr);

    g_camX += (-g_moveF * sy + g_moveR * cy) * g_speed;
    g_camZ += ( g_moveF * cy + g_moveR * sy) * g_speed;
    g_camY += g_moveU * g_speed;
}

// ── EGL хук ───────────────────────────────────────────────────────────────────
static void setup() {
    if (g_initialized || g_width <= 0) return;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.FontGlobalScale = 1.4f;
    ImGui_ImplAndroid_Init();
    ImGui_ImplOpenGL3_Init("#version 300 es");
    g_initialized = true;
    LOGI("ImGui initialized");
}

static void render() {
    if (!g_initialized) return;

    // Сохраняем GL состояние
    GLint last_prog; glGetIntegerv(GL_CURRENT_PROGRAM, &last_prog);
    GLint last_fbo;  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &last_fbo);
    GLint last_vp[4]; glGetIntegerv(GL_VIEWPORT, last_vp);
    GLboolean last_blend = glIsEnabled(GL_BLEND);
    GLboolean last_depth = glIsEnabled(GL_DEPTH_TEST);
    GLboolean last_scissor = glIsEnabled(GL_SCISSOR_TEST);

    tickMovement();

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)g_width, (float)g_height);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame(g_width, g_height);
    ImGui::NewFrame();

    drawMenu();

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    // Восстанавливаем GL состояние
    glUseProgram(last_prog);
    glBindFramebuffer(GL_FRAMEBUFFER, last_fbo);
    glViewport(last_vp[0], last_vp[1], last_vp[2], last_vp[3]);
    if (last_blend)   glEnable(GL_BLEND);   else glDisable(GL_BLEND);
    if (last_depth)   glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (last_scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
}

static EGLBoolean hook_eglswapbuffers(EGLDisplay dpy, EGLSurface surf) {
    if (!orig_eglswapbuffers) return EGL_FALSE;
    EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT ||
       (g_targetcontext != EGL_NO_CONTEXT && (ctx != g_targetcontext || surf != g_targetsurface)))
        return orig_eglswapbuffers(dpy, surf);

    EGLint w, h;
    eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    if (w < 100 || h < 100) return orig_eglswapbuffers(dpy, surf);

    if (g_targetcontext == EGL_NO_CONTEXT) {
        g_targetcontext = ctx;
        g_targetsurface = surf;
    }
    g_width = w; g_height = h;

    setup();
    render();

    return orig_eglswapbuffers(dpy, surf);
}

// ── тач колбэк ───────────────────────────────────────────────────────────────
typedef bool (*PreloaderInput_OnTouch_Fn)(int action, int pointerId, float x, float y);
struct PreloaderInput_Interface {
    void (*RegisterTouchCallback)(PreloaderInput_OnTouch_Fn callback);
};
typedef PreloaderInput_Interface* (*GetPreloaderInput_Fn)();

// Состояние джойстика
static bool  g_joyActive    = false;
static float g_joyStartX    = 0.f, g_joyStartY = 0.f;
static int   g_joyPointerId = -1;
static bool  g_rotActive    = false;
static float g_rotLastX     = 0.f, g_rotLastY = 0.f;
static int   g_rotPointerId = -1;

static bool OnTouchCallback(int action, int pointerId, float x, float y) {
    if (!g_initialized) return false;

    ImGuiIO& io = ImGui::GetIO();

    // Обновляем ImGui курсор только для первого пальца
    if (pointerId == 0) {
        io.AddMousePosEvent(x, y);
        if (action == AMOTION_EVENT_ACTION_DOWN)
            io.AddMouseButtonEvent(0, true);
        else if (action == AMOTION_EVENT_ACTION_UP || action == AMOTION_EVENT_ACTION_CANCEL)
            io.AddMouseButtonEvent(0, false);
    }

    // Если ImGui захватил ввод — не обрабатываем движение
    if (io.WantCaptureMouse) return true;

    if (!g_freecam_active) return false;

    float joyR = 70.f;
    float joyCX = joyR + 20.f;
    float joyCY = (g_height > 0 ? g_height : 2400) - joyR - 20.f;

    auto inJoy = [&](float px, float py) {
        float dx = px - joyCX, dy = py - joyCY;
        return (dx*dx + dy*dy) <= joyR*joyR;
    };

    if (action == AMOTION_EVENT_ACTION_DOWN || action == AMOTION_EVENT_ACTION_POINTER_DOWN) {
        if (inJoy(x, y) && g_joyPointerId < 0) {
            g_joyActive    = true;
            g_joyPointerId = pointerId;
            g_joyStartX    = x;
            g_joyStartY    = y;
            g_moveF = g_moveR = 0.f;
            return true;
        }
        if (!inJoy(x, y) && g_rotPointerId < 0) {
            g_rotActive    = true;
            g_rotPointerId = pointerId;
            g_rotLastX     = x;
            g_rotLastY     = y;
            return false;
        }
    }

    if (action == AMOTION_EVENT_ACTION_MOVE) {
        if (g_joyActive && pointerId == g_joyPointerId) {
            float dx = (x - g_joyStartX) / joyR;
            float dy = (y - g_joyStartY) / joyR;
            if (dx > 1.f) dx = 1.f; if (dx < -1.f) dx = -1.f;
            if (dy > 1.f) dy = 1.f; if (dy < -1.f) dy = -1.f;
            g_moveR =  dx;
            g_moveF = -dy; // вверх на экране = вперёд
            return true;
        }
        if (g_rotActive && pointerId == g_rotPointerId) {
            g_camYaw   += (x - g_rotLastX) * 0.2f;
            g_camPitch += (y - g_rotLastY) * 0.2f;
            if (g_camPitch >  90.f) g_camPitch =  90.f;
            if (g_camPitch < -90.f) g_camPitch = -90.f;
            g_rotLastX = x;
            g_rotLastY = y;
            return false;
        }
    }

    if (action == AMOTION_EVENT_ACTION_UP ||
        action == AMOTION_EVENT_ACTION_POINTER_UP ||
        action == AMOTION_EVENT_ACTION_CANCEL) {
        if (pointerId == g_joyPointerId) {
            g_joyActive    = false;
            g_joyPointerId = -1;
            g_moveF = g_moveR = 0.f;
        }
        if (pointerId == g_rotPointerId) {
            g_rotActive    = false;
            g_rotPointerId = -1;
        }
    }

    return false;
}

// ── инициализация (в отдельном потоке как в MotionBlur) ───────────────────────
static void* mainthread(void*) {
    sleep(3); // ждём пока лаунчер загрузит всё

    GlossInit(true);

    // Хук eglSwapBuffers для ImGui и тика движения
    GHandle hegl = GlossOpen("libEGL.so");
    if (hegl) {
        void* swap = (void*)GlossSymbol(hegl, "eglSwapBuffers", nullptr);
        if (swap) {
            GlossHook(swap, (void*)hook_eglswapbuffers, (void**)&orig_eglswapbuffers);
            LOGI("eglSwapBuffers hooked");
        }
    }

    // Хуки камеры
    uintptr_t vt = findVtable("16VanillaCameraAPI");
    if (vt) {
        patchSlot(vt, 7, (void*)hook_getPerspective, (void**)&g_orig_getPerspective);
        patchSlot(vt, 8, (void*)hook_getCamPos,      (void**)&g_orig_getCamPos);
        patchSlot(vt, 9, (void*)hook_getCamRot,      (void**)&g_orig_getCamRot);
        LOGI("Camera hooked");
    } else {
        LOGE("VanillaCameraAPI not found");
    }

    // Регистрация тач-колбэка через libpreloader.so
    void* preloaderLib = dlopen("libpreloader.so", RTLD_NOW);
    if (preloaderLib) {
        GetPreloaderInput_Fn GetInput =
            (GetPreloaderInput_Fn)dlsym(preloaderLib, "GetPreloaderInput");
        if (GetInput) {
            PreloaderInput_Interface* input = GetInput();
            if (input && input->RegisterTouchCallback) {
                input->RegisterTouchCallback(OnTouchCallback);
                LOGI("Touch callback registered");
            }
        } else {
            LOGE("GetPreloaderInput not found in libpreloader.so");
        }
    } else {
        LOGE("libpreloader.so not found");
    }

    LOGI("FreeCam ready!");
    return nullptr;
}

__attribute__((constructor))
void freecam_init() {
    pthread_t t;
    pthread_create(&t, nullptr, mainthread, nullptr);
}
