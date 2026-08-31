#pragma once

#include <stdint.h>
#include <vector>

namespace smvk {

#if defined(ENGINE_VULKAN)
	struct VulkanResourceManager;
	using ResourceManagerImpl = VulkanResourceManager;
#else
#error "No rendering backend selected"
#endif


	template<typename HandleType>
	class Holder final {
	public:
		Holder() = default;
		Holder(ResourceManagerImpl* ctx, HandleType handle) : ctx_(ctx), handle_(handle) {}
		~Holder() {
			destroy(ctx_, handle_);
		}
		Holder(const Holder&) = delete;
		Holder(Holder&& other) : ctx_(other.ctx_), handle_(other.handle_) {
			other.ctx_ = nullptr;
			other.handle_ = HandleType{};
		}
		Holder& operator=(const Holder&) = delete;
		Holder& operator=(Holder&& other) {
			std::swap(ctx_, other.ctx_);
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
			destroy(ctx_, handle_);
			ctx_ = nullptr;
			handle_ = HandleType{};
		}

		HandleType release() {
			ctx_ = nullptr;
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
		ResourceManagerImpl* ctx_ = nullptr;
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
