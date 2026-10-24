#include <d3dcompiler.h>
#include <d3d11_1.h>
#include <unordered_map>
#include <ModSettings.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <openvr.h>
#include <aer/ConstantsPool.h>

#include "../VR.hpp"
#include "Fc5Boot.hpp"

#include "D3D11Component.hpp"

#ifdef VERBOSE_D3D11
#define LOG_VERBOSE(...) spdlog::info(__VA_ARGS__)
#else
#define LOG_VERBOSE
#endif

namespace vrmod {
class DX11StateBackup {
public:
    DX11StateBackup(ID3D11DeviceContext* context) {
        if (context == nullptr) {
            throw std::runtime_error("DX11StateBackup: context is null");
        }

        m_context = context;

        m_old.ScissorRectsCount = m_old.ViewportsCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        m_context->RSGetScissorRects(&m_old.ScissorRectsCount, m_old.ScissorRects);
        m_context->RSGetViewports(&m_old.ViewportsCount, m_old.Viewports);
        m_context->RSGetState(&m_old.RS);
        m_context->OMGetBlendState(&m_old.BlendState, m_old.BlendFactor, &m_old.SampleMask);
        m_context->OMGetDepthStencilState(&m_old.DepthStencilState, &m_old.StencilRef);
        m_context->PSGetShaderResources(0, 1, &m_old.PSShaderResource);
        m_context->PSGetSamplers(0, 1, &m_old.PSSampler);
        m_old.PSInstancesCount = m_old.VSInstancesCount = m_old.GSInstancesCount = 256;
        m_context->PSGetShader(&m_old.PS, m_old.PSInstances, &m_old.PSInstancesCount);
        m_context->VSGetShader(&m_old.VS, m_old.VSInstances, &m_old.VSInstancesCount);
        m_context->VSGetConstantBuffers(0, 1, &m_old.VSConstantBuffer);
        m_context->GSGetShader(&m_old.GS, m_old.GSInstances, &m_old.GSInstancesCount);

        m_context->IAGetPrimitiveTopology(&m_old.PrimitiveTopology);
        m_context->IAGetIndexBuffer(&m_old.IndexBuffer, &m_old.IndexBufferFormat, &m_old.IndexBufferOffset);
        m_context->IAGetVertexBuffers(0, 1, &m_old.VertexBuffer, &m_old.VertexBufferStride, &m_old.VertexBufferOffset);
        m_context->IAGetInputLayout(&m_old.InputLayout);
    }

    virtual ~DX11StateBackup() {
        m_context->RSSetScissorRects(m_old.ScissorRectsCount, m_old.ScissorRects);
        m_context->RSSetViewports(m_old.ViewportsCount, m_old.Viewports);
        m_context->RSSetState(m_old.RS); if (m_old.RS) m_old.RS->Release();
        m_context->OMSetBlendState(m_old.BlendState, m_old.BlendFactor, m_old.SampleMask); if (m_old.BlendState) m_old.BlendState->Release();
        m_context->OMSetDepthStencilState(m_old.DepthStencilState, m_old.StencilRef); if (m_old.DepthStencilState) m_old.DepthStencilState->Release();
        m_context->PSSetShaderResources(0, 1, &m_old.PSShaderResource); if (m_old.PSShaderResource) m_old.PSShaderResource->Release();
        m_context->PSSetSamplers(0, 1, &m_old.PSSampler); if (m_old.PSSampler) m_old.PSSampler->Release();
        m_context->PSSetShader(m_old.PS, m_old.PSInstances, m_old.PSInstancesCount); if (m_old.PS) m_old.PS->Release();
        for (UINT i = 0; i < m_old.PSInstancesCount; i++) if (m_old.PSInstances[i]) m_old.PSInstances[i]->Release();
        m_context->VSSetShader(m_old.VS, m_old.VSInstances, m_old.VSInstancesCount); if (m_old.VS) m_old.VS->Release();
        m_context->VSSetConstantBuffers(0, 1, &m_old.VSConstantBuffer); if (m_old.VSConstantBuffer) m_old.VSConstantBuffer->Release();
        m_context->GSSetShader(m_old.GS, m_old.GSInstances, m_old.GSInstancesCount); if (m_old.GS) m_old.GS->Release();
        for (UINT i = 0; i < m_old.VSInstancesCount; i++) if (m_old.VSInstances[i]) m_old.VSInstances[i]->Release();
        m_context->IASetPrimitiveTopology(m_old.PrimitiveTopology);
        m_context->IASetIndexBuffer(m_old.IndexBuffer, m_old.IndexBufferFormat, m_old.IndexBufferOffset); if (m_old.IndexBuffer) m_old.IndexBuffer->Release();
        m_context->IASetVertexBuffers(0, 1, &m_old.VertexBuffer, &m_old.VertexBufferStride, &m_old.VertexBufferOffset); if (m_old.VertexBuffer) m_old.VertexBuffer->Release();
        m_context->IASetInputLayout(m_old.InputLayout); if (m_old.InputLayout) m_old.InputLayout->Release();
    }

private:
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context{};

    struct BACKUP_DX11_STATE {
        UINT                        ScissorRectsCount, ViewportsCount;
        D3D11_RECT                  ScissorRects[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
        D3D11_VIEWPORT              Viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
        ID3D11RasterizerState*      RS;
        ID3D11BlendState*           BlendState;
        FLOAT                       BlendFactor[4];
        UINT                        SampleMask;
        UINT                        StencilRef;
        ID3D11DepthStencilState*    DepthStencilState;
        ID3D11ShaderResourceView*   PSShaderResource;
        ID3D11SamplerState*         PSSampler;
        ID3D11PixelShader*          PS;
        ID3D11VertexShader*         VS;
        ID3D11GeometryShader*       GS;
        UINT                        PSInstancesCount, VSInstancesCount, GSInstancesCount;
        ID3D11ClassInstance         *PSInstances[256], *VSInstances[256], *GSInstances[256];   // 256 is max according to PSSetShader documentation
        D3D11_PRIMITIVE_TOPOLOGY    PrimitiveTopology;
        ID3D11Buffer*               IndexBuffer, *VertexBuffer, *VSConstantBuffer;
        UINT                        IndexBufferOffset, VertexBufferStride, VertexBufferOffset;
        DXGI_FORMAT                 IndexBufferFormat;
        ID3D11InputLayout*          InputLayout;
    } m_old{};
};

D3D11Component::TextureContext::TextureContext(ID3D11Resource* in_tex, std::optional<DXGI_FORMAT> rtv_format, std::optional<DXGI_FORMAT> srv_format) {
    set(in_tex, rtv_format, srv_format);
}

bool D3D11Component::TextureContext::set(ID3D11Resource* in_tex, std::optional<DXGI_FORMAT> rtv_format, std::optional<DXGI_FORMAT> srv_format) {
    bool is_same_tex = this->tex.Get() == in_tex;

    if (!is_same_tex) {
        this->tex.Reset();
    }

    this->tex = in_tex;

    if (in_tex == nullptr) {
        this->rtv.Reset();
        this->srv.Reset();
        return false;
    }
   
    if (!is_same_tex) {
        this->rtv.Reset();
        this->srv.Reset();

        auto device = g_framework->get_d3d11_hook()->get_device();
        bool made_rtv = false;
        bool made_srv = false;

        if (!rtv_format) {
            if (!FAILED(device->CreateRenderTargetView(tex.Get(), nullptr, &rtv))) {
                made_rtv = true;
            }
        }

        if (!srv_format) {  
            if (!FAILED(device->CreateShaderResourceView(tex.Get(), nullptr, &srv))) {
                made_srv = true;
            }
        }

        if (!made_rtv) {
            if (!rtv_format) {
                rtv_format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            }

            D3D11_RENDER_TARGET_VIEW_DESC rtv_desc{};
            rtv_desc.Format = *rtv_format;
            rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            rtv_desc.Texture2D.MipSlice = 0;

            made_rtv = !FAILED(device->CreateRenderTargetView(tex.Get(), &rtv_desc, &this->rtv));
        }

        if (!made_srv) {
            if (!srv_format) {
                srv_format = DXGI_FORMAT_R8G8B8A8_UNORM;
            }

            D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
            srv_desc.Format = *srv_format;
            srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srv_desc.Texture2D.MipLevels = 1;
            srv_desc.Texture2D.MostDetailedMip = 0;

            made_srv = !FAILED(device->CreateShaderResourceView(tex.Get(), &srv_desc, &this->srv));
        }

        if (!made_rtv) {
            spdlog::error("Failed to create render target view for texture");
        }

        if (!made_srv) {
            spdlog::error("Failed to create shader resource view for texture");
        }

        return made_rtv && made_srv;
    }

    return true;
}

bool D3D11Component::TextureContext::clear_rtv(float* color) {
    if (tex == nullptr || rtv == nullptr) {
        return false;
    }

    if (color == nullptr) {
        return false;
    }

    auto device = g_framework->get_d3d11_hook()->get_device();

    ComPtr<ID3D11DeviceContext> context{};
    device->GetImmediateContext(&context);

    context->ClearRenderTargetView(rtv.Get(), color);
    return true;
}

vr::EVRCompositorError D3D11Component::on_frame(VR* vr) {
    if (m_left_eye_tex == nullptr) {
        if (!setup()) {
            return vr::VRCompositorError_None;
        }
    }

    auto& hook = g_framework->get_d3d11_hook();

    // get device
    auto device = hook->get_device();

    // Get the context.
    ComPtr<ID3D11DeviceContext> context{};

    device->GetImmediateContext(&context);

    // get swapchain
    auto swapchain = hook->get_swap_chain();

    // get back buffer
    ComPtr<ID3D11Texture2D> backbuffer{};

    swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer));

    if (backbuffer == nullptr) {
        spdlog::error("[VR] Failed to get back buffer.");
        return vr::VRCompositorError_None;
    }

    // Copy the backbuffer into our SRV compatible texture.
    bool backbufferIs8Bit = m_backbuffer_is_8bit && ModSettings::g_internalSettings.toneMapAlg == 0;
    if (!backbufferIs8Bit) {
        context->CopyResource(m_backbuffer_copy_tex.Get(), backbuffer.Get());
    }

    auto runtime = vr->get_runtime();
    bool is_left_eye_frame = vr->m_presenter_frame_count % 2 == vr->m_left_eye_interval;
    bool is_right_eye_frame = !is_left_eye_frame;

    // Duplicate frames can sometimes cause the UI to get stuck on the screen.
    // and can lock up the compositor.
    if (runtime->is_openxr() && vr->m_openxr->frame_began) {
        if (is_right_eye_frame) {
            auto fw_rt = g_framework->get_rendertarget_d3d11();

            if (fw_rt != nullptr && g_framework->is_drawing_ui() && !vr->m_fc5_f11_image_active.load()) {
                m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::FRAMEWORK_UI, fw_rt.Get());
            }
        }
    }

    ComPtr<ID3D11Texture2D> scene_depth_tex{};
//
//    if (vr->is_depth_enabled() && runtime->is_depth_allowed()) {
//        auto& rt_pool = vr->get_render_target_pool_hook();
//        scene_depth_tex = rt_pool->get_texture<ID3D11Texture2D>(L"SceneDepthZ");
//
//        if (scene_depth_tex != nullptr) {
//            D3D11_TEXTURE2D_DESC desc{};
//            scene_depth_tex->GetDesc(&desc);
//
//            if (runtime->is_openxr()) {
//                if (vr->m_openxr->needs_depth_resize(desc.Width, desc.Height) || m_openxr.made_depth_with_null_defaults) {
//                    spdlog::info("[OpenXR] Depth size changed, recreating swapchains [{}x{}]", desc.Width, desc.Height);
//                    m_openxr.create_swapchains(); // recreate swapchains to match the new depth size
//                }
//            }
//        }
//    }


    // If m_frame_count is even, we're rendering the left eye.

    if (is_left_eye_frame) {
        auto copy_from_tex = backbufferIs8Bit ? backbuffer : m_left_eye_rt.tex;

        // HDR compatible path. If backbuffer is 8bit, we just copy from that instead.
        // If not, then we use SpriteBatch to convert the texture to 8bit.
        if (!backbufferIs8Bit && m_left_eye_rt.has_srv()) {
            DX11StateBackup backup{context.Get()};

            ID3D11RenderTargetView* views[] = { m_left_eye_rt };
            context->OMSetRenderTargets(1, views, nullptr);

            D3D11_VIEWPORT viewport{};
            viewport.Width = m_backbuffer_size[0];
            viewport.Height = m_backbuffer_size[1];
            context->RSSetViewports(1, &viewport);

            D3D11_RECT scissor_rect{};
            scissor_rect.left = 0;
            scissor_rect.top = 0;
            scissor_rect.right = m_backbuffer_size[0];
            scissor_rect.bottom = m_backbuffer_size[1];
            context->RSSetScissorRects(1, &scissor_rect);

            ModSettings::InternalSettings& settings = ModSettings::g_internalSettings;
            m_toneMap->SetOperator(static_cast<DirectX::ToneMapPostProcess::Operator>(settings.toneMapAlg));
            m_toneMap->SetExposure(settings.toneMapExposure);
            m_toneMap->SetHDRSourceTexture(m_backbuffer_copy_rt);
            m_toneMap->Process(context.Get());
        } else if (backbufferIs8Bit && !(vr->m_fc5_skip_bb_copy.load() && vr->m_fc5_eye_override && !vr->is_using_async_aer())) {
            context->CopyResource(m_left_eye_tex.Get(), backbuffer.Get());
        }

        if (runtime->is_openxr() && runtime->ready()) {
            LOG_VERBOSE("Copying left eye");
            // Far Cry 5 DLSS upscaling: the eye image composed at the eye's size replaces the back buffer.
            m_openxr.copy(0, vr->m_fc5_eye_override ? vr->m_fc5_eye_override : (ID3D11Texture2D*)copy_from_tex.Get());
            {
                // Bring-up probe (Far Cry 5): the centre pixel of the game image and of what goes to the left eye.
                static auto last = std::chrono::steady_clock::now();
                if (!vr->m_fc5_no_eye_probe.load() && std::chrono::steady_clock::now() - last > std::chrono::seconds(5)) {
                    last = std::chrono::steady_clock::now();
                    auto probe =[&](ID3D11Texture2D* t) -> std::string {
                        if (!t) return "null";
                        D3D11_TEXTURE2D_DESC d{};
                        t->GetDesc(&d);
                        D3D11_TEXTURE2D_DESC s{};
                        s.Width = 1; s.Height = 1; s.MipLevels = 1; s.ArraySize = 1; s.Format = d.Format; s.SampleDesc.Count = 1;
                        s.Usage = D3D11_USAGE_STAGING; s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                        ComPtr<ID3D11Texture2D> st{};
                        if (FAILED(g_framework->get_d3d11_hook()->get_device()->CreateTexture2D(&s, nullptr, &st))) return "staging refused";
                        D3D11_BOX box{ d.Width / 2, d.Height / 2, 0, d.Width / 2 + 1, d.Height / 2 + 1, 1 };
                        context->CopySubresourceRegion(st.Get(), 0, 0, 0, 0, t, 0, &box);
                        D3D11_MAPPED_SUBRESOURCE m{};
                        if (FAILED(context->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) return "map refused";
                        const uint32_t v = *static_cast<const uint32_t*>(m.pData);
                        context->Unmap(st.Get(), 0);
                        return fmt::format("{}x{} fmt {} centre {:08x}", d.Width, d.Height, (int)d.Format, v);
                    };
                    spdlog::info("[VR] eye probe: backbuffer {} | left eye source {} | 8-bit path {}", probe(backbuffer.Get()), probe((ID3D11Texture2D*)copy_from_tex.Get()),
                                 backbufferIs8Bit);
                }
            }
            if (vr->is_using_async_aer()) {
                ID3D11Texture2D* fresh_tex = vr->m_fc5_eye_override ? vr->m_fc5_eye_override : (ID3D11Texture2D*)copy_from_tex.Get();
                // Far Cry 5 AFW without a held image yet (just switched on): the fresh image for both eyes, never an unwritten eye copy.
                auto other_eye_tex = vr->m_afw_held_texture ? vr->m_afw_held_texture : vr->m_fc5_afw_active.load() ? fresh_tex : (backbufferIs8Bit ? m_right_eye_tex.Get() : (ID3D11Texture2D*)m_right_eye_rt.tex.Get());
                m_openxr.copy(1, other_eye_tex);
            }
        }

        if (runtime->is_openvr()) {
            vr::VRTextureWithPose_t left_eye{};
            left_eye.handle = (void*)m_left_eye_tex.Get();
            left_eye.eType = vr::TextureType_DirectX;
            left_eye.eColorSpace = vr::ColorSpace_Auto;
            left_eye.mDeviceToAbsoluteTracking = GlobalPool::get_openvr_pose(vr->m_presenter_frame_count);

            auto e = vr::VRCompositor()->Submit(vr::Eye_Left, (vr::Texture_t*)&left_eye, &vr->m_left_bounds, vr::Submit_TextureWithPose);

            if (e != vr::VRCompositorError_None) {
                spdlog::error("[VR] VRCompositor failed to submit left eye: {}", (int)e);
                vr->m_submitted = false;
                return e;
            }

            if (vr->is_using_async_aer()) {
                vr::VRTextureWithPose_t right_eye{};
                right_eye.handle = (void*)m_right_eye_tex.Get();
                right_eye.eType = vr::TextureType_DirectX;
                right_eye.eColorSpace = vr::ColorSpace_Auto;
                right_eye.mDeviceToAbsoluteTracking = GlobalPool::get_openvr_pose(vr->m_presenter_frame_count - 1);

                e = vr::VRCompositor()->Submit(vr::Eye_Right, (vr::Texture_t*)&right_eye, &vr->m_right_bounds, vr::Submit_TextureWithPose);

                if (e != vr::VRCompositorError_None) {
                    spdlog::error("[VR] VRCompositor failed to submit right eye (resubmit): {}", (int)e);
                }
                vr->m_submitted = true;
            }
        }
    } else {
        auto copy_from_tex = backbufferIs8Bit ? backbuffer : m_right_eye_rt.tex;

        // HDR compatible path. If backbuffer is 8bit, we just copy from that instead.
        // If not, then we use SpriteBatch to convert the texture to 8bit.
        if (!backbufferIs8Bit && m_right_eye_rt.has_srv()) {
            DX11StateBackup backup{context.Get()};

            ID3D11RenderTargetView* views[] = { m_right_eye_rt };
            context->OMSetRenderTargets(1, views, nullptr);

            D3D11_VIEWPORT viewport{};
            viewport.Width = m_backbuffer_size[0];
            viewport.Height = m_backbuffer_size[1];
            context->RSSetViewports(1, &viewport);

            D3D11_RECT scissor_rect{};
            scissor_rect.left = 0;
            scissor_rect.top = 0;
            scissor_rect.right = m_backbuffer_size[0];
            scissor_rect.bottom = m_backbuffer_size[1];
            context->RSSetScissorRects(1, &scissor_rect);


            ModSettings::InternalSettings& settings = ModSettings::g_internalSettings;
            m_toneMap->SetOperator(static_cast<DirectX::ToneMapPostProcess::Operator>(settings.toneMapAlg));
            m_toneMap->SetExposure(settings.toneMapExposure);
            m_toneMap->SetHDRSourceTexture(m_backbuffer_copy_rt);
            m_toneMap->Process(context.Get());
        } else if (backbufferIs8Bit && !(vr->m_fc5_skip_bb_copy.load() && vr->m_fc5_eye_override && !vr->is_using_async_aer())) {
            context->CopyResource(m_right_eye_tex.Get(), backbuffer.Get());
        }

        if (runtime->ready() && runtime->is_openxr()) {
            LOG_VERBOSE("Copying right eye");
            // Far Cry 5 DLSS upscaling: the eye image composed at the eye's size replaces the back buffer.
            m_openxr.copy(1, vr->m_fc5_eye_override ? vr->m_fc5_eye_override : (ID3D11Texture2D*)copy_from_tex.Get());
            if (vr->is_using_async_aer()) {
                ID3D11Texture2D* fresh_tex = vr->m_fc5_eye_override ? vr->m_fc5_eye_override : (ID3D11Texture2D*)copy_from_tex.Get();
                auto other_eye_tex = vr->m_afw_held_texture ? vr->m_afw_held_texture : vr->m_fc5_afw_active.load() ? fresh_tex : (backbufferIs8Bit ? m_left_eye_tex.Get() : (ID3D11Texture2D*)m_left_eye_rt.tex.Get());
                m_openxr.copy(0, other_eye_tex);
            }
        }

        if (runtime->is_openvr()) {
            if (vr->is_using_async_aer()) {
                vr::VRTextureWithPose_t left_eye{};
                left_eye.handle = (void*)m_left_eye_tex.Get();
                left_eye.eType = vr::TextureType_DirectX;
                left_eye.eColorSpace = vr::ColorSpace_Auto;
                left_eye.mDeviceToAbsoluteTracking = GlobalPool::get_openvr_pose(vr->m_presenter_frame_count - 1);

                auto e = vr::VRCompositor()->Submit(vr::Eye_Left, (vr::Texture_t*)&left_eye, &vr->m_left_bounds, vr::Submit_TextureWithPose);

                if (e != vr::VRCompositorError_None) {
                    spdlog::error("[VR] VRCompositor failed to submit left eye (resubmit): {}", (int)e);
                }
            }
            // Copy the back buffer to the right eye texture.
            if (backbufferIs8Bit) {
                context->CopyResource(m_right_eye_tex.Get(), backbuffer.Get());
            }
            
            vr::VRTextureWithPose_t right_eye{};
            right_eye.handle = (void*)m_right_eye_tex.Get();
            right_eye.eType = vr::TextureType_DirectX;
            right_eye.eColorSpace = vr::ColorSpace_Auto;
            right_eye.mDeviceToAbsoluteTracking = GlobalPool::get_openvr_pose(vr->m_presenter_frame_count);

            auto e = vr::VRCompositor()->Submit(vr::Eye_Right, (vr::Texture_t*)&right_eye, &vr->m_right_bounds, vr::Submit_TextureWithPose);

            if (e != vr::VRCompositorError_None) {
                spdlog::error("[VR] VRCompositor failed to submit right eye: {}", (int)e);
                vr->m_submitted = false;
                return e;
            }

            vr->m_submitted = true;
        }

    }

    if (is_right_eye_frame || vr->is_using_async_aer()) {
        if (runtime->ready()) {
            if (runtime->get_synchronize_stage() == VRRuntime::SynchronizeStage::VERY_LATE || !runtime->got_first_sync) {
                runtime->synchronize_frame(vr->m_presenter_frame_count);

                if (!runtime->got_first_poses) {
                    runtime->update_poses(vr->m_presenter_frame_count + 1);
                }
            }
        }

        if (runtime->is_openxr() && vr->m_openxr->ready()) {
            if (runtime->get_synchronize_stage() == VRRuntime::SynchronizeStage::VERY_LATE || !vr->m_openxr->frame_began) {
                LOG_VERBOSE("Beginning frame.");
                vr->m_openxr->begin_frame(vr->m_presenter_frame_count);
            }

            LOG_VERBOSE("Ending frame");
            std::vector<XrCompositionLayerBaseHeader*> quad_layers{};

            // Far Cry 5: the game's HUD on a head-locked panel (premultiplied alpha), under the F11 panel.
            static XrCompositionLayerQuad hud_layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
            // The HUD layer is submitted on every frame once it has content: a frame without a fresh HUD
            // image reuses the last one instead of dropping the layer (a layer that comes and goes from one
            // frame to the next made the whole headset image blink black on Virtual Desktop).
            static int hud_last_fresh = -1000000;
            VR::s_fc5_end_frames++;
            const auto hud_index = (uint32_t)runtimes::OpenXR::SwapchainIndex::GAME_HUD;
            if (vr->m_fc5_hud_texture) {
                m_openxr.copy(hud_index, vr->m_fc5_hud_texture);
                hud_last_fresh = vr->m_presenter_frame_count;
                D3D11_TEXTURE2D_DESC hd{};
                vr->m_fc5_hud_texture->GetDesc(&hd);
                const float aspect = hd.Height > 0 ? (float)hd.Width / (float)hd.Height : 1.0f;
                hud_layer.size = {vr->m_fc5_hud_width, vr->m_fc5_hud_width / aspect};
            }
            if (vr->m_presenter_frame_count - hud_last_fresh < 120 && vr->m_fc5_hud_width > 0.0f) {
                const auto& sc = vr->m_openxr->swapchains[hud_index];
                hud_layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                hud_layer.space = vr->m_openxr->view_space;
                hud_layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                hud_layer.subImage.swapchain = sc.handle;
                hud_layer.subImage.imageRect.offset = {0, 0};
                hud_layer.subImage.imageRect.extent = {(int32_t)sc.width, (int32_t)sc.height};
                hud_layer.pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
                hud_layer.pose.position = {0.0f, vr->m_fc5_hud_height, -vr->m_fc5_hud_distance};
                if (sc.handle != XR_NULL_HANDLE && hud_layer.size.width > 0.0f) {
                    quad_layers.push_back((XrCompositionLayerBaseHeader*)&hud_layer);
                    VR::s_fc5_hud_submits++;
                    static int hud_logs = 0;
                    if (hud_logs++ % 900 == 0) {
                        spdlog::info("[VR] HUD quad layer submitted: {}x{} swapchain, {:.2f} x {:.2f} m at (0, {:.2f}, {:.2f}) in view space", sc.width, sc.height,
                                     hud_layer.size.width, hud_layer.size.height, hud_layer.pose.position.y, hud_layer.pose.position.z);
                    }
                }
            }
            auto& openxr_overlay = vr->get_overlay_component().get_openxr();

            // Far Cry 5: "f11layer keep" (the F11 layer kept in the list while closed, cleared to transparent) was removed 24 Oct
            // (Homard: the black blink is gone without it).
            // Far Cry 5 (f11image on): the menu is drawn into the eye images by the mod, from the quad worked out here
            // (which also does the controller pointing); no layer is sent.
            const bool f11_image = vr->m_fc5_f11_image_active.load();
            vr->m_fc5_f11_quad_valid = false;
            if (m_openxr.ever_acquired((uint32_t)runtimes::OpenXR::SwapchainIndex::FRAMEWORK_UI) || f11_image) {
                const auto framework_quad = openxr_overlay.generate_framework_ui_quad(false);

                if (framework_quad) {
                    if (g_framework->is_drawing_ui()) {
                        const auto& q = framework_quad->get();
                        const float v[9]{ q.pose.position.x, q.pose.position.y, q.pose.position.z, q.pose.orientation.x, q.pose.orientation.y,
                                          q.pose.orientation.z, q.pose.orientation.w, q.size.width, q.size.height };
                        std::memcpy(vr->m_fc5_f11_quad, v, sizeof(v));
                        vr->m_fc5_f11_quad_view  = q.space == vr->m_openxr->view_space;
                        vr->m_fc5_f11_quad_valid = true;
                    }
                    if (!f11_image) {
                        quad_layers.push_back((XrCompositionLayerBaseHeader*)&framework_quad->get());
                    }
                }
            }

            static const bool simulator = [] {
                char rt[512]{};
                const DWORD n = GetEnvironmentVariableA("XR_RUNTIME_JSON", rt, sizeof(rt));
                std::string s{ rt, n < sizeof(rt) ? n : 0 };
                for (auto& c : s) c = (char)tolower((unsigned char)c);
                const bool sim = s.find("simulator") != std::string::npos;
                spdlog::info("[VR] runtime is the Meta XR Simulator: {} (flush before xrEndFrame there: xrflushsim)", sim);
                return sim;
            }();
            if (vr->m_fc5_xr_flush.load() || (simulator && vr->m_fc5_xr_flush_sim.load())) {
                context->Flush();
            }
            auto result = vr->m_openxr->end_frame(quad_layers, vr->m_presenter_frame_count, scene_depth_tex != nullptr);

            vr->m_openxr->needs_pose_update = true;
            vr->m_submitted = result == XR_SUCCESS;
            if (result == XR_SUCCESS) {
                fc5_boot_vr_running();
            }
        }

        // Desktop spectator view
        if (runtime->ready() && vr->m_desktop_fix->value() && is_right_eye_frame) {
            if (vr->m_desktop_fix_skip_present->value()) {
                hook->ignore_next_present();
            } else if (backbufferIs8Bit) {
                context->CopyResource(backbuffer.Get(), m_left_eye_tex.Get());
            } else {
                DX11StateBackup backup{context.Get()};

                ID3D11RenderTargetView* views[] = { m_backbuffer_rtv.Get() };
                context->OMSetRenderTargets(1, views, nullptr);

                m_sprite_batch->Begin();

                D3D11_VIEWPORT viewport{};
                viewport.Width = m_backbuffer_size[0];
                viewport.Height = m_backbuffer_size[1];
                m_sprite_batch->SetViewport(viewport);

                context->RSSetViewports(1, &viewport);

                D3D11_RECT scissor_rect{};
                scissor_rect.left = 0;
                scissor_rect.top = 0;
                scissor_rect.right = m_backbuffer_size[0];
                scissor_rect.bottom = m_backbuffer_size[1];

                context->RSSetScissorRects(1, &scissor_rect);

                RECT dest_rect{};
                dest_rect.left = 0;
                dest_rect.top = 0;
                dest_rect.right = m_backbuffer_size[0];
                dest_rect.bottom = m_backbuffer_size[1];

                m_sprite_batch->Draw(m_left_eye_rt, DirectX::Colors::White);

                m_sprite_batch->End();
            }
        }
    }

    return vr::VRCompositorError_None;
}

void D3D11Component::on_reset(VR* vr) {
    m_backbuffer_rtv.Reset();
    m_backbuffer_copy_rt.reset();
    m_backbuffer_copy_tex.Reset();
    m_left_eye_rt.reset();
    m_right_eye_rt.reset();
    m_left_eye_tex.Reset();
    m_right_eye_tex.Reset();
    m_left_eye_depthstencil.Reset();
    m_right_eye_depthstencil.Reset();
    m_sprite_batch.reset();
    m_toneMap.reset();

    if (vr->get_runtime()->is_openxr() && vr->get_runtime()->loaded) {
        // Far Cry 5: the eye images and the HUD panel also follow the mod's sizes, which change on a live resolution change.
        const auto& sc = vr->m_openxr->swapchains;
        const auto  eye = (size_t)runtimes::OpenXR::SwapchainIndex::AFR_LEFT_EYE, hud = (size_t)runtimes::OpenXR::SwapchainIndex::GAME_HUD;
        const bool  fc5_sizes = VR::s_fc5_eye_size[0] != 0 && sc[eye].handle != XR_NULL_HANDLE &&
                               (sc[eye].width != (int)VR::s_fc5_eye_size[0] || sc[eye].height != (int)VR::s_fc5_eye_size[1] ||
                                (VR::s_fc5_hud_full_res && VR::s_fc5_hud_res[0] && (sc[hud].width != (int)VR::s_fc5_hud_res[0] || sc[hud].height != (int)VR::s_fc5_hud_res[1])));
        if (m_openxr.last_resolution[0] != vr->get_hmd_width() || m_openxr.last_resolution[1] != vr->get_hmd_height() || fc5_sizes) {
            spdlog::info("[VR] reset: OpenXR swapchains recreated (eye images {}x{})", VR::s_fc5_eye_size[0], VR::s_fc5_eye_size[1]);
            m_openxr.create_swapchains();
        }
    }
}

bool D3D11Component::setup() {
    // Get device and swapchain.
    auto& hook = g_framework->get_d3d11_hook();
    auto device = hook->get_device();
    auto swapchain = hook->get_swap_chain();

    // Get back buffer.
    ComPtr<ID3D11Texture2D> backbuffer{};

    if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer)))) {
        spdlog::error("[VR] Failed to get backbuffer (D3D11).");
        return false;
    }

    // Get backbuffer description.
    D3D11_TEXTURE2D_DESC backbuffer_desc{};

    backbuffer->GetDesc(&backbuffer_desc);
    backbuffer_desc.BindFlags |= D3D11_BIND_RENDER_TARGET;
    backbuffer_desc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;

    m_backbuffer_is_8bit = backbuffer_desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM;

    // Create raw RTV of backbuffer.
    if (FAILED(device->CreateRenderTargetView(backbuffer.Get(), nullptr, &m_backbuffer_rtv))) {
        spdlog::error("[VR] Failed to create backbuffer raw render target (D3D11).");
        return false;
    }

    // Create a copy of the backbuffer that has SRV and RTV bind flags so we can pass it to the sprite batch.
    if (FAILED(device->CreateTexture2D(&backbuffer_desc, nullptr, &m_backbuffer_copy_tex))) {
        spdlog::error("[VR] Failed to create backbuffer copy (D3D11).");
        return false;
    }

    if (!m_backbuffer_copy_rt.set(m_backbuffer_copy_tex.Get(), std::nullopt, std::nullopt)) {
        spdlog::error("[VR] Failed to create backbuffer render target (D3D11).");
        return false;
    }

    backbuffer_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;

    m_backbuffer_size[0] = backbuffer_desc.Width;
    m_backbuffer_size[1] = backbuffer_desc.Height;

    // Create eye textures.
    if (FAILED(device->CreateTexture2D(&backbuffer_desc, nullptr, &m_left_eye_tex))) {
        spdlog::error("[VR] Failed to create left eye texture (D3D11).");
        return false;
    }

    if (FAILED(device->CreateTexture2D(&backbuffer_desc, nullptr, &m_right_eye_tex))) {
        spdlog::error("[VR] Failed to create right eye texture (D3D11).");
        return false;
    }

    if (!m_left_eye_rt.set(m_left_eye_tex.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM)) {
        spdlog::error("[VR] Failed to create left eye render target (D3D11).");
        return false;
    }

    if (!m_right_eye_rt.set(m_right_eye_tex.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM)) {
        spdlog::error("[VR] Failed to create right eye render target (D3D11).");
        return false;
    }

    // copy backbuffer into right eye
    // Get the context.
    ComPtr<ID3D11DeviceContext> context{};

    device->GetImmediateContext(&context);
    //context->CopyResource(m_right_eye_tex.Get(), backbuffer.Get());

    // Make depth stencils for both eyes.
    auto depthstencil = hook->get_last_depthstencil_used();

    if (depthstencil != nullptr) {
        D3D11_TEXTURE2D_DESC depthstencil_desc{};

        depthstencil->GetDesc(&depthstencil_desc);

        // Create eye depthstencils.
        device->CreateTexture2D(&depthstencil_desc, nullptr, &m_left_eye_depthstencil);
        device->CreateTexture2D(&depthstencil_desc, nullptr, &m_right_eye_depthstencil);

        // Copy the current depthstencil into the right eye.
        context->CopyResource(m_right_eye_depthstencil.Get(), depthstencil.Get());
    }

    m_sprite_batch = std::make_unique<DirectX::DX11::SpriteBatch>(context.Get());
    m_toneMap = std::make_unique<DirectX::DX11::ToneMapPostProcess>(device);

    spdlog::info("[VR] d3d11 textures have been setup");

    return true;
}

void D3D11Component::OpenXR::initialize(XrSessionCreateInfo& session_info) {
    std::scoped_lock _{this->mtx};

    auto& hook = g_framework->get_d3d11_hook();

    auto device = hook->get_device();

    this->binding.device = device;

    PFN_xrGetD3D11GraphicsRequirementsKHR fn = nullptr;
    xrGetInstanceProcAddr(VR::get()->m_openxr->instance, "xrGetD3D11GraphicsRequirementsKHR", (PFN_xrVoidFunction*)(&fn));

    if (fn == nullptr) {
        spdlog::error("[VR] xrGetD3D11GraphicsRequirementsKHR not found");
        return;
    }

    // get existing adapter from device
    ComPtr<IDXGIDevice> dxgi_device{};
    
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi_device)))) {
        spdlog::error("[VR] failed to get DXGI device from D3D11 device");
        return;
    }
    
    ComPtr<IDXGIAdapter> adapter{};

    if (FAILED(dxgi_device->GetAdapter(&adapter))) {
        spdlog::error("[VR] failed to get DXGI adapter from DXGI device");
        return;
    }

    DXGI_ADAPTER_DESC desc{};

    if (FAILED(adapter->GetDesc(&desc))) {
        spdlog::error("[VR] failed to get DXGI adapter description");
        return;
    }
    
    // The runtime says which GPU the headset needs (an output): on a PC with two GPUs the game may run on the other
    // one, and the session then fails or shows black.
    XrGraphicsRequirementsD3D11KHR gr{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    fc5_boot_step("OpenXR: graphics requirements");
    const XrResult gr_result = fn(VR::get()->m_openxr->instance, VR::get()->m_openxr->system, &gr);
    fc5_boot_result("OpenXR: graphics requirements", gr_result);
    char game_gpu[128]{};
    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, game_gpu, sizeof(game_gpu) - 1, nullptr, nullptr);
    spdlog::info("[VR] the game's GPU: {} (LUID {:08x}:{:08x}); the runtime needs LUID {:08x}:{:08x}, feature level 0x{:x} (result {})", game_gpu,
                 (uint32_t)desc.AdapterLuid.HighPart, desc.AdapterLuid.LowPart, (uint32_t)gr.adapterLuid.HighPart, gr.adapterLuid.LowPart, (int)gr.minFeatureLevel,
                 (int)gr_result);
    if (gr_result == XR_SUCCESS && (gr.adapterLuid.HighPart != desc.AdapterLuid.HighPart || gr.adapterLuid.LowPart != desc.AdapterLuid.LowPart)) {
        spdlog::error("[VR] the game runs on a different GPU ({}) from the one the headset is connected to: set Far Cry 5 to the headset's GPU "
                      "(Windows Settings > Display > Graphics), or the VR session may fail or stay black",
                      game_gpu);
    }

    session_info.next = &this->binding;
}

std::optional<std::string> D3D11Component::OpenXR::create_swapchains() {
    std::scoped_lock _{this->mtx};

    spdlog::info("[VR] Creating OpenXR swapchains for D3D11");

    this->destroy_swapchains();

    auto& hook = g_framework->get_d3d11_hook();
    auto device = hook->get_device();
    auto  lDxgiSwapchain = hook->get_swap_chain();

    // Get back buffer.
    ComPtr<ID3D11Texture2D> backbuffer{};

    lDxgiSwapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer));

    if (backbuffer == nullptr) {
        spdlog::error("[VR] Failed to get back buffer.");
        return "Failed to get back buffer.";
    }

    // Get backbuffer description.
    D3D11_TEXTURE2D_DESC backbuffer_desc{};
    backbuffer->GetDesc(&backbuffer_desc);

    backbuffer_desc.BindFlags |= D3D11_BIND_RENDER_TARGET;
    backbuffer_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;

    auto& vr = VR::get();
    auto& openxr = vr->m_openxr;

    uint32_t ui_textture_size[2] = {backbuffer_desc.Width, backbuffer_desc.Height};
    backbuffer_desc.Width = vr->get_hmd_width();
    backbuffer_desc.Height = vr->get_hmd_height();

    this->contexts.clear();
    //TODO clean up this mess
    this->contexts.resize((size_t)runtimes::OpenXR::SwapchainIndex::END);
    auto create_swapchain = [&](uint32_t swapchainIndex, int format, int width, int height) -> std::optional<std::string> {
        spdlog::info("[VR] Creating swapchain for eye {}", swapchainIndex);
        spdlog::info("[VR] Width: {}", width);
        spdlog::info("[VR] Height: {}", height);
        spdlog::info("[VR] Format: {}", format);
        XrSwapchainCreateInfo swapchain_create_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        swapchain_create_info.arraySize = 1;
        swapchain_create_info.format = format;
        swapchain_create_info.width = width;
        swapchain_create_info.height = height;
        swapchain_create_info.mipCount = 1;
        swapchain_create_info.faceCount = 1;
        swapchain_create_info.sampleCount = backbuffer_desc.SampleDesc.Count;
        swapchain_create_info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        auto& swapchain = vr->m_openxr->swapchains[swapchainIndex];
        swapchain.width = swapchain_create_info.width;
        swapchain.height = swapchain_create_info.height;
        if (xrCreateSwapchain(openxr->session, &swapchain_create_info, &swapchain.handle) != XR_SUCCESS) {
            spdlog::error("[VR] D3D11: Failed to create swapchain.");
            return "Failed to create swapchain.";
        }
        uint32_t image_count{};
        auto result = xrEnumerateSwapchainImages(swapchain.handle, 0, &image_count, nullptr);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] Failed to enumerate swapchain images.");
            return "Failed to enumerate swapchain images.";
        }

        spdlog::info("[VR] Runtime wants {} images for swapchain {}", image_count, swapchainIndex);

        auto& ctx = this->contexts[swapchainIndex];

        ctx.textures.clear();
        ctx.textures.resize(image_count);
        ctx.swapchain_index = swapchainIndex;

        for (uint32_t j = 0; j < image_count; ++j) {
            ctx.textures[j] = {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR};
        }

        result = xrEnumerateSwapchainImages(swapchain.handle, image_count, &image_count, (XrSwapchainImageBaseHeader*)&ctx.textures[0]);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] Failed to enumerate swapchain images after texture creation.");
            return "Failed to enumerate swapchain images after texture creation.";
        }
        return std::nullopt;
    };

    if(openxr->get_view_count() != 2) {
        spdlog::error("[VR] Unsupported view count: {}", openxr->get_view_count());
        return "Unsupported view count.";
    }

    if(auto err = create_swapchain((int)runtimes::OpenXR::SwapchainIndex::AFR_LEFT_EYE, (int)DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, VR::s_fc5_eye_size[0] ? VR::s_fc5_eye_size[0] : backbuffer_desc.Width, VR::s_fc5_eye_size[0] ? VR::s_fc5_eye_size[1] : backbuffer_desc.Height)) {
        return err;
    }
    if(auto err = create_swapchain((int)runtimes::OpenXR::SwapchainIndex::AFR_RIGHT_EYE, (int)DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, VR::s_fc5_eye_size[0] ? VR::s_fc5_eye_size[0] : backbuffer_desc.Width, VR::s_fc5_eye_size[0] ? VR::s_fc5_eye_size[1] : backbuffer_desc.Height)) {
        return err;
    }
    for (auto idx : {runtimes::OpenXR::SwapchainIndex::AFR_LEFT_EYE_B, runtimes::OpenXR::SwapchainIndex::AFR_RIGHT_EYE_B}) {
        if (auto err = create_swapchain((int)idx, (int)DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, VR::s_fc5_eye_size[0] ? VR::s_fc5_eye_size[0] : backbuffer_desc.Width,
                                        VR::s_fc5_eye_size[0] ? VR::s_fc5_eye_size[1] : backbuffer_desc.Height)) {
            return err;
        }
    }
    if(auto err = create_swapchain((int)runtimes::OpenXR::SwapchainIndex::FRAMEWORK_UI, (int)DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, backbuffer_desc.Width, backbuffer_desc.Height)) {
        return err;
    }
    {
        // Far Cry 5 HUD panel (report B1, "hudfullres on"): the panel image at the HUD texture's own size (the game render
        // size), so the copy is 1:1 instead of a one-tap shrink to half the back buffer.
        const bool full = VR::s_fc5_hud_full_res && VR::s_fc5_hud_res[0] > 0 && VR::s_fc5_hud_res[1] > 0;
        const uint32_t hw = full ? VR::s_fc5_hud_res[0] : std::max(1u, backbuffer_desc.Width / 2);
        const uint32_t hh = full ? VR::s_fc5_hud_res[1] : std::max(1u, backbuffer_desc.Height / 2);
        spdlog::info("[VR] HUD panel swapchain {}x{}{}", hw, hh, full ? " (full HUD resolution)" : "");
        if(auto err = create_swapchain((int)runtimes::OpenXR::SwapchainIndex::GAME_HUD, (int)DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, hw, hh)) {
            return err;
        }
    }

    this->last_resolution = {vr->get_hmd_width(), vr->get_hmd_height()};

    spdlog::info("[VR] Successfully created OpenXR swapchains for D3D11");

    return std::nullopt;
}

void D3D11Component::OpenXR::destroy_swapchains() {
    std::scoped_lock _{this->mtx};

	if (this->contexts.empty()) {
        return;
    }

    spdlog::info("[VR] Destroying swapchains.");

    for (auto i = 0; i < this->contexts.size(); ++i) {
        auto& ctx = this->contexts[i];

        if(VR::get()->m_openxr->swapchains[i].handle) {
            auto result = xrDestroySwapchain(VR::get()->m_openxr->swapchains[i].handle);
            if (result != XR_SUCCESS) {
                spdlog::error("[VR] Failed to destroy swapchain {}.", i);
            } else {
                spdlog::info("[VR] Destroyed swapchain {}.", i);
            }
        }
        ctx.textures.clear();
        VR::get()->m_openxr->swapchains[ctx.swapchain_index] = {};
    }

    this->contexts.clear();
}

namespace {
// The game's image to an eye swapchain image of another size (CopyResource needs equal sizes and
// silently does nothing otherwise: the headset stayed black in Far Cry 5, 1920x1080 into 2688x2880).
// A fullscreen triangle samples the source bilinearly; an sRGB target gets the decoded value so the
// stored bits match what a raw copy would have stored.
struct StretchBlit {
    ComPtr<ID3D11VertexShader> vs{};
    ComPtr<ID3D11PixelShader> ps_raw{}, ps_srgb{};
    ComPtr<ID3D11SamplerState> samp{};
    ComPtr<ID3DDeviceContextState> state{};
    ComPtr<ID3D11Texture2D> src_copy{};
    ComPtr<ID3D11ShaderResourceView> src_srv{};
    D3D11_TEXTURE2D_DESC src_desc{};
    std::unordered_map<ID3D11Texture2D*, ComPtr<ID3D11RenderTargetView>> rtvs{};
    bool failed{false};

    bool init(ID3D11Device* dev) {
        if (vs || failed) return vs != nullptr;
        const char* src =
            "struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
            "V vsmain(uint id : SV_VertexID) { V o; float2 uv = float2((id << 1) & 2, id & 2); o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = uv; return o; }\n"
            "Texture2D t : register(t0); SamplerState s : register(s0);\n"
            "float4 psraw(V i) : SV_Target { return t.SampleLevel(s, i.uv, 0); }\n"
            "float3 dec(float3 c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }\n"
            "float4 pssrgb(V i) : SV_Target { float4 c = t.SampleLevel(s, i.uv, 0); return float4(dec(saturate(c.rgb)), c.a); }\n";
        using Fn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
        auto fn = (Fn)GetProcAddress(LoadLibraryA("d3dcompiler_47.dll"), "D3DCompile");
        ComPtr<ID3DBlob> b1, b2, b3, err;
        bool ok = fn && SUCCEEDED(fn(src, strlen(src), "blit", nullptr, nullptr, "vsmain", "vs_5_0", 0, 0, &b1, &err)) &&
                  SUCCEEDED(fn(src, strlen(src), "blit", nullptr, nullptr, "psraw", "ps_5_0", 0, 0, &b2, &err)) &&
                  SUCCEEDED(fn(src, strlen(src), "blit", nullptr, nullptr, "pssrgb", "ps_5_0", 0, 0, &b3, &err));
        ok = ok && SUCCEEDED(dev->CreateVertexShader(b1->GetBufferPointer(), b1->GetBufferSize(), nullptr, &vs)) &&
             SUCCEEDED(dev->CreatePixelShader(b2->GetBufferPointer(), b2->GetBufferSize(), nullptr, &ps_raw)) &&
             SUCCEEDED(dev->CreatePixelShader(b3->GetBufferPointer(), b3->GetBufferSize(), nullptr, &ps_srgb));
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        ok = ok && SUCCEEDED(dev->CreateSamplerState(&sd, &samp));
        ComPtr<ID3D11Device1> dev1;
        if (ok && SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&dev1)))) {
            const D3D_FEATURE_LEVEL lvl = dev->GetFeatureLevel();
            ok = SUCCEEDED(dev1->CreateDeviceContextState(0, &lvl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr, &state));
        } else {
            ok = false;
        }
        if (!ok) {
            spdlog::error("[VR] stretch blit unavailable: eye images of a different size than the swapchain cannot be shown");
            failed = true;
            vs.Reset();
        }
        return ok;
    }

    static DXGI_FORMAT unorm_of(DXGI_FORMAT f) {
        switch (f) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        default: return f;
        }
    }

    void run(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* dst, ID3D11Texture2D* src) {
        if (!init(dev)) return;
        D3D11_TEXTURE2D_DESC sd{};
        src->GetDesc(&sd);
        if (!src_copy || sd.Width != src_desc.Width || sd.Height != src_desc.Height || sd.Format != src_desc.Format) {
            src_copy.Reset(); src_srv.Reset();
            D3D11_TEXTURE2D_DESC c = sd;
            c.MipLevels = 1; c.ArraySize = 1; c.SampleDesc = {1, 0}; c.Usage = D3D11_USAGE_DEFAULT;
            c.BindFlags = D3D11_BIND_SHADER_RESOURCE; c.CPUAccessFlags = 0; c.MiscFlags = 0;
            if (FAILED(dev->CreateTexture2D(&c, nullptr, &src_copy))) return;
            D3D11_SHADER_RESOURCE_VIEW_DESC v{};
            v.Format = unorm_of(sd.Format); v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; v.Texture2D.MipLevels = 1;
            if (FAILED(dev->CreateShaderResourceView(src_copy.Get(), &v, &src_srv))) { src_copy.Reset(); return; }
            src_desc = sd;
        }
        ctx->CopyResource(src_copy.Get(), src);

        D3D11_TEXTURE2D_DESC dd{};
        dst->GetDesc(&dd);
        const bool typeless = dd.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS || dd.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
        const bool srgb = typeless || dd.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || dd.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        auto& rtv = rtvs[dst];
        if (!rtv) {
            D3D11_RENDER_TARGET_VIEW_DESC r{};
            r.Format = dd.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                     : dd.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : dd.Format;
            r.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            if (FAILED(dev->CreateRenderTargetView(dst, &r, &rtv))) {
                spdlog::error("[VR] stretch blit: no render target view on the swapchain image (format {})", (int)dd.Format);
                return;
            }
            spdlog::info("[VR] stretch blit {}x{} -> {}x{} (swapchain format {}, view format {})", sd.Width, sd.Height, dd.Width, dd.Height, (int)dd.Format, (int)r.Format);
        }

        ComPtr<ID3D11DeviceContext1> ctx1;
        if (FAILED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1)))) return;
        ComPtr<ID3DDeviceContextState> prev;
        ctx1->SwapDeviceContextState(state.Get(), &prev);
        D3D11_VIEWPORT vp{0, 0, (float)dd.Width, (float)dd.Height, 0, 1};
        ctx->RSSetViewports(1, &vp);
        ID3D11RenderTargetView* r = rtv.Get();
        ctx->OMSetRenderTargets(1, &r, nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetInputLayout(nullptr);
        ctx->VSSetShader(vs.Get(), nullptr, 0);
        ctx->PSSetShader(srgb ? ps_srgb.Get() : ps_raw.Get(), nullptr, 0);
        ID3D11ShaderResourceView* srv = src_srv.Get();
        ctx->PSSetShaderResources(0, 1, &srv);
        ID3D11SamplerState* smp = samp.Get();
        ctx->PSSetSamplers(0, 1, &smp);
        ctx->Draw(3, 0);
        ID3D11ShaderResourceView* none = nullptr;
        ctx->PSSetShaderResources(0, 1, &none);
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
        ctx1->SwapDeviceContextState(prev.Get(), nullptr);
    }
};
StretchBlit g_stretch{};
}

void D3D11Component::OpenXR::clear(uint32_t swapchain_idx) {
    auto& vr = VR::get();
    if (swapchain_idx >= (uint32_t)runtimes::OpenXR::SwapchainIndex::END) {
        return;
    }
    const auto& sc = vr->m_openxr->swapchains[swapchain_idx];
    auto&       z  = this->zero_textures[swapchain_idx];
    if (!z) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = std::max(1, (int)sc.width); d.Height = std::max(1, (int)sc.height); d.MipLevels = 1; d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        std::vector<uint32_t> zeros((size_t)d.Width * d.Height, 0u);
        D3D11_SUBRESOURCE_DATA init{zeros.data(), d.Width * 4u, 0};
        g_framework->get_d3d11_hook()->get_device()->CreateTexture2D(&d, &init, &z);
        spdlog::info("[VR] transparent image for swapchain {} ({}x{}): {}", swapchain_idx, d.Width, d.Height, z ? "ready" : "failed");
    }
    if (z) {
        copy(swapchain_idx, z.Get());
    }
}

void D3D11Component::OpenXR::copy(uint32_t eye_idx, ID3D11Texture2D* resource) {
    std::scoped_lock _{this->mtx};
    const auto copy_t0 = std::chrono::steady_clock::now();
    struct CopyTimer { std::chrono::steady_clock::time_point t0; ~CopyTimer() { runtimes::OpenXR::s_fc5_copy_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); runtimes::OpenXR::s_fc5_copy_us += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count(); } } copy_timer{ copy_t0 };

    auto& vr = VR::get();
    // Far Cry 5: eye swapchains alternate between two image sets per headset frame (see OpenXR::eye_swapchain).
    const uint32_t swapchain_idx = vr->m_openxr->eye_swapchain(eye_idx);

    if (!vr->m_openxr->should_render()) {
        return;
    }

    if (!vr->m_openxr->frame_began) {
        spdlog::error("[VR] OpenXR: Frame not begun when trying to copy.");
        //return;
    }

    if (this->contexts[swapchain_idx].num_textures_acquired > 0) {
        spdlog::info("[VR] Already acquired textures for swapchain {}?", swapchain_idx);
    }

    auto device = g_framework->get_d3d11_hook()->get_device();
    
    // get immediate context
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);

    const auto& swapchain = vr->m_openxr->swapchains[swapchain_idx];
    auto& ctx = this->contexts[swapchain_idx];

    XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};

    uint32_t texture_index{};
    LOG_VERBOSE("Acquiring swapchain image for {}", swapchain_idx);
    auto result = xrAcquireSwapchainImage(swapchain.handle, &acquire_info, &texture_index);

    if (result != XR_SUCCESS) {
        spdlog::error("[VR] xrAcquireSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
    } else {
        ctx.num_textures_acquired++;

        XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        //wait_info.timeout = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::seconds(1)).count();
        wait_info.timeout = XR_INFINITE_DURATION;

        LOG_VERBOSE("Waiting on swapchain image for {}", swapchain_idx);
        result = xrWaitSwapchainImage(swapchain.handle, &wait_info);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] xrWaitSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
        } else {
            LOG_VERBOSE("Copying swapchain image {} for {}", texture_index, swapchain_idx);
            {
                D3D11_TEXTURE2D_DESC a{}, b{};
                ((ID3D11Texture2D*)ctx.textures[texture_index].texture)->GetDesc(&a);
                resource->GetDesc(&b);
                if (a.Width == b.Width && a.Height == b.Height) {
                    context->CopyResource(ctx.textures[texture_index].texture, resource);
                } else {
                    g_stretch.run(device, context.Get(), (ID3D11Texture2D*)ctx.textures[texture_index].texture, resource);
                }
            }
            // Far Cry 5 check: is what the headset gets for this eye black? Every copy: 5 pixels of the
            // swapchain image into a staging ring, read back 6 copies later without waiting (no stall).
            if (eye_idx <= 1) {
                struct ProbeSlot { ComPtr<ID3D11Texture2D> st{}; DXGI_FORMAT fmt{}; int frame{ -1 }; bool pending{ false }; };
                static ProbeSlot ring[2][6]{};
                static uint32_t  pos[2]{}, checked[2]{}, black[2]{}, busy[2]{};
                static auto      last = std::chrono::steady_clock::now();
                auto*            img  = (ID3D11Texture2D*)ctx.textures[texture_index].texture;
                D3D11_TEXTURE2D_DESC d{};
                img->GetDesc(&d);
                auto& slot = ring[eye_idx][pos[eye_idx]++ % 6];
                if (slot.pending && slot.st) {
                    D3D11_MAPPED_SUBRESOURCE m{};
                    if (SUCCEEDED(context->Map(slot.st.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) {
                        const uint32_t* px = static_cast<const uint32_t*>(m.pData);
                        bool all_black = true;
                        for (int k = 0; k < 5; ++k) if ((px[k] & 0x00ffffff) != 0) all_black = false;
                        context->Unmap(slot.st.Get(), 0);
                        ++checked[eye_idx];
                        if (all_black) {
                            ++black[eye_idx];
                            vr->m_fc5_black_frame = slot.frame;
                        }
                    } else {
                        ++busy[eye_idx];
                    }
                    slot.pending = false;
                }
                if (!slot.st || slot.fmt != d.Format) {
                    slot.st.Reset();
                    D3D11_TEXTURE2D_DESC sd{};
                    sd.Width = 5; sd.Height = 1; sd.MipLevels = 1; sd.ArraySize = 1; sd.Format = d.Format; sd.SampleDesc.Count = 1;
                    sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    device->CreateTexture2D(&sd, nullptr, &slot.st);
                    slot.fmt = d.Format;
                }
                if (slot.st) {
                    const UINT xs[5]{ d.Width / 2, d.Width / 4, 3 * d.Width / 4, d.Width / 2, d.Width / 2 };
                    const UINT ys[5]{ d.Height / 2, d.Height / 2, d.Height / 2, d.Height / 4, 3 * d.Height / 4 };
                    for (UINT k = 0; k < 5; ++k) {
                        D3D11_BOX box{ xs[k], ys[k], 0, xs[k] + 1, ys[k] + 1, 1 };
                        context->CopySubresourceRegion(slot.st.Get(), 0, k, 0, 0, img, 0, &box);
                    }
                    slot.frame   = vr->m_presenter_frame_count;
                    slot.pending = true;
                }
                if (std::chrono::steady_clock::now() - last > std::chrono::seconds(5)) {
                    last = std::chrono::steady_clock::now();
                    spdlog::info("[VR] eye images to the headset in the last 5 s: left {} ({} black), right {} ({} black); readbacks not ready {} / {}", checked[0], black[0],
                                 checked[1], black[1], busy[0], busy[1]);
                    checked[0] = checked[1] = black[0] = black[1] = busy[0] = busy[1] = 0;
                }
            }
            XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};

            LOG_VERBOSE("Releasing swapchain image for {}", swapchain_idx);
            auto result = xrReleaseSwapchainImage(swapchain.handle, &release_info);

            if (result != XR_SUCCESS) {
                spdlog::error("[VR] xrReleaseSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
                return;
            }

            ctx.num_textures_acquired--;
            ctx.ever_acquired = true;
        }
    }
}
} // namespace vrmod
