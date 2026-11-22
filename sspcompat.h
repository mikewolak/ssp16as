/* sspcompat.h  -  fixed-width types and big-endian byte-lane helpers for ssp16as.
 * Project svp-lara, Mike Wolak, EPROM Foundry Inc.  Structure of esdsp's escompat.h (project dp4plusplus, Mike Wolak,
 * EPROM Foundry Inc.), reduced to the SSP1601's 16-bit word.
 *
 * Every word access to a cartridge image goes through rd16be / wr16be: the image is big-endian (the 68000's byte
 * order); SSP program word w is image bytes 2w, 2w+1.
 */
#ifndef SSPCOMPAT_H
#define SSPCOMPAT_H

#include <stdint.h>
typedef uint8_t  ssp_u8;
typedef uint16_t ssp_u16;
typedef uint32_t ssp_u32;

ssp_u16 rd16be(const ssp_u8 *p);
void    wr16be(ssp_u8 *p, ssp_u16 v);

#endif /* SSPCOMPAT_H */
