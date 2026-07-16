/* byteswap.h shim so kernel host tools build on macOS.
 * Author: Marco Müller <hello@annoyedmilk.ch> */
#ifndef _S31_BYTESWAP_SHIM
#define _S31_BYTESWAP_SHIM
#include <libkern/OSByteOrder.h>
#define bswap_16 OSSwapInt16
#define bswap_32 OSSwapInt32
#define bswap_64 OSSwapInt64
#endif
