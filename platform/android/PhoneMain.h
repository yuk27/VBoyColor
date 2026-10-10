#pragma once

struct android_app;
class AndroidPlatform;

// The phone and tablet app's main loop (see PhoneMain.cpp) - android_main
// runs it instead of the headset's when MainActivity.isHeadset() is false.
// Returns when the activity is destroyed.
void RunPhoneApp(android_app *app, AndroidPlatform &platform);
