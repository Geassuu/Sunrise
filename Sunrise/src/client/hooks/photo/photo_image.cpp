/**
 * The single translation unit that compiles the stb_image_write implementation the photo
 * shutter links against. Everything else includes the header for declarations only.
 * The vendor header calls the deprecated sprintf family, so that one warning is muted here;
 * the project treats every other warning as an error.
 */

#define STB_IMAGE_WRITE_IMPLEMENTATION
#pragma warning(push)
#pragma warning(disable : 4996)
#include "../../../../vendor/stb/stb_image_write.h"
#pragma warning(pop)
