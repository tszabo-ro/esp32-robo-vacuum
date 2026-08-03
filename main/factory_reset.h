#pragma once

#include <cstdint>
#include "hal/gpio_types.h"

// Wipes the stored configuration when a pin is held to ground.
//
// This is the last line of recovery for a device with no serial access: it
// clears the WiFi credentials, MQTT broker and access point password, then
// restarts, which brings the setup access point back up for re-provisioning.
class FactoryReset {
public:
    // Configures the pin and starts watching it.
    void start();

    // True while the pin is held down but the hold is not yet long enough to
    // act. The status LED uses this to show that the hold has registered.
    static bool pending();

    // Held to ground to trigger. Driven by an internal pull-up, so leaving it
    // unconnected is safe. Deliberately not GPIO2, GPIO8 or GPIO9: those are
    // strapping pins, where a wire left attached could change the boot mode.
    static constexpr gpio_num_t PIN = GPIO_NUM_10;

    // Long enough that a momentary short, or a floating wire picking up noise,
    // cannot wipe the device by accident.
    static constexpr uint32_t HOLD_MS = 5000;

private:
    static void watch_task(void* arg);
    static void wipe_and_restart();

    static constexpr uint32_t POLL_MS = 100;
};
