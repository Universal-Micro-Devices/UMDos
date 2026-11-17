/* sha3.h - SHA3-256 */
#ifndef SHA3_H
#define SHA3_H

#include "types.h"

void sha3_256(const void *data, u32 len, u8 out[32]);

#endif
