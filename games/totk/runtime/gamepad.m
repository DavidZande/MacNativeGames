/* Native macOS controllers feed the same FullKey state as the keyboard. */
#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "runtime.h"

static pthread_mutex_t gamepadLock = PTHREAD_MUTEX_INITIALIZER;
static RuntimeGamepadState gamepadState;
static uint64_t pressedEdges;
static BOOL started;

/* Local change: the button layout and the stick deadzone, from the settings window (runtime_gamepad_config) or the
   environment (TOTK_PAD_LAYOUT=nintendo, TOTK_PAD_DEADZONE=0.05..0.4). Nintendo layout: buttons by position, as on a
   Switch controller (bottom = B, right = A, left = Y, top = X); the default follows the labels (Xbox A = Switch A). */
static _Atomic int padNintendo = -1, padDeadzoneMilli = -1;
static void pad_env(void) {
    if (atomic_load(&padNintendo) < 0) { const char *e = getenv("TOTK_PAD_LAYOUT"); atomic_store(&padNintendo, e && !strcmp(e, "nintendo")); }
    if (atomic_load(&padDeadzoneMilli) < 0) { const char *e = getenv("TOTK_PAD_DEADZONE"); double v = e ? atof(e) : 0.12; if (v < 0.02 || v > 0.4) v = 0.12; atomic_store(&padDeadzoneMilli, (int)(v * 1000)); }
}
void runtime_gamepad_config(int nintendo, double deadzone) {
    if (!getenv("TOTK_PAD_LAYOUT")) atomic_store(&padNintendo, nintendo != 0);
    if (!getenv("TOTK_PAD_DEADZONE") && deadzone >= 0.02 && deadzone <= 0.4) atomic_store(&padDeadzoneMilli, (int)(deadzone * 1000));
}
static int32_t stick_axis(float value) {
    pad_env(); const float deadzone = atomic_load(&padDeadzoneMilli) / 1000.0f;
    float magnitude = fabsf(value);
    if (magnitude <= deadzone) return 0;
    if (magnitude > 1.0f) magnitude = 1.0f;
    int32_t result = (int32_t)((magnitude - deadzone) / (1.0f - deadzone) * 32767.0f);
    return value < 0 ? -result : result;
}

static int32_t stronger_axis(int32_t a, int32_t b) {
    return abs(b) > abs(a) ? b : a;
}

static uint64_t gamepad_buttons(GCExtendedGamepad *pad) {
    uint64_t buttons = 0; pad_env(); int n = atomic_load(&padNintendo) == 1;
    if (pad.buttonA.isPressed) buttons |= 1ULL << (n ? 1 : 0);   /* bottom */
    if (pad.buttonB.isPressed) buttons |= 1ULL << (n ? 0 : 1);   /* right */
    if (pad.buttonX.isPressed) buttons |= 1ULL << (n ? 3 : 2);   /* left */
    if (pad.buttonY.isPressed) buttons |= 1ULL << (n ? 2 : 3);   /* top */
    if (pad.leftThumbstickButton.isPressed) buttons |= 1ULL << 4;
    if (pad.rightThumbstickButton.isPressed) buttons |= 1ULL << 5;
    if (pad.leftShoulder.isPressed) buttons |= 1ULL << 6;
    if (pad.rightShoulder.isPressed) buttons |= 1ULL << 7;
    if (pad.leftTrigger.isPressed) buttons |= 1ULL << 8;
    if (pad.rightTrigger.isPressed) buttons |= 1ULL << 9;
    if (pad.buttonMenu.isPressed) buttons |= 1ULL << 10;
    if (pad.buttonOptions.isPressed) buttons |= 1ULL << 11;
    if (pad.dpad.left.isPressed) buttons |= 1ULL << 12;
    if (pad.dpad.up.isPressed) buttons |= 1ULL << 13;
    if (pad.dpad.right.isPressed) buttons |= 1ULL << 14;
    if (pad.dpad.down.isPressed) buttons |= 1ULL << 15;
    return buttons;
}

static void refresh_gamepads(void) {
    RuntimeGamepadState next = {0};
    for (GCController *controller in GCController.controllers) {
        GCExtendedGamepad *pad = controller.extendedGamepad;
        if (!pad) continue;
        next.connected = 1;
        next.buttons |= gamepad_buttons(pad);
        next.lx = stronger_axis(next.lx, stick_axis(pad.leftThumbstick.xAxis.value));
        next.ly = stronger_axis(next.ly, stick_axis(pad.leftThumbstick.yAxis.value));
        next.rx = stronger_axis(next.rx, stick_axis(pad.rightThumbstick.xAxis.value));
        next.ry = stronger_axis(next.ry, stick_axis(pad.rightThumbstick.yAxis.value));
    }
    pthread_mutex_lock(&gamepadLock);
    pressedEdges |= next.buttons & ~gamepadState.buttons;
    gamepadState = next;
    pthread_mutex_unlock(&gamepadLock);
}

static void attach_gamepad(GCController *controller) {
    GCExtendedGamepad *pad = controller.extendedGamepad;
    if (!pad) return;
    pad.valueChangedHandler = ^(GCExtendedGamepad *changed, GCControllerElement *element) {
        (void)changed; (void)element;
        refresh_gamepads();
    };
    fprintf(stderr, "[totk-input] native gamepad connected: %s (%s)\n",
            (controller.vendorName ?: @"unknown").UTF8String,
            (controller.productCategory ?: @"unknown").UTF8String);
    runtime_feedback_attach(controller);
    refresh_gamepads();
}

void runtime_gamepad_start(void) {
    if (started) return;
    started = YES;
    NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
    [center addObserverForName:GCControllerDidConnectNotification object:nil queue:NSOperationQueue.mainQueue
                   usingBlock:^(NSNotification *notification) { attach_gamepad(notification.object); }];
    [center addObserverForName:GCControllerDidDisconnectNotification object:nil queue:NSOperationQueue.mainQueue
                   usingBlock:^(NSNotification *notification) {
                       GCController *controller = notification.object;
                       fprintf(stderr, "[totk-input] native gamepad disconnected: %s\n",
                               (controller.vendorName ?: @"unknown").UTF8String);
                       runtime_feedback_detach(controller);
                       refresh_gamepads();
                   }];
    for (GCController *controller in GCController.controllers) attach_gamepad(controller);
    [GCController startWirelessControllerDiscoveryWithCompletionHandler:nil];
}

void runtime_gamepad_clear(void) {
    runtime_feedback_quiet();
    pthread_mutex_lock(&gamepadLock);
    gamepadState = (RuntimeGamepadState){0};
    pressedEdges = 0;
    pthread_mutex_unlock(&gamepadLock);
}

/* On-screen touch controls (iPhone, ios/app/Sources/touch_controls.m) feed a second state merged with the physical
   controllers: buttons are OR-ed (a tap shorter than one game frame is latched like a controller press), each stick
   axis takes the stronger value. */
static RuntimeGamepadState touchState;
void runtime_touch_pad_set(const RuntimeGamepadState *s) {
    pthread_mutex_lock(&gamepadLock);
    pressedEdges |= s->buttons & ~touchState.buttons;
    touchState = *s;
    pthread_mutex_unlock(&gamepadLock);
}

void runtime_gamepad_read(RuntimeGamepadState *out) {
    pthread_mutex_lock(&gamepadLock);
    *out = gamepadState;
    out->buttons |= touchState.buttons | pressedEdges;
    out->lx = stronger_axis(out->lx, touchState.lx); out->ly = stronger_axis(out->ly, touchState.ly);
    out->rx = stronger_axis(out->rx, touchState.rx); out->ry = stronger_axis(out->ry, touchState.ry);
    if (touchState.connected) out->connected = 1;
    pressedEdges = 0;
    pthread_mutex_unlock(&gamepadLock);
}
