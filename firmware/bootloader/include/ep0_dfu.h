#ifndef PWMVID_EP0_DFU_H
#define PWMVID_EP0_DFU_H
#include <stdint.h>
#include <stdbool.h>
void ep0_reset(void);
void ep0_setup(const uint8_t setup[8]);
void ep0_out(const uint8_t *data,uint16_t count);
void ep0_in_complete(void);
void ep0_poll(void);
void boot_usb_error(uint8_t status);
/* Port primitives. All callbacks and poll run in foreground. Packets <=64 B.
 * Stock Cube HAL does not notify zero-length OUT; status OUT is therefore armed
 * before pending Flash work is run after the final GETSTATUS IN completion. */
void ep0_port_send(const uint8_t *data,uint16_t count);
void ep0_port_receive(void);
void ep0_port_stall(void);
void ep0_port_address(uint8_t address);
void ep0_port_leave(void);
uint32_t ep0_port_uid(unsigned word);
#endif
