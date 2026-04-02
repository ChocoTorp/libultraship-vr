#if defined(__ANDROID__) || defined(__IOS__)
#include "ship/port/mobile/MobileImpl.h"
#include <SDL2/SDL.h>

#include <imgui_internal.h>

static bool isShowingVirtualKeyboard = true;

void Ship::Mobile::ImGuiProcessEvent(bool wantsTextInput) {
    ImGuiInputTextState* state = ImGui::GetInputTextState(ImGui::GetActiveID());

    if (wantsTextInput) {
        if (!isShowingVirtualKeyboard) {
            state->ClearText();

            isShowingVirtualKeyboard = true;
            SDL_StartTextInput();
        }
    } else {
        if (isShowingVirtualKeyboard) {
            isShowingVirtualKeyboard = false;
            SDL_StopTextInput();
        }
    }
}
#endif

#ifdef __ANDROID__
#include <SDL_gamecontroller.h>
#include <jni.h>
#include <atomic>

static bool isUsingTouchscreenControls = false;
static std::atomic<bool> sGamepadBackPressed{false};

extern "C" void JNICALL Java_com_dishii_soh_MainActivity_nativeGamepadBackPressed(JNIEnv* env, jclass clazz) {
    sGamepadBackPressed = true;
}

bool Ship::Mobile::ConsumeGamepadBackPress() {
    return sGamepadBackPressed.exchange(false);
}
static int virtual_joystick_id = -1;
static SDL_Joystick* virtual_joystick = nullptr;

extern "C" void JNICALL Java_com_dishii_soh_MainActivity_attachController(JNIEnv* env, jobject obj) {
    virtual_joystick_id = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 6, 18, 0);
    if (virtual_joystick_id == -1) {
        SDL_Log("Could not create overlay virtual controller");
        return;
    }
    virtual_joystick = SDL_JoystickOpen(virtual_joystick_id);
    if (virtual_joystick == nullptr)
        SDL_Log("Could not create virtual joystick");
    isUsingTouchscreenControls = true;
}

extern "C" void JNICALL Java_com_dishii_soh_MainActivity_detachController(JNIEnv* env, jobject obj) {
    SDL_JoystickClose(virtual_joystick);
    SDL_JoystickDetachVirtual(virtual_joystick_id);
    virtual_joystick = nullptr;
    virtual_joystick_id = -1;
    isUsingTouchscreenControls = false;
}

extern "C" void JNICALL Java_com_dishii_soh_MainActivity_setButton(JNIEnv* env, jobject obj, jint button, jboolean value) {
    if (button < 0) {
        SDL_JoystickSetVirtualAxis(virtual_joystick, -button, value ? SDL_MAX_SINT16 : -SDL_MAX_SINT16);
    } else {
        SDL_JoystickSetVirtualButton(virtual_joystick, button, value);
    }
}

extern "C" void JNICALL Java_com_dishii_soh_MainActivity_setAxis(JNIEnv* env, jobject obj, jint axis, jshort value) {
    SDL_JoystickSetVirtualAxis(virtual_joystick, axis, value);
}

extern "C" void JNICALL Java_com_dishii_soh_MainActivity_setCameraState(JNIEnv* env, jobject obj, jint axis, jfloat value) {
}

void Ship::Mobile::SetToggleButtonVisible(bool visible) {
    JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();
    jobject javaObject = (jobject)SDL_AndroidGetActivity();
    jclass javaClass = env->GetObjectClass(javaObject);
    jmethodID method = env->GetMethodID(javaClass, "SetToggleButtonVisible", "(Z)V");
    env->CallVoidMethod(javaObject, method, (jboolean)visible);
}
#endif
