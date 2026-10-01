#ifndef PWMVID_BOOT_CONTRACT_H
#define PWMVID_BOOT_CONTRACT_H
#include <stdint.h>
#include "boot_contract_cb.h"
#define PWMVID_APP_MAX_BYTES (PWMVID_FOOTER_BASE-PWMVID_APP_BASE)
typedef struct {
 uint32_t magic,length,crc,format,commit,commit_inverse,board,board_inverse;
} pwmvid_image_footer_t;
/* All words little-endian. CRC32/ISO-HDLC covers exactly length bytes from
 * PWMVID_APP_BASE, including up-to-7 trailing 0xff alignment bytes. Footer is
 * outside the CRC span. The commit+inverse doubleword is programmed LAST.
 * Cookie is consumed only with RCC_CSR_SFTRSTF, before HAL/IWDG initialization.
 * Both linker scripts reserve its 8 bytes and place stack top at 0x200057f8. */
#endif
