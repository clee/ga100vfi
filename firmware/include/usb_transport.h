#ifndef PWMVID_USB_TRANSPORT_H
#define PWMVID_USB_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 255 text bytes plus a terminating NUL; CR/LF are not returned. */
#define USB_TRANSPORT_LINE_SIZE 256U
#define USB_TRANSPORT_RX_RING_SIZE 1024U
#define USB_TRANSPORT_TX_SIZE 768U

/* Initialize once, after HAL and the 48 MHz USB clock have been configured. */
bool usb_transport_init(void);
/* A configured, unsuspended interface with a fresh host DTR assertion. */
bool usb_transport_connected(void);
/* Main-loop-only: bounded work, at most one complete nonempty line per call. */
bool usb_transport_poll_line(char *line, size_t capacity);
/* Main-loop-only: copies at most 768 bytes; drops rather than waiting. */
bool usb_transport_write(const char *text);
/* Session-loss epoch, not a count of individual physical unplug operations. */
uint32_t usb_transport_loss_count(void);
/* Includes ring full, overlong lines, invalid text and undersized line buffers. */
uint32_t usb_transport_rx_overflow_count(void);
uint32_t usb_transport_tx_drop_count(void);

#ifdef __cplusplus
}
#endif
#endif
