#define STB_IMAGE_IMPLEMENTATION
#include "../vendor/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../vendor/stb_image_write.h"

#include "lut_io.h"

bool LutLoadPng(const char* path, std::vector<uint8_t>& rgba, int& w, int& h)
{
	int comp = 0;
	stbi_uc* pixels = stbi_load(path, &w, &h, &comp, 4);
	if (!pixels)
		return false;
	rgba.assign(pixels, pixels + static_cast<size_t>(w) * h * 4);
	stbi_image_free(pixels);
	return true;
}

bool LutWritePng(const char* path, int w, int h, const uint8_t* rgba)
{
	return stbi_write_png(path, w, h, 4, rgba, w * 4) != 0;
}
