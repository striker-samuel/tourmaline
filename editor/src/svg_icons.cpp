// SVG icon set (editor/icons/*.svg): nanosvg rasterization + ImGui
// user-texture registration, plus the single-TU nanosvg build (the
// IMPLEMENTATION defines live in exactly one TU — this one).
//
// Flow (EditorApp::EnsureSvgIcons, once at startup after ImGui init):
// editor/icons/<name>.svg -> nsvgParse -> nsvgRasterize (64x64 RGBA8,
// the set's native viewBox) -> premultiply (the ImGui font pipeline
// blends premultiplied) -> ResourceManager::CreateTexture (UNORM RGBA8:
// values pass through raw like the font atlas) ->
// ImGui_ImplVulkan_AddTexture -> ImTextureID for ImDrawList::AddImage
// in the asset browser grid. A failed/missing icon keeps imgui == 0 and
// the browser falls back to the drawn vector shapes.

#define NANOSVG_IMPLEMENTATION	// parses SVG (subset) into flat shapes
#define NANOSVGRAST_IMPLEMENTATION // rasterizes shapes into RGBA8
#include <nanosvg.h>
#include <nanosvgrast.h>

#include "editor_app.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <vector>

#include <imgui.h>
#include <imgui_impl_vulkan.h>

#include <Engine/core.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace editor {

EditorApp::IconType EditorApp::IconTypeFor(const std::string& name,
	bool dir) const {
	if (dir)
		return IconType::Folder;
	std::string ext =
		std::filesystem::path(name).extension().string();
	for (char& c : ext)
		c = char(std::tolower((unsigned char)c));
	if (ext == ".gltf" || ext == ".glb" || ext == ".geometry"
		// the pre-10/6 derived-blob name (read-only legacy)
		|| ext == ".mesh")
		return IconType::Model;
	if (ext == ".tour")
		return IconType::Scene;
	if (ext == ".ktx2" || ext == ".png" || ext == ".jpg"
		|| ext == ".jpeg" || ext == ".hdr" || ext == ".dds"
		|| ext == ".tga" || ext == ".bmp" || ext == ".texture")
		return IconType::Texture;
	if (ext == ".pak")
		return IconType::Pak;
	// Cook assets (settings + blob manifests) read as GI content, and so
	// do the bake payloads/manifests themselves (previously File).
	if (ext == ".gicook" || ext == ".lmcook" || ext == ".sggi"
		|| ext == ".sglm" || ext == ".sgrv" || ext == ".cvol"
		|| ext == ".regmanifest" || ext == ".cvmanifest"
		|| ext == ".gicvolume")
		return IconType::GI;
	if (ext == ".cubemanifest" || ext == ".cubemap")
		return IconType::Env;
	return IconType::File;
}

bool EditorApp::EnsureSvgIcons() {
	if (m_iconsReady)
		return true;
	m_iconsReady = true; // one attempt; failures fall back to shapes
	if (!m_context || !m_imgui || !m_imgui->IsInitialized())
		return false;
	// exe-relative staged icon dir (the build copies editor/icons next
	// to Editor.exe; repo layout is not available from installed runs).
	std::string dir = "editor/icons";
#ifdef _WIN32
	{
		char exePath[1024] = {};
		const DWORD n = GetModuleFileNameA(nullptr, exePath,
			DWORD(sizeof(exePath)));
		(void)n;
		if (exePath[0])
			dir = (std::filesystem::path(exePath).parent_path()
				/ "editor" / "icons").generic_string();
	}
#endif
	static constexpr struct {
		IconType t;
		const char* file;
	} kIcons[] = {
		{ IconType::Folder, "folder.svg" },
		{ IconType::Pak, "pak.svg" },
		{ IconType::Model, "mesh.svg" },
		{ IconType::Scene, "scene.svg" },
		{ IconType::Texture, "texture.svg" },
		{ IconType::File, "file.svg" },
		{ IconType::Light, "light.svg" },
		{ IconType::Camera, "camera.svg" },
		{ IconType::Env, "enviroment.svg" },
		{ IconType::GI, "global_illumination.svg" },
		{ IconType::Character, "character.svg" },
		{ IconType::Skeleton, "skeleton.svg" },
		{ IconType::Animation, "animation.svg" },
		{ IconType::Timeline, "timeline.svg" },
		{ IconType::Settings, "settings.svg" },
	};
	// The user-texture sampler: linear + clamp-to-edge.
	const uint32_t samplerIdx = m_context->GetOrCreateSampler(
		smvk::SamplerCreateInfo{
			.magFilter = smvk::Filter::Linear,
			.minFilter = smvk::Filter::Linear,
			.mipmapMode = smvk::MipmapMode::Linear,
			.addressU = smvk::AddressMode::ClampToEdge,
			.addressV = smvk::AddressMode::ClampToEdge,
			.addressW = smvk::AddressMode::ClampToEdge,
			.maxAnisotropy = 1.0f });
	const void* vkSampler =
		m_context->GetVkSamplerOpaque(samplerIdx);
	size_t ok = 0;
	for (const auto& [t, file] : kIcons) {
		IconTex& slot = m_icons[size_t(t)];
		const std::filesystem::path path =
			std::filesystem::path(dir) / file;
		std::error_code ec;
		if (!std::filesystem::is_regular_file(path, ec))
			continue;
		// nsvgParse MUTATES the buffer (writes NULs): a mutable copy.
		std::vector<char> svg = readFile(path.generic_string());
		if (svg.empty())
			continue;
		svg.resize(svg.size() + 1, '\0');
		NSVGimage* img = nsvgParse(svg.data(), "px", 96.0f);
		if (!img || !(img->width > 0.0f) || !(img->height > 0.0f))
			continue;
		constexpr int kSize = 64;
		std::vector<unsigned char> px(
			size_t(kSize) * size_t(kSize) * 4, 0);
		{
			NSVGrasterizer* rast = nsvgCreateRasterizer();
			nsvgRasterize(rast, img, 0.0f, 0.0f,
				float(kSize) / float(img->width), px.data(), kSize,
				kSize, kSize * 4);
			nsvgDeleteRasterizer(rast);
		}
		nsvgDelete(img);
		// Premultiplied alpha (the ImGui font pipeline's blending
		// expects it; straight alpha fringes at the stroke edges).
		for (size_t i = 0; i + 3 < px.size(); i += 4) {
			const unsigned char a = px[i + 3];
			if (a == 255)
				continue;
			px[i + 0] = unsigned char((px[i + 0] * a + 127) / 255);
			px[i + 1] = unsigned char((px[i + 1] * a + 127) / 255);
			px[i + 2] = unsigned char((px[i + 2] * a + 127) / 255);
		}
		// UNORM RGBA8: raw value passthrough like the font atlas (the
		// icons' sRGB-encoded colors must not linearize on sample).
		smvk::ImageCreateInfo ci{};
		ci.extent = smvk::Extent3D{ uint32_t(kSize), uint32_t(kSize),
			1 };
		ci.format = smvk::Format::RGBA8_UNorm;
		ci.mipLevels = 1;
		ci.arrayLayers = 1;
		ci.name = "svg icon";
		slot.tex = m_context->CreateTexture(ci, px.data());
		if (slot.tex.empty() || !vkSampler)
			continue;
		const void* vkView =
			m_context->GetTextureVkImageView(slot.tex);
		if (!vkView)
			continue;
		VkDescriptorSet set = ImGui_ImplVulkan_AddTexture(
			VkSampler(vkSampler), VkImageView(vkView),
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		if (set == VK_NULL_HANDLE) {
			slot.tex = {};
			continue;
		}
		slot.imgui = ImTextureID(reinterpret_cast<ImU64>(set));
		++ok;
	}
	Log("Icons: " + std::to_string(ok) + "/"
		+ std::to_string(size_t(IconType::Count))
		+ " SVG icon(s) rasterized (from '" + dir + "').");
	return true;
}

} // namespace editor
