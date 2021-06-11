/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * This file is part of the TinyUSB stack.
 */

#include "tusb_option.h"

#if CFG_TUSB_MCU == OPT_MCU_RP2040

#include <stdlib.h>
#include "rp2040_usb.h"

// Direction strings for debug
const char *ep_dir_string[] = {
        "out",
        "in",
};

static inline void _hw_endpoint_lock_update(struct hw_endpoint *ep, int delta) {
    // todo add critsec as necessary to prevent issues between worker and IRQ...
    //  note that this is perhaps as simple as disabling IRQs because it would make
    //  sense to have worker and IRQ on same core, however I think using critsec is about equivalent.
}

#if TUSB_OPT_HOST_ENABLED
static inline void _hw_endpoint_update_last_buf(struct hw_endpoint *ep)
{
    ep->last_buf = (ep->len + ep->transfer_size == ep->total_len);
}
#endif

void rp2040_usb_init(void)
{
    // Reset usb controller
    reset_block(RESETS_RESET_USBCTRL_BITS);
    unreset_block_wait(RESETS_RESET_USBCTRL_BITS);

    // Clear any previous state just in case
    memset(usb_hw, 0, sizeof(*usb_hw));
    memset(usb_dpram, 0, sizeof(*usb_dpram));

    // Mux the controller to the onboard usb phy
    usb_hw->muxing = USB_USB_MUXING_TO_PHY_BITS    | USB_USB_MUXING_SOFTCON_BITS;

    // Force VBUS detect so the device thinks it is plugged into a host
    // TODO support VBUs detect
    usb_hw->pwr    = USB_USB_PWR_VBUS_DETECT_BITS  | USB_USB_PWR_VBUS_DETECT_OVERRIDE_EN_BITS;
}

void hw_endpoint_reset_transfer(struct hw_endpoint *ep)
{
    ep->stalled = false;
    ep->active = false;
#if TUSB_OPT_HOST_ENABLED
    ep->sent_setup = false;
#endif
    ep->total_len = 0;
    ep->len = 0;
    ep->transfer_size = 0;
    ep->user_buf = 0;
}

void _hw_endpoint_buffer_control_update32(struct hw_endpoint *ep, uint32_t and_mask, uint32_t or_mask) {
    uint32_t value = 0;
    if (and_mask) {
        value = *ep->buffer_control & and_mask;
    }
    if (or_mask) {
        value |= or_mask;
        if (or_mask & USB_BUF_CTRL_AVAIL) {
            if (*ep->buffer_control & USB_BUF_CTRL_AVAIL) {
                panic("ep %d %s was already available", tu_edpt_number(ep->ep_addr), ep_dir_string[tu_edpt_dir(ep->ep_addr)]);
            }
            *ep->buffer_control = value & ~USB_BUF_CTRL_AVAIL;
            // 12 cycle delay.. (should be good for 48*12Mhz = 576Mhz)
            // Don't need delay in host mode as host is in charge
#if !TUSB_OPT_HOST_ENABLED
            __asm volatile (
                    "b 1f\n"
                    "1: b 1f\n"
                    "1: b 1f\n"
                    "1: b 1f\n"
                    "1: b 1f\n"
                    "1: b 1f\n"
                    "1:\n"
                    : : : "memory");
#endif
        }
    }
    *ep->buffer_control = value;
}

// Prepare buffer control register value
void _hw_endpoint_start_next_buffer(struct hw_endpoint *ep)
{
  uint16_t remaining = ep->total_len - ep->len;
  uint32_t ep_ctrl = *ep->endpoint_control;
  uint32_t buf_ctrl;

  // Buffer 0
  ep->transfer_size = tu_min16(remaining, ep->wMaxPacketSize);
  remaining -= ep->transfer_size;

  buf_ctrl = ep->transfer_size | USB_BUF_CTRL_AVAIL;
  if ( !ep->rx )
  {
    // Copy data from user buffer to hw buffer
    memcpy(ep->hw_data_buf, ep->user_buf+ep->len, ep->transfer_size);

    // Mark as full
    buf_ctrl |= USB_BUF_CTRL_FULL;
  }

  // PID
  buf_ctrl |= ep->next_pid ? USB_BUF_CTRL_DATA1_PID : USB_BUF_CTRL_DATA0_PID;
  ep->next_pid ^= 1u;

  // Buffer 1
  ep->buf_1_len = tu_min16(remaining, ep->wMaxPacketSize);
  remaining -= ep->buf_1_len;

  if (ep->buf_1_len)
  {
    buf_ctrl |= (ep->buf_1_len | USB_BUF_CTRL_AVAIL) << 16;
    buf_ctrl |= (ep->next_pid ? USB_BUF_CTRL_DATA1_PID : USB_BUF_CTRL_DATA0_PID) << 16;
    ep->next_pid ^= 1u;

    if ( !ep->rx )
    {
      // Copy data from user buffer to hw buffer
      memcpy(ep->hw_data_buf+64, ep->user_buf+ep->len+ep->transfer_size, ep->buf_1_len);
    }

    // Set endpoint control double buffered bit if needed
    ep_ctrl &= ~EP_CTRL_INTERRUPT_PER_BUFFER;
    ep_ctrl |= EP_CTRL_DOUBLE_BUFFERED_BITS | EP_CTRL_INTERRUPT_PER_DOUBLE_BUFFER;
  }else
  {
    ep_ctrl &= ~(EP_CTRL_DOUBLE_BUFFERED_BITS | EP_CTRL_INTERRUPT_PER_DOUBLE_BUFFER);
    ep_ctrl |= EP_CTRL_INTERRUPT_PER_BUFFER;
  }

  *ep->endpoint_control = ep_ctrl;

#if TUSB_OPT_HOST_ENABLED
  // Is this the last buffer? Only really matters for host mode. Will trigger
  // the trans complete irq but also stop it polling. We only really care about
  // trans complete for setup packets being sent
  if (remaining == 0)
  {
    buf_ctrl |= USB_BUF_CTRL_LAST << (ep->buf_1_len ? 16 : 0);
  }
#endif

  print_bufctrl32(buf_ctrl);

  // Finally, write to buffer_control which will trigger the transfer
  // the next time the controller polls this dpram address
  _hw_endpoint_buffer_control_set_value32(ep, buf_ctrl);
}


void _hw_endpoint_xfer_start(struct hw_endpoint *ep, uint8_t *buffer, uint16_t total_len)
{
  _hw_endpoint_lock_update(ep, 1);
  pico_trace("Start transfer of total len %d on ep %d %s\n", total_len, tu_edpt_number(ep->ep_addr),
             ep_dir_string[tu_edpt_dir(ep->ep_addr)]);
  if ( ep->active )
  {
    // TODO: Is this acceptable for interrupt packets?
    pico_warn("WARN: starting new transfer on already active ep %d %s\n", tu_edpt_number(ep->ep_addr),
              ep_dir_string[tu_edpt_dir(ep->ep_addr)]);

    hw_endpoint_reset_transfer(ep);
  }

  // Fill in info now that we're kicking off the hw
  ep->total_len = total_len;
  ep->len       = 0;
  ep->active    = true;
  ep->user_buf  = buffer;

  _hw_endpoint_start_next_buffer(ep);
  _hw_endpoint_lock_update(ep, -1);
}

void _hw_endpoint_xfer_sync (struct hw_endpoint *ep)
{
  // Update hw endpoint struct with info from hardware
  // after a buff status interrupt

  uint32_t const buf_ctrl = _hw_endpoint_buffer_control_get_value32(ep);
  print_bufctrl32(buf_ctrl);

  // Transferred bytes for each buffer
  uint16_t xferred_bytes[2];

  xferred_bytes[0] = buf_ctrl & USB_BUF_CTRL_LEN_MASK;

  // double buffered: take buffer1 into account as well
  if ( (*ep->endpoint_control) & EP_CTRL_DOUBLE_BUFFERED_BITS )
  {
    xferred_bytes[1] = (buf_ctrl >> 16) & USB_BUF_CTRL_LEN_MASK;
  }else
  {
    xferred_bytes[1] = 0;
  }

  TU_LOG_INT(2, xferred_bytes[0]);
  TU_LOG_INT(2, xferred_bytes[1]);

  // We are continuing a transfer here. If we are TX, we have successfully
  // sent some data can increase the length we have sent
  if ( !ep->rx )
  {
    assert(!(buf_ctrl & USB_BUF_CTRL_FULL));
    ep->len += xferred_bytes[0] + xferred_bytes[1];
  }
  else
  {
    // If we are OUT we have recieved some data, so can increase the length
    // we have recieved AFTER we have copied it to the user buffer at the appropriate offset
    assert(buf_ctrl & USB_BUF_CTRL_FULL);

    memcpy(&ep->user_buf[ep->len], ep->hw_data_buf, xferred_bytes[0]);
    ep->len += xferred_bytes[0];

    if (xferred_bytes[1])
    {
      memcpy(&ep->user_buf[ep->len], ep->hw_data_buf+64, xferred_bytes[1]);
      ep->len += xferred_bytes[1];
    }
  }

  // Sometimes the host will send less data than we expect...
  // If this is a short out transfer update the total length of the transfer
  // to be the current length
  if ( (ep->rx) && ((xferred_bytes[0] < ep->wMaxPacketSize) || (xferred_bytes[1] && (xferred_bytes[1] < ep->wMaxPacketSize))) )
  {
    pico_trace("Short rx transfer\n");
    // Reduce total length as this is last packet
    ep->total_len = ep->len;
  }
}

// Returns true if transfer is complete
bool _hw_endpoint_xfer_continue(struct hw_endpoint *ep)
{
    _hw_endpoint_lock_update(ep, 1);
    // Part way through a transfer
    if (!ep->active)
    {
        panic("Can't continue xfer on inactive ep %d %s", tu_edpt_number(ep->ep_addr), ep_dir_string);
    }

    // Update EP struct from hardware state
    _hw_endpoint_xfer_sync(ep);

    // Now we have synced our state with the hardware. Is there more data to transfer?
    // Limit by packet size
    uint16_t remaining_bytes = ep->total_len - ep->len;
    ep->transfer_size = tu_min16(remaining_bytes, ep->wMaxPacketSize);

    TU_LOG_INT(2, ep->transfer_size);

    // Can happen because of programmer error so check for it
    if (ep->len > ep->total_len)
    {
        panic("Transferred more data than expected");
    }

    // If we are done then notify tinyusb
    if (ep->len == ep->total_len)
    {
        pico_trace("Completed transfer of %d bytes on ep %d %s\n",
                   ep->len, tu_edpt_number(ep->ep_addr), ep_dir_string[tu_edpt_dir(ep->ep_addr)]);
        // Notify caller we are done so it can notify the tinyusb stack
        _hw_endpoint_lock_update(ep, -1);
        return true;
    }
    else
    {
        _hw_endpoint_start_next_buffer(ep);
    }

    _hw_endpoint_lock_update(ep, -1);
    // More work to do
    return false;
}

#endif
