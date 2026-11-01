/* stb implementations (kept in their own file so warnings can be turned off) */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_PSD
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_TGA
#include "vendor/stb_image.h"
#define STB_TRUETYPE_IMPLEMENTATION
#include "vendor/stb_truetype.h"
