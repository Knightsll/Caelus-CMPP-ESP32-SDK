/* SPDX-License-Identifier: Apache-2.0 */
#include "cmpp/cmpp_backend_relay.h"

int cmpp_relay_physical_level(bool active_high, bool on)
{
    return on == active_high ? 1 : 0;
}

