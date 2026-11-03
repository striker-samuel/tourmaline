// SPDX-License-Identifier: MIT
// Optional Vulkan/SDL3 presentation smoke test for Tourmaline recomp hosting.
// This is NOT Super Mario 64. It deliberately uses synthetic video only.
// The element shares Tourmaline's *existing* App, ResourceManager, RenderGraph,
// GameUiRenderer, SDL3 input and swapchain ownership. No game data is shipped.
#include "../portkit.h"
#include <Engine/App/app.h>
#include <Engine/App/Element.h>
#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/Renderer/GameUiRenderer.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/RHI/swapchain.h>
#include <Engine/Utils/shader_library.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {
using tour::recomp::Byte;
namespace input=tour::input;
constexpr std::uint32_t W=320,H=240;

struct DiagnosticGame {
    std::array<Byte,W*H*4> pixels{};
    TourRecompControllerV1 pad{};
    std::uint64_t ticks=0;
    void Draw() {
        // Pattern visibly changes with analog input, A/B and original ticks.
        const unsigned ax=unsigned(int(pad.stick_x)+80);
        const unsigned ay=unsigned(int(pad.stick_y)+80);
        for(std::uint32_t y=0;y<H;++y) for(std::uint32_t x=0;x<W;++x) {
            const auto p=(std::size_t(y)*W+x)*4;
            const bool checker=(((x/16)+(y/16)+(ticks/8))%2)!=0;
            pixels[p+0]=Byte((x+ticks*3+ax)&0xff);
            pixels[p+1]=Byte((y+ticks+ay)&0xff);
            pixels[p+2]=Byte(checker?(pad.buttons&0x8000?255:170):
                                         (pad.buttons&0x4000?210:40));
            pixels[p+3]=255;
        }
    }
};
extern "C" int DemoBoot(void* u,const Byte* rom,size_t romLen,
                        Byte* mem,size_t memLen,Byte* save,size_t saveLen,
                        const TourRecompHostV1* callbacks) {
    if(!u||!rom||romLen!=16||!mem||memLen!=tour::recomp::RdramBytes||
       !save||saveLen!=512||!callbacks)return 0;
    auto& g=*static_cast<DiagnosticGame*>(u);
    g.ticks=0;g.Draw();
    if(callbacks->log)callbacks->log(callbacks->user,1,
                                    "Synthetic adapter booted (NOT SM64)");
    return 1;
}
extern "C" void DemoStep(void* u,TourRecompControllerV1 pad) {
    auto& g=*static_cast<DiagnosticGame*>(u);
    g.pad=pad;++g.ticks;g.Draw();
}
extern "C" int DemoCapture(void* u,TourRecompFrameV1* out) {
    if(!out)return 0;
    auto& g=*static_cast<DiagnosticGame*>(u);
    *out={g.pixels.data(),W,H,W*4};
    return 1;
}
extern "C" void DemoShutdown(void*) {}
void DemoLog(void*,std::uint32_t level,const char* msg) {
    std::fprintf(stderr,"[RecompDiagnostic:%u] %s\n",level,msg?msg:"");
}

class DiagnosticElement final:public engine::IAppElement {
public:
    explicit DiagnosticElement(tour::App& host):host_(host) {}
    const char* ElementName() const override {return "RecompDiagnostic";}
    bool onAttach() override {
        auto* ctx=host_.GetResourceManager();
        if(!ctx || !host_.GetRendergraph() || !host_.GetSwapchain())return false;
        tour::shaders::Init();
        if(!ui_.EnsureShaders(*ctx)) {
            std::fprintf(stderr,"Missing gameui shaders; stage engine-shaders.pak\n");
            return false;
        }
        game_.Draw();
        TourRecompApiV1 api{};
        api.abi_version=TOUR_RECOMP_ABI_VERSION;
        api.struct_bytes=sizeof(api);
        api.capabilities=TOUR_RECOMP_CAP_RGBA_FRAME;
        api.user=&game_;
        api.boot=DemoBoot;
        api.step=DemoStep;
        api.frame_rgba=DemoCapture;
        api.shutdown=DemoShutdown;
        TourRecompHostV1 callbacks{};
        callbacks.struct_bytes=sizeof(callbacks);
        callbacks.log=DemoLog;
        if(!runner_.Start(api,syntheticContent_,memory_,save_,callbacks,error_)){
            std::fprintf(stderr,"Synthetic adapter rejected: %s\n",error_.c_str());
            return false;
        }
        return RebindSwapchainViews();
    }
    void onDetach() override {
        runner_.Stop();
        if(host_.GetRendergraph()){
            ui_.Destroy(*host_.GetRendergraph());
            FreeSwapchainViews();
        }
        texture_.reset();
    }
    void onResize(smvk::Extent2D) override {
        if(!RebindSwapchainViews())host_.RequestClose();
    }
    void onUpdate(float dt) override {
        if(!runner_.Running())return;
        tour::recomp::Controller pad{};
        if(host_.m_input.isKeyHeld(input::Key::Space) ||
           host_.m_input.gamepadButtonHeld(0,input::GamepadButton::South))
            pad.buttons |= tour::recomp::A;
        if(host_.m_input.isKeyHeld(input::Key::LeftShift) ||
           host_.m_input.gamepadButtonHeld(0,input::GamepadButton::East))
            pad.buttons |= tour::recomp::B;
        if(host_.m_input.isKeyHeld(input::Key::Enter) ||
           host_.m_input.gamepadButtonHeld(0,input::GamepadButton::Start))
            pad.buttons |= tour::recomp::Start;
        float x=0,y=0;
        host_.m_input.gamepadStick(0,true,x,y);
        if(host_.m_input.isKeyHeld(input::Key::A))x=-1;
        if(host_.m_input.isKeyHeld(input::Key::D))x=1;
        if(host_.m_input.isKeyHeld(input::Key::W))y=-1;
        if(host_.m_input.isKeyHeld(input::Key::S))y=1;
        pad.x=tour::recomp::Stick(x);
        pad.y=tour::recomp::Stick(-y); // N64 forward = positive Y
        const auto before=game_.ticks;
        if(!runner_.Pump(dt,pad,error_)){
            std::fprintf(stderr,"Adapter failure: %s\n",error_.c_str());
            host_.RequestClose();return;
        }
        if(game_.ticks==before)return; // 30-Hz logic; present at monitor rate.
        const auto& image=runner_.Video();
        smvk::ImageCreateInfo ci{};
        ci.extent={image.Width(),image.Height(),1};
        ci.format=smvk::Format::RGBA8_UNorm;
        ci.name="Synthetic recomp RGBA output";
        auto* ctx=host_.GetResourceManager();
        if(texture_.empty()) texture_=ctx->CreateTexture(ci,image.Pixels().data());
        else ctx->UpdateTexture(texture_,ci,image.Pixels().data());
        if(texture_.empty()){
            std::fprintf(stderr,"Recomp video texture allocation failed\n");
            host_.RequestClose();return;
        }
        ctx->FlushUploads();
    }
    void onRender(engine::ElementFrame& frame) override {
        if(!frame.fb || !host_.GetRendergraph() ||
           frame.fb->imageIndex>=views_.size())return;
        auto& graph=*host_.GetRendergraph();
        auto& ctx=*host_.GetResourceManager();
        graph.NewFrame();
        ui_.BeginFrame();
        if(!texture_.empty()){
            const float sx=float(frame.swapExtent.width)/float(W);
            const float sy=float(frame.swapExtent.height)/float(H);
            const float scale=std::min(sx,sy);
            const float drawW=W*scale, drawH=H*scale;
            tour::UiDrawRect2D quad{};
            quad.bounds={{(float(frame.swapExtent.width)-drawW)*0.5f,
                          (float(frame.swapExtent.height)-drawH)*0.5f},
                         {drawW,drawH}};
            quad.texture=texture_;
            ui_.DrawRect(quad);
        }
        const auto flight=frame.fb->frameIndex % smvk::limits::MaxFramesInFlight;
        if(!ui_.AppendPass(ctx,graph,views_[frame.fb->imageIndex],
                           frame.swapExtent,flight,true)){
            std::fprintf(stderr,"GameUiRenderer::AppendPass failed\n");
            host_.RequestClose();return;
        }
        graph.Execute(*frame.fb);
    }
private:
    void FreeSwapchainViews(){
        auto* rg=host_.GetRendergraph();
        if(rg)for(auto view:views_)if(!view.empty())rg->FreeImageView(view);
        views_.clear();
    }
    bool RebindSwapchainViews(){
        auto* sw=host_.GetSwapchain();
        auto* rg=host_.GetRendergraph();
        if(!sw||!rg)return false;
        FreeSwapchainViews();
        for(std::uint32_t i=0;i<sw->GetImageCount();++i)
            views_.push_back(rg->AddExternalImageView(
                sw->GetImageViewHandle(i),sw->GetSurfaceFormat(),
                "Recomp diagnostic swapchain"));
        return !views_.empty();
    }
    tour::App& host_;
    DiagnosticGame game_{};
    std::array<Byte,16> syntheticContent_{};
    tour::recomp::GuestMemory memory_;
    tour::recomp::Eeprom save_;
    tour::recomp::BridgeRunner runner_;
    tour::GameUiRenderer ui_;
    smvk::Holder<smvk::TextureHandle> texture_;
    std::vector<smvk::LogicalImageViewHandle> views_;
    std::string error_;
};
} // namespace

int main(int argc,const char** argv){
    if(argc!=2||std::string(argv[1])!="--synthetic"){
        std::fprintf(stderr,
          "Usage: RecompDiagnostic --synthetic\n"
          "This is a generated pattern, NOT Super Mario 64.\n"
          "A playable SM64 recomp needs an external licensed runtime/RT64 bridge.\n");
        return 2;
    }
    tour::App app;
    app.AddElement(std::make_shared<DiagnosticElement>(app));
    return app.Run()?0:1;
}
