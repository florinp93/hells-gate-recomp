package org.libsdl.app;

/** Public access to SDL's package-private Android joystick JNI entry points (touch controller). */
public final class TouchBridge {
    private TouchBridge() {}

    public static void addJoystick(int deviceId, String name, String desc, int vendor, int product,
                                   int buttonMask, int naxes, int axisMask) {
        SDLControllerManager.nativeAddJoystick(deviceId, name, desc, vendor, product, buttonMask,
                naxes, axisMask, 0, false, false);
    }

    public static void removeJoystick(int deviceId) {
        SDLControllerManager.nativeRemoveJoystick(deviceId);
    }

    public static void axis(int deviceId, int axis, float value) {
        SDLControllerManager.onNativeJoy(deviceId, axis, value);
    }

    public static void buttonDown(int deviceId, int keycode) {
        SDLControllerManager.onNativePadDown(deviceId, keycode);
    }

    public static void buttonUp(int deviceId, int keycode) {
        SDLControllerManager.onNativePadUp(deviceId, keycode);
    }
}
