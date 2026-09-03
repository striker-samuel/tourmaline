#pragma once
#include <stdint.h>
#include <vector>

namespace smvk {

	// Lifted from LightweightVk.
	// Non-ref counted handles; based on:
	// https://enginearchitecture.realtimerendering.com/downloads/reac2023_modern_mobile_rendering_at_hypehype.pdf
	template<typename ObjectType>
	class Handle final {
	public:
		Handle() = default;

		bool empty() const {
			return gen_ == 0;
		}
		bool valid() const {
			return gen_ != 0;
		}
		uint32_t index() const {
			return index_;
		}
		uint32_t gen() const {
			return gen_;
		}
		void* indexAsVoid() const {
			return reinterpret_cast<void*>(static_cast<ptrdiff_t>(index_));
		}
		bool operator==(const Handle<ObjectType>& other) const {
			return index_ == other.index_ && gen_ == other.gen_;
		}
		bool operator!=(const Handle<ObjectType>& other) const {
			return index_ != other.index_ || gen_ != other.gen_;
		}
		// allow conditions 'if (handle)'
		explicit operator bool() const {
			return gen_ != 0;
		}

	private:
		Handle(uint32_t index, uint32_t gen) : index_(index), gen_(gen) {};

		template<typename ObjectType_, typename ImplObjectType>
		friend class Pool;

		template<typename ObjectType_, typename ImplObjectType>
		friend class ShardedPool;

		uint32_t index_ = 0;
		uint32_t gen_ = 0;
	};

	static_assert(sizeof(Handle<class Foo>) == sizeof(uint64_t));

}; //namespace smvk