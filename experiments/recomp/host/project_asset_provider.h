// SPDX-License-Identifier: MIT
#pragma once
// Adapter over Tourmaline's EXISTING ProjectRuntime + ContentRegistry.
// Never opens an asset directly, never copies assets into source control,
// never creates a second pak format, and never resolves the user's ROM.
// Project.exe owns import/cook/pak/manifest; runtime owns provider selection.
#include <ProjectRuntime.h>
#include <Engine/Asset/appmanifest.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
namespace tour::recomp {
class ProjectAssetProvider {
public:
    bool Open(const std::string& manifest, std::string& error) {
        runtime_.reset();
        runtime_=app::ProjectRuntime::Load(manifest,error);
        return runtime_.has_value();
    }
    bool Ready() const{return runtime_.has_value();}
    std::size_t Read(std::uint64_t id,std::uint8_t* dst,
                     std::size_t capacity) {
        if(!runtime_ || id==0)return 0;
        std::string error;
        const auto record=runtime_->Asset(id);
        if(!record) {
            std::fprintf(stderr,"[recomp-assets] unknown catalog id 0x%llx\n",
                         static_cast<unsigned long long>(id));
            return 0;
        }
        // Fail closed on scene/mesh/generic kinds the runtime cannot
        // serve as byte assets; ProjectRuntime performs the kind gate.
        auto data=runtime_->ReadAsset(id,error);
        if(!data || data->empty()) {
            if(!error.empty())std::fprintf(stderr,
                "[recomp-assets] asset 0x%llx: %s\n",
                static_cast<unsigned long long>(id),error.c_str());
            return 0;
        }
        if(!dst)return data->size(); // size query
        if(capacity<data->size())return 0;
        std::memcpy(dst,data->data(),data->size());
        return data->size();
    }
    static std::size_t ReadCallback(void* user,std::uint64_t id,
                                    std::uint8_t* dst,std::size_t capacity){
        if(!user)return 0;
        return static_cast<ProjectAssetProvider*>(user)->Read(id,dst,capacity);
    }
    bool OverridePak(const std::string& name,const std::string& path,
                     std::string& error) {
        if(!runtime_){error="No external content project loaded";return false;}
        return runtime_->AddPakOverride(name,path,error);
    }
    bool OverrideLoose(const std::string& name,const std::string& root,
                       std::string& error) {
        if(!runtime_){error="No external content project loaded";return false;}
        return runtime_->AddLooseOverride(name,root,error);
    }
private:
    std::optional<app::ProjectRuntime> runtime_;
};
} // namespace tour::recomp
