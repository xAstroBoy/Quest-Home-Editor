/* stb_truetype implementation TU — rasterizes the editor UI font atlas (Inter / system fallback).
   Kept separate (like stb_image_impl.c / stb_vorbis_impl.c) so the header can be included for declarations
   in the C++ UI code without duplicating the implementation. */
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
