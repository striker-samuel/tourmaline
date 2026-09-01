#pragma once

#include <cassert>
#include <cstdint>
#include <utility>
#include <vector>

#include <Engine/smvk.h>

namespace smvk {

	// Forward declare destroy overloads (defined in RHI/resource_manager.cpp private TU)
	void destroy(ResourceManager* ctx, ImageHandle handle);
	void destroy(ResourceManager* ctx, TextureHandle handle);
	void destroy(ResourceManager* ctx, BufferHandle handle);
	void destroy(ResourceManager* ctx, SamplerHandle handle);
	void destroy(ResourceManager* ctx, BindlessImageHandle handle);
	void destroy(ResourceManager* ctx, ImageViewHandle handle);
	void destroy(ResourceManager* ctx, DescriptorPoolHandle handle);
	void destroy(ResourceManager* ctx, ShaderModuleHandle handle);

	template<typename HandleType>
	class Holder final {
	public:
		Holder() = default;
		template<typename Ctx>
		Holder(Ctx* ctx, HandleType handle) : ctx_(ctx), handle_(handle) {
			deleter_ = [](void* c, HandleType h){ destroy(static_cast<Ctx*>(c), h); };
		}
		~Holder() {
			if (ctx_ && deleter_ && !handle_.empty()) deleter_(ctx_, handle_);
		}
		Holder(const Holder&) = delete;
		Holder(Holder&& other) : ctx_(other.ctx_), deleter_(other.deleter_), handle_(other.handle_) {
			other.ctx_ = nullptr;
			other.deleter_ = nullptr;
			other.handle_ = HandleType{};
		}
		Holder& operator=(const Holder&) = delete;
		Holder& operator=(Holder&& other) {
			std::swap(ctx_, other.ctx_);
			std::swap(deleter_, other.deleter_);
			std::swap(handle_, other.handle_);
			return *this;
		}
		Holder& operator=(std::nullptr_t) {
			this->reset();
			return *this;
		}

		inline operator HandleType() const {
			return handle_;
		}

		bool valid() const {
			return handle_.valid();
		}

		bool empty() const {
			return handle_.empty();
		}

		void reset() {
			if (ctx_ && deleter_ && !handle_.empty()) deleter_(ctx_, handle_);
			ctx_ = nullptr;
			deleter_ = nullptr;
			handle_ = HandleType{};
		}

		HandleType release() {
			ctx_ = nullptr;
			deleter_ = nullptr;
			return std::exchange(handle_, HandleType{});
		}

		uint32_t gen() const {
			return handle_.gen();
		}
		uint32_t index() const {
			return handle_.index();
		}
		void* indexAsVoid() const {
			return handle_.indexAsVoid();
		}

	private:
		void* ctx_ = nullptr;
		void(*deleter_)(void*, HandleType) = nullptr;
		HandleType handle_ = {};
	};

	template<typename ObjectType, typename ImplObjectType>
	class Pool {
		static constexpr uint32_t kListEndSentinel = 0xffffffff;
		struct PoolEntry {
			explicit PoolEntry(ImplObjectType& obj) : obj_(std::move(obj)) {}
			ImplObjectType obj_ = {};
			uint32_t gen_ = 1;
			uint32_t nextFree_ = kListEndSentinel;
			bool occupied_ = false;
		};
		uint32_t freeListHead_ = kListEndSentinel;
		uint32_t numObjects_ = 0;

	public:
		std::vector<PoolEntry> objects_;
		Handle<ObjectType> create(ImplObjectType&& obj) {
			uint32_t idx = 0;
			if (freeListHead_ != kListEndSentinel) {
				idx = freeListHead_;
				freeListHead_ = objects_[idx].nextFree_;
				objects_[idx].obj_ = std::move(obj);
			}
			else {
				idx = (uint32_t)objects_.size();
				objects_.emplace_back(obj);
			}
			objects_[idx].occupied_ = true;
			numObjects_++;
			return Handle<ObjectType>(idx, objects_[idx].gen_);
		}
		void destroy(Handle<ObjectType> handle) {
			if (handle.empty())
				return;
			assert(numObjects_ > 0); // double deletion
			const uint32_t index = handle.index();
			assert(index < objects_.size());
			assert(handle.gen() == objects_[index].gen_); // double deletion
			objects_[index].obj_ = ImplObjectType{};
			objects_[index].gen_++;
			objects_[index].nextFree_ = freeListHead_;
			objects_[index].occupied_ = false;
			freeListHead_ = index;
			numObjects_--;
		}
		const ImplObjectType* get(Handle<ObjectType> handle) const {
			if (handle.empty())
				return nullptr;

			const uint32_t index = handle.index();
			assert(index < objects_.size());
			assert(handle.gen() == objects_[index].gen_); // accessing deleted object
			return &objects_[index].obj_;
		}
		ImplObjectType* get(Handle<ObjectType> handle) {
			if (handle.empty())
				return nullptr;

			const uint32_t index = handle.index();
			assert(index < objects_.size());
			assert(handle.gen() == objects_[index].gen_); // accessing deleted object
			return &objects_[index].obj_;
		}
		Handle<ObjectType> getHandle(uint32_t index) const {
			assert(index < objects_.size());
			if (index >= objects_.size())
				return {};

			return Handle<ObjectType>(index, objects_[index].gen_);
		}
		Handle<ObjectType> findObject(const ImplObjectType* obj) {
			if (!obj)
				return {};

			for (size_t idx = 0; idx != objects_.size(); idx++) {
				if (objects_[idx].obj_ == *obj) {
					return Handle<ObjectType>((uint32_t)idx, objects_[idx].gen_);
				}
			}

			return {};
		}
		void clear() {
			objects_.clear();
			freeListHead_ = kListEndSentinel;
			numObjects_ = 0;
		}
		uint32_t numObjects() const {
			return numObjects_;
		}

		template<typename Func>
		void forEach(Func&& func)
		{
			for (auto& entry : objects_) {
				if (entry.occupied_) {
					func(entry.obj_);
				}
			}
		}

		template<typename Func>
		void forEachHandle(Func&& func)
		{
			for (uint32_t i = 0; i < objects_.size(); ++i) {
				if (objects_[i].occupied_) {
					func(Handle<ObjectType>(
						i,
						objects_[i].gen_
					));
				}
			}
		}

	};
}; // namespace smvk
