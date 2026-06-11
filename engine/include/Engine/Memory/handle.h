#pragma once

#include <stdint.h>

class Context;

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

    uint32_t index_ = 0;
    uint32_t gen_ = 0;
};

static_assert(sizeof(Handle<class Foo>) == sizeof(uint64_t));


template<typename HandleType>
class Holder final {
public:
    Holder() = default;
    Holder(Context* ctx, HandleType handle) : ctx_(ctx), handle_(handle) {}
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
    Context* ctx_ = nullptr;
    HandleType handle_ = {};
};



// CONTEXT
using ImageHandle = Handle<struct Image>;
using TextureHandle = Handle<struct Texture>;

using ShaderModuleHandle = Handle<struct ShaderModule>;
using DescriptorPoolHandle = Handle<struct DescriptorPool>;
using DescriptorSetHandle = Handle<struct DescriptorSet>;

using PipelineHandle = Handle<struct Pipeline>;

//using PipelineLayoutHandle = Handle<struct Pipeline::Layout>;
using DeviceBufferHandle = Handle<struct DeviceBuffer>;

//MESH MANAGER
using MeshHandle = Handle<struct Mesh>;
using GeometryPageHandle = Handle<struct GeometryPage>;
