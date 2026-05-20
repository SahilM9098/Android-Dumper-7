#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <dlfcn.h>
#include <jni.h>
#include <pthread.h>
#include <unistd.h>

#include <GLES3/gl3.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include "casncadia_mono.h"
#include "fa_solid_900.h"
#include "IconsFontAwesome7.h"
#include "dex_payload.h"
#include "imgui.h"
#include "imgui_impl_android.h"
#include "imgui_impl_opengl3.h"
#include "imgui_internal.h"
#include "Utility.h"
#include "MainPanel.h"

namespace {

constexpr int kMaxTouchWindows = 20;
constexpr const char *kLogTag = "ImguiRenderer";

struct WindowRectSnapshot {
  bool active;
  int id;
  float x;
  float y;
  float w;
  float h;
};

struct TouchEvent {
  bool down;
  float x;
  float y;
};

bool g_Init = false;
bool g_MenuVisible = true;
ANativeWindow *g_Window = nullptr;
int32_t g_ScreenWidth = 0;
int32_t g_ScreenHeight = 0;
bool g_MouseDown = false;
pthread_mutex_t g_ImGuiMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t g_TouchMutex = PTHREAD_MUTEX_INITIALIZER;
TouchEvent g_TouchEvents[128] = {};
int g_TouchEventCount = 0;
int g_NativeTouchLogCount = 0;
int g_ConsumedTouchLogCount = 0;
WindowRectSnapshot g_WindowRectCache[kMaxTouchWindows] = {};

using JNI_GetCreatedJavaVMs_t = jint (*)(JavaVM **, jsize, jsize *);
using AndroidRuntimeGetJavaVM_t = JavaVM *(*)();

void LogError(const char *message) {
  __android_log_print(ANDROID_LOG_ERROR, kLogTag, "%s", message);
}

bool HasImGuiContext() {
  return g_Init && ImGui::GetCurrentContext() != nullptr;
}

void ClearWindowRectCache() {
  std::memset(g_WindowRectCache, 0, sizeof(g_WindowRectCache));
}

void QueueTouchEvent(bool down, float x, float y) {
  pthread_mutex_lock(&g_TouchMutex);
  if (g_TouchEventCount >= static_cast<int>(sizeof(g_TouchEvents) / sizeof(g_TouchEvents[0]))) {
    std::memmove(g_TouchEvents, g_TouchEvents + 1,
                 sizeof(TouchEvent) * (sizeof(g_TouchEvents) / sizeof(g_TouchEvents[0]) - 1));
    g_TouchEventCount =
        static_cast<int>(sizeof(g_TouchEvents) / sizeof(g_TouchEvents[0])) - 1;
  }
  g_TouchEvents[g_TouchEventCount++] = TouchEvent{down, x, y};
  pthread_mutex_unlock(&g_TouchMutex);
}

int TakeTouchEvents(TouchEvent *outEvents, int maxEvents) {
  pthread_mutex_lock(&g_TouchMutex);
  const int count = ImMin(g_TouchEventCount, maxEvents);
  if (count > 0) {
    std::memcpy(outEvents, g_TouchEvents, sizeof(TouchEvent) * count);
    const int remaining = g_TouchEventCount - count;
    if (remaining > 0) {
      std::memmove(g_TouchEvents, g_TouchEvents + count, sizeof(TouchEvent) * remaining);
    }
    g_TouchEventCount = remaining;
  }
  pthread_mutex_unlock(&g_TouchMutex);
  return count;
}

void ApplyTouchEventToImGui(bool down, float x, float y) {
  ImGuiIO &io = ImGui::GetIO();
  io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
  io.AddMousePosEvent(x, y);
  io.MousePos = ImVec2(x, y);
  if (g_MouseDown != down) {
    io.AddMouseButtonEvent(0, down);
    g_MouseDown = down;
  }
  io.MouseDown[0] = down;
}

void ApplyMenuStyle(float density) {
  ImGuiIO &io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.FontGlobalScale = density > 0.0f ? density * 0.55f : 1.0f;

  ImGui::StyleColorsDark();
  ImGuiStyle &style = ImGui::GetStyle();
  style.WindowRounding = 8.0f;
  style.ChildRounding = 8.0f;
  style.FrameRounding = 7.0f;
  style.PopupRounding = 8.0f;
  style.ScrollbarRounding = 8.0f;
  style.GrabRounding = 8.0f;
  style.WindowBorderSize = 1.0f;
  style.FrameBorderSize = 1.0f;
  style.ItemSpacing = ImVec2(10.0f, 8.0f);
  style.FramePadding = ImVec2(12.0f, 8.0f);
  style.WindowPadding = ImVec2(14.0f, 14.0f);
  style.TouchExtraPadding = ImVec2(6.0f, 6.0f);
  style.ScrollbarSize = 18.0f;
  style.GrabMinSize = 22.0f;
}

void UpdateWindowRectCache() {
  ClearWindowRectCache();
  if (!HasImGuiContext()) {
    return;
  }

  ImGuiContext *context = ImGui::GetCurrentContext();
  if (context == nullptr) {
    return;
  }

  int outIndex = 0;
  for (int index = 0; index < context->Windows.Size && outIndex < kMaxTouchWindows;
       ++index) {
    ImGuiWindow *window = context->Windows[index];
    if (window == nullptr || !window->WasActive || window->RootWindow != window) {
      continue;
    }

    const ImVec2 pos = window->Pos;
    const ImVec2 size = window->Size;
    const float maxX =
        (g_ScreenWidth > 0) ? ImMax(0.0f, static_cast<float>(g_ScreenWidth) - size.x) : pos.x;
    const float maxY =
        (g_ScreenHeight > 0) ? ImMax(0.0f, static_cast<float>(g_ScreenHeight) - size.y) : pos.y;

    WindowRectSnapshot &snapshot = g_WindowRectCache[outIndex++];
    snapshot.active = true;
    snapshot.id = static_cast<int>(window->ID);
    snapshot.x = ImClamp(pos.x, 0.0f, maxX);
    snapshot.y = ImClamp(pos.y, 0.0f, maxY);
    snapshot.w = size.x;
    snapshot.h = size.y;
  }
}


JavaVM *TryResolveVmFromHandle(void *handle) {
  if (handle == nullptr) {
    return nullptr;
  }

  auto getVms = reinterpret_cast<JNI_GetCreatedJavaVMs_t>(
      dlsym(handle, "JNI_GetCreatedJavaVMs"));
  if (getVms != nullptr) {
    JavaVM *vm = nullptr;
    jsize count = 0;
    if (getVms(&vm, 1, &count) == JNI_OK && count > 0 && vm != nullptr) {
      return vm;
    }
  }

  auto getVm = reinterpret_cast<AndroidRuntimeGetJavaVM_t>(
      dlsym(handle, "AndroidRuntimeGetJavaVM"));
  if (getVm != nullptr) {
    return getVm();
  }

  return nullptr;
}

JavaVM *ResolveJavaVm() {
  if (JavaVM *vm = TryResolveVmFromHandle(RTLD_DEFAULT)) {
    return vm;
  }

  const char *libraries[] = {
      "libandroid_runtime.so",
      "/system/lib64/libandroid_runtime.so",
      "libnativehelper.so",
      "/apex/com.android.art/lib64/libnativehelper.so",
      "libart.so",
      "/apex/com.android.art/lib64/libart.so",
  };

  for (const char *library : libraries) {
    void *handle = dlopen(library, RTLD_NOW | RTLD_GLOBAL);
    if (JavaVM *vm = TryResolveVmFromHandle(handle)) {
      return vm;
    }
  }

  return nullptr;
}

jclass LoadClass(JNIEnv *env, jobject classLoader, const char *name) {
  jclass classLoaderClass = env->FindClass("java/lang/ClassLoader");
  jmethodID loadClass = env->GetMethodID(
      classLoaderClass, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
  jstring className = env->NewStringUTF(name);
  jclass result = static_cast<jclass>(env->CallObjectMethod(classLoader, loadClass, className));
  env->DeleteLocalRef(className);
  return result;
}

}  // namespace

static void jni_initImgui(JNIEnv *env, jclass clazz, jobject surface, jfloat density) {
  (void)clazz;

  pthread_mutex_lock(&g_ImGuiMutex);
  if (g_Init) {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplAndroid_Shutdown();
    if (g_Window != nullptr) {
      ANativeWindow_release(g_Window);
      g_Window = nullptr;
    }
  } else {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    g_Init = true;
  }

  g_Window = ANativeWindow_fromSurface(env, surface);
  ImGui_ImplAndroid_Init(g_Window);
  ImGui_ImplOpenGL3_Init("#version 300 es");

    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig fontCfg;
    fontCfg.OversampleH = 2;
    fontCfg.OversampleV = 1;
    fontCfg.PixelSnapH  = true;

    io.Fonts->AddFontFromMemoryCompressedBase85TTF(
            CascadiaMono_compressed_data_base85, 25.0f,
            &fontCfg, io.Fonts->GetGlyphRangesDefault());

    static const ImWchar iconRanges[] = {ICON_MIN_FA, ICON_MAX_FA, 0};
    ImFontConfig iconCfg;
    iconCfg.MergeMode        = true;
    iconCfg.PixelSnapH       = true;
    iconCfg.GlyphMinAdvanceX = 18.0f;

    io.Fonts->AddFontFromMemoryTTF(
            (void*)fa_solid_900, sizeof(fa_solid_900),
            18.0f, &iconCfg, iconRanges);

  ApplyMenuStyle(density);
  pthread_mutex_unlock(&g_ImGuiMutex);
}

static void jni_updateSize(JNIEnv *env, jclass clazz, jint width, jint height) {
  (void)env;
  (void)clazz;
  if (!HasImGuiContext()) {
    return;
  }

  pthread_mutex_lock(&g_ImGuiMutex);
  g_ScreenWidth = width;
  g_ScreenHeight = height;
  glViewport(0, 0, width, height);
  ImGui::GetIO().DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
  pthread_mutex_unlock(&g_ImGuiMutex);
}

static void jni_Tick(JNIEnv *env, jclass clazz) {
  (void)env;
  (void)clazz;
  if (!HasImGuiContext()) {
    return;
  }

  pthread_mutex_lock(&g_ImGuiMutex);
  ImGui_ImplOpenGL3_NewFrame();
  ImGui_ImplAndroid_NewFrame();

  TouchEvent touchEvents[128];
  const int touchEventCount =
      TakeTouchEvents(touchEvents, static_cast<int>(sizeof(touchEvents) / sizeof(touchEvents[0])));
  if (touchEventCount > 0) {
    ImGuiIO &io = ImGui::GetIO();
    for (int index = 0; index < touchEventCount; ++index) {
      const TouchEvent &event = touchEvents[index];
      ApplyTouchEventToImGui(event.down, event.x, event.y);
      if (g_ConsumedTouchLogCount < 24) {
        __android_log_print(ANDROID_LOG_INFO, kLogTag,
                            "consume touch down=%d x=%.1f y=%.1f display=%.1fx%.1f",
                            event.down ? 1 : 0,
                            event.x,
                            event.y,
                            io.DisplaySize.x,
                            io.DisplaySize.y);
        ++g_ConsumedTouchLogCount;
      }
    }
  }

  ImGui::NewFrame();
  if (g_MenuVisible) {
    MainPanel::Render();
  }
  UpdateWindowRectCache();

  ImGui::Render();
  glViewport(0, 0, g_ScreenWidth, g_ScreenHeight);
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
  pthread_mutex_unlock(&g_ImGuiMutex);
}

static void jni_imguiShutdown(JNIEnv *env, jclass clazz) {
  (void)env;
  (void)clazz;
  if (!g_Init) {
    return;
  }

  pthread_mutex_lock(&g_ImGuiMutex);
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplAndroid_Shutdown();
  ImGui::DestroyContext();

  if (g_Window != nullptr) {
    ANativeWindow_release(g_Window);
    g_Window = nullptr;
  }

  g_Init = false;
  g_ScreenWidth = 0;
  g_ScreenHeight = 0;
  g_MouseDown = false;
  pthread_mutex_lock(&g_TouchMutex);
  g_TouchEventCount = 0;
  pthread_mutex_unlock(&g_TouchMutex);
  g_NativeTouchLogCount = 0;
  g_ConsumedTouchLogCount = 0;
  ClearWindowRectCache();
  pthread_mutex_unlock(&g_ImGuiMutex);
}

static void jni_motionEventClick(
    JNIEnv *env, jclass clazz, jboolean down, jfloat posX, jfloat posY) {
  (void)env;
  (void)clazz;
  if (!HasImGuiContext()) {
    if (g_NativeTouchLogCount < 8) {
      __android_log_print(ANDROID_LOG_WARN, kLogTag,
                          "drop touch before imgui context down=%d x=%.1f y=%.1f",
                          down == JNI_TRUE ? 1 : 0,
                          posX,
                          posY);
      ++g_NativeTouchLogCount;
    }
    return;
  }

  const bool touchDown = down == JNI_TRUE;
  pthread_mutex_lock(&g_ImGuiMutex);
  ApplyTouchEventToImGui(touchDown, posX, posY);
  pthread_mutex_unlock(&g_ImGuiMutex);
  if (g_NativeTouchLogCount < 24) {
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "apply touch down=%d x=%.1f y=%.1f",
                        touchDown ? 1 : 0,
                        posX,
                        posY);
    ++g_NativeTouchLogCount;
  }
}

static jobjectArray jni_getWindowRect(JNIEnv *env, jclass clazz) {
  (void)clazz;
  jclass stringClass = env->FindClass("java/lang/String");
  jstring emptyString = env->NewStringUTF("");
  jobjectArray results = env->NewObjectArray(kMaxTouchWindows, stringClass, emptyString);
  env->DeleteLocalRef(emptyString);

  char buffer[128];
  for (int index = 0; index < kMaxTouchWindows; ++index) {
    const WindowRectSnapshot &snapshot = g_WindowRectCache[index];
    if (!snapshot.active) {
      jstring emptyRect = env->NewStringUTF("1000|0|0|0|0");
      env->SetObjectArrayElement(results, index, emptyRect);
      env->DeleteLocalRef(emptyRect);
      continue;
    }

    std::snprintf(buffer, sizeof(buffer), "%d|%.4f|%.4f|%.4f|%.4f",
                  snapshot.id, snapshot.x, snapshot.y, snapshot.w, snapshot.h);
    jstring rect = env->NewStringUTF(buffer);
    env->SetObjectArrayElement(results, index, rect);
    env->DeleteLocalRef(rect);
  }

  return results;
}

static jboolean jni_nativeIsMenuVisible(JNIEnv *env, jclass clazz) {
  (void)env;
  (void)clazz;
  return g_MenuVisible ? JNI_TRUE : JNI_FALSE;
}

static void *inject_thread(void *) {
  JavaVM *vm = ResolveJavaVm();
  if (vm == nullptr) {
    LogError("Failed to resolve JavaVM");
    return nullptr;
  }

  JNIEnv *env = nullptr;
  bool attached = false;
  if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK) {
    if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
      LogError("Failed to attach JNI thread");
      return nullptr;
    }
    attached = true;
  }

  jobject byteBuffer = env->NewDirectByteBuffer(
      const_cast<unsigned char *>(payload_dex), payload_dex_size);
  jclass classLoaderClass = env->FindClass("java/lang/ClassLoader");
  jmethodID getSystemClassLoader = env->GetStaticMethodID(
      classLoaderClass, "getSystemClassLoader", "()Ljava/lang/ClassLoader;");
  jobject systemClassLoader = env->CallStaticObjectMethod(
      classLoaderClass, getSystemClassLoader);

  jclass inMemoryDexClass = env->FindClass("dalvik/system/InMemoryDexClassLoader");
  jmethodID ctor = env->GetMethodID(
      inMemoryDexClass, "<init>", "(Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V");
  jobject dexClassLoader = env->NewObject(
      inMemoryDexClass, ctor, byteBuffer, systemClassLoader);

  JNINativeMethod glesMethods[] = {
      {"nativeIsMenuVisible", "()Z", (void *)jni_nativeIsMenuVisible},
      {"initImgui", "(Landroid/view/Surface;F)V", (void *)jni_initImgui},
      {"updateSize", "(II)V", (void *)jni_updateSize},
      {"Tick", "()V", (void *)jni_Tick},
      {"imguiShutdown", "()V", (void *)jni_imguiShutdown},
      {"motionEventClick", "(ZFF)V", (void *)jni_motionEventClick},
      {"getWindowRect", "()[Ljava/lang/String;", (void *)jni_getWindowRect},
  };

  jclass glesClass = LoadClass(
      env, dexClassLoader, "com.sahilm9098.arkmodmenu.GLES3JNIView");
  if (glesClass == nullptr || env->RegisterNatives(
          glesClass, glesMethods, sizeof(glesMethods) / sizeof(glesMethods[0])) != JNI_OK) {
    LogError("Failed to register GLES3JNIView natives");
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
    }
    if (attached) {
      vm->DetachCurrentThread();
    }
    return nullptr;
  }

  jclass loaderClass = LoadClass(
      env, dexClassLoader, "com.sahilm9098.arkmodmenu.Loader");
  jmethodID mainMethod = loaderClass != nullptr
      ? env->GetStaticMethodID(loaderClass, "main", "()V")
      : nullptr;
  if (mainMethod == nullptr) {
    LogError("Failed to resolve Loader.main");
  } else {
    env->CallStaticVoidMethod(loaderClass, mainMethod);
    if (env->ExceptionCheck()) {
      env->ExceptionDescribe();
      env->ExceptionClear();
    }
  }

  if (attached) {
    vm->DetachCurrentThread();
  }
  return nullptr;
}

__attribute__((constructor)) static void payload_main() {
  pthread_t thread;
  if (pthread_create(&thread, nullptr, inject_thread, nullptr) == 0) {
    pthread_detach(thread);
  }
}
