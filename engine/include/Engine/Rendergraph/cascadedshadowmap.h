#pragma once
#include "rendergraph.h"

namespace smvk {

	struct LogicalCascadedShadowAtlas
	{
		struct Cascade
		{
			smvk::Extent2D offset;
			smvk::Extent2D extent;

			glm::vec2 uvScale;
			glm::vec2 uvBias;
		};

		LogicalCascadedShadowAtlas(
			RenderGraph* rg,
			smvk::Format format,
			smvk::Extent2D cascadeExtent,
			uint32_t cascadeCount,
			const char* name = nullptr)
			: cascadeExtent_(cascadeExtent)
			, cascadeCount_(cascadeCount)
		{
			assert(cascadeExtent.width != 0);
			assert(cascadeExtent.height != 0);
			assert(cascadeCount != 0);

			const uint32_t columns =
				static_cast<uint32_t>(
					std::ceil(std::sqrt(static_cast<float>(cascadeCount)))
					);

			const uint32_t rows =
				(cascadeCount + columns - 1) / columns;

			atlasExtent_ = {
				cascadeExtent.width * columns,
				cascadeExtent.height * rows
			};

			ImageCreateInfo info{
				.type = ImageType::Image2D,
				.extent = {
					atlasExtent_.width,
					atlasExtent_.height,
					1
				},
				.format = format,
				.usage =
					ImageUsage::DepthStencilAttachment |
					ImageUsage::Sampled,
				.samples = SampleCount::Count1,
				.mipLevels = 1,
				.arrayLayers = 1,
				.name = name
			};

			image = rg->AddImage(info, name);

			view = rg->AddImageView(
				image,
				ImageViewType::View2D,
				format,
				{
					.aspect = ImageAspect::Depth,
					.baseMipLevel = 0,
					.mipLevelCount = 1,
					.baseArrayLayer = 0,
					.arrayLayerCount = 1
				}
			);

			cascades.reserve(cascadeCount);

			const glm::vec2 invAtlasExtent{
				1.0f / static_cast<float>(atlasExtent_.width),
				1.0f / static_cast<float>(atlasExtent_.height)
			};

			for (uint32_t i = 0; i < cascadeCount; ++i)
			{
				const uint32_t x = i % columns;
				const uint32_t y = i / columns;

				const smvk::Extent2D offset{
					static_cast<int32_t>(x * cascadeExtent.width),
					static_cast<int32_t>(y * cascadeExtent.height)
				};

				Cascade cascade{
					.offset = offset,
					.extent = cascadeExtent,

					.uvScale = {
						static_cast<float>(cascadeExtent.width) * invAtlasExtent.x,
						static_cast<float>(cascadeExtent.height) * invAtlasExtent.y
					},

					.uvBias = {
						static_cast<float>(offset.width) * invAtlasExtent.x,
						static_cast<float>(offset.height) * invAtlasExtent.y
					}
				};

				cascades.push_back(cascade);
			}
		}

		[[nodiscard]] const Cascade& GetCascade(uint32_t index) const
		{
			assert(index < cascadeCount_);
			return cascades[index];
		}

		[[nodiscard]] uint32_t CascadeCount() const
		{
			return cascadeCount_;
		}

		[[nodiscard]] smvk::Extent2D AtlasExtent() const
		{
			return atlasExtent_;
		}

		[[nodiscard]] smvk::Extent2D CascadeExtent() const
		{
			return cascadeExtent_;
		}

		smvk::Extent2D atlasExtent_;
		smvk::Extent2D cascadeExtent_;

		uint32_t cascadeCount_;

		LogicalImageHandle image;
		LogicalImageViewHandle view;

		std::vector<Cascade> cascades;
	};


}; //namespace smvk
