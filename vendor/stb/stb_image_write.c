/* SPDX-License-Identifier: MIT */
/* Not upstream's: the one translation unit that holds stb_image_write's
 * implementation, compiled on its own flags like lz4 (vendor/stb/README.md).
 * PNG only - the other writers are compiled out. */
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"
