// SPDX-License-Identifier: BSD-2-Clause
/* ESP-Mosaico CoreBoard V1.2: Linux owns the display and camera. */

#include "hal/usb_serial_jtag_ll.h"
#include "loader.h"

void board_start_display(void)
{
    /* DVP D2 and the flash control need GPIO33/34, not USB Serial/JTAG. */
    usb_serial_jtag_ll_disable_intr_mask(USB_SERIAL_JTAG_LL_INTR_MASK);
    usb_serial_jtag_ll_phy_enable_pad(false);
}
