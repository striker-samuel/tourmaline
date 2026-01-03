#include <Engine/Renderer/constants.h>

struct RGB { float r, g, b; };

static RGB hsv2rgb(float h, float s, float v)
{
	float c = v * s;
	float x = c * (1.0f - fabsf(fmodf(h * 6.0f, 2.0f) - 1.0f));
	float m = v - c;

	float r, g, b;
	if (h < 1.0f / 6.0f) { r = c; g = x; b = 0; }
	else if (h < 2.0f / 6.0f) { r = x; g = c; b = 0; }
	else if (h < 3.0f / 6.0f) { r = 0; g = c; b = x; }
	else if (h < 4.0f / 6.0f) { r = 0; g = x; b = c; }
	else if (h < 5.0f / 6.0f) { r = x; g = 0; b = c; }
	else { r = c; g = 0; b = x; }

	return { r + m, g + m, b + m };
}

void FabricateLightData(Light* data)
{

	for (int i = 0; i < MAX_LIGHTS; i++) {
		data[i].position[0] = 3.0f * i;
		data[i].position[1] = 1.0f;
		data[i].position[2] = 0.0f;

		data[i].direction[0] = 4.0f;
		data[i].direction[1] = .0f;
		data[i].direction[2] = .0f;

		float hue = float(i) / float(MAX_LIGHTS);
		RGB rgb = hsv2rgb(hue, 1.0f, 1.0f);

		data[i].color = { rgb.r, rgb.g, rgb.b, 1.0f };
		data[i].intensity = 1.0f;
		//data[i].intensity = .0f;
		data[i].type = LIGHT_TYPE_SPOT;
		if (i % 2 == 1) {
			data[i].type = LIGHT_TYPE_POINT;
			data[i].intensity = 1.0f;
			//	data[i].position[1] = 3.0f;
			//	data[i].position[0] = 2.0f;
			//	data[i].position[2] = 3.0f;
			//}
		}
	}
}
