/* sha256.h */
#ifndef SHA256_H
#define SHA256_H

#include "types.h"

void sha256(const void *data, u32 len, u8 out[32]);

#endif
