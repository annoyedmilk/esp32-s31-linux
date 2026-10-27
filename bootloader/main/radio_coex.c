// SPDX-License-Identifier: BSD-2-Clause
// Author: Marco Müller <hello@annoyedmilk.ch>
//
// The Wi-Fi/802.15.4 coexistence scheme takes 70% of the Wi-Fi throughput,
// and without it Wi-Fi stops while the 802.15.4 radio receives.  Run the
// scheme only while the radio receives: the linker wraps the radio calls of
// OpenThread (see CMakeLists.txt).

#include <stdbool.h>
#include "esp_coex_i154.h"
#include "openthread/platform/radio.h"

static bool s_rx_on;

static void set_rx(bool on)
{
    if (s_rx_on == on) {
        return;
    }
    s_rx_on = on;
    if (on) {
        esp_coex_ieee802154_status_enable();
    } else {
        esp_coex_ieee802154_status_disable();
    }
}

otError __real_otPlatRadioReceive(otInstance *inst, uint8_t channel);
otError __real_otPlatRadioSleep(otInstance *inst);
otError __real_otPlatRadioDisable(otInstance *inst);

otError __wrap_otPlatRadioReceive(otInstance *inst, uint8_t channel)
{
    set_rx(true);
    return __real_otPlatRadioReceive(inst, channel);
}

otError __wrap_otPlatRadioSleep(otInstance *inst)
{
    set_rx(false);
    return __real_otPlatRadioSleep(inst);
}

otError __wrap_otPlatRadioDisable(otInstance *inst)
{
    set_rx(false);
    return __real_otPlatRadioDisable(inst);
}
