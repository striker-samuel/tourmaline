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

void FabricateLightData(std::vector<Light>& lights)
{

	Light light;
	light.color = glm::vec4(1.0f);
	light.position[0] = .0f;
	light.position[1] = .0f;

	light.position[2] = .0f;
	light.direction[0] = -1.f;
	light.direction[1] = -1.f;
	light.direction[2] = 0.45f;

	light.intensity = 1;
	light.flags = 0;
	light.type = LIGHT_TYPE_DIRECTIONAL;
	light.shadow_map_index = 0;
	lights.push_back(light);

	for (int i = 1; i < 3; i++) {

		Light new_l;

		new_l.position[0] = 3.0f * i;
		new_l.position[1] = 1.0f;
		new_l.position[2] = 0.0f;

		new_l.direction[0] = 4.0f;
		new_l.direction[1] = .0f;
		new_l.direction[2] = .0f;

		float hue = float(i) / float(MAX_LIGHTS);
		RGB rgb = hsv2rgb(hue, 1.0f, 1.0f);

		new_l.color = { rgb.r, rgb.g, rgb.b, 1.0f };
		new_l.intensity = 1.0f;
		new_l.type = LIGHT_TYPE_SPOT;



		light.shadow_map_index = i + 2;
		lights.push_back(new_l);

	}


}
