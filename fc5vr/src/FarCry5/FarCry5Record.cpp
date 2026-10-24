#include "FarCry5Record.h"

#include <spdlog/spdlog.h>
#include <windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fc5::rec
{
    namespace
    {
        constexpr int kVtableSize = 149; // ID3D11DeviceContext4

        // Indices (ID3D11DeviceContext4 vtable) that must stay on the immediate context.
        enum : int
        {
            kQueryInterface = 0, kAddRef = 1, kRelease = 2, kGetDevice = 3, kGetPrivateData = 4, kSetPrivateData = 5, kSetPrivateDataInterface = 6,
            kMap = 14, kUnmap = 15, kGetData = 29, kFlush = 111, kGetType = 112, kGetContextFlags = 113, kFinishCommandList = 114,
            kSwapDeviceContextState = 131, kFlush1 = 144, kSetHardwareProtectionState = 145, kGetHardwareProtectionState = 146, kSignal = 147, kWait = 148,
        };

        ID3D11Device*         g_device{ nullptr };
        ID3D11DeviceContext4* g_deferred{ nullptr };
        ID3D11DeviceContext*  g_immediate{ nullptr };
        void**                g_orig_vtable{ nullptr };
        void*                 g_thunks{ nullptr };
        void*                 g_vtable[kVtableSize]{};
        uint64_t              g_slot_count[kVtableSize]{};
        std::vector<uint8_t*> g_jne_sites{};
        std::vector<uint8_t>  g_jne_disp{};
        uint64_t              g_slot_snap[kVtableSize]{};
        void*                 g_handlers[kVtableSize]{};
        void**                g_def_vtable{ nullptr };
        bool                  g_ready{ false };
        std::atomic<bool>     g_recording{ false };
        std::unordered_set<ID3D11Resource*> g_mapped_on_immediate{};
        int64_t g_open_def_maps{ 0 };
        uint64_t g_maps_def{ 0 }, g_maps_imm{ 0 }, g_map_fail{ 0 }, g_nooverwrite{ 0 }, g_recordings{ 0 };

        using MapFn   = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
        using UnmapFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT);

        HRESULT STDMETHODCALLTYPE map_thunk(ID3D11DeviceContext* self, ID3D11Resource* res, UINT sub, D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE* out)
        {
            auto* orig_map = reinterpret_cast<MapFn>(g_orig_vtable[kMap]);
            if (type == D3D11_MAP_READ || type == D3D11_MAP_READ_WRITE || type == D3D11_MAP_WRITE) {
                // Read-backs and staging writes cannot be recorded: done now on the immediate context.
                g_maps_imm++;
                const HRESULT hr = orig_map(self, res, sub, type, flags, out);
                if (SUCCEEDED(hr)) g_mapped_on_immediate.insert(res);
                return hr;
            }
            if (type == D3D11_MAP_WRITE_NO_OVERWRITE) g_nooverwrite++;
            g_maps_def++;
            g_open_def_maps++;
            auto* def_map = reinterpret_cast<MapFn>(g_handlers[kMap] ? g_handlers[kMap] : g_def_vtable[kMap]);
            auto* defctx  = static_cast<ID3D11DeviceContext*>(g_deferred);
            HRESULT hr = def_map(defctx, res, sub, type, flags, out);
            if (FAILED(hr) && type == D3D11_MAP_WRITE_NO_OVERWRITE) {
                // A deferred context needs a discard before the first no-overwrite map of a resource.
                hr = def_map(defctx, res, sub, D3D11_MAP_WRITE_DISCARD, flags, out);
            }
            if (FAILED(hr) && g_map_fail++ < 10) spdlog::error("[Rec] deferred Map failed {:08x} (type {})", (uint32_t)hr, (int)type);
            return hr;
        }

        void STDMETHODCALLTYPE unmap_thunk(ID3D11DeviceContext* self, ID3D11Resource* res, UINT sub)
        {
            if (auto it = g_mapped_on_immediate.find(res); it != g_mapped_on_immediate.end()) {
                g_mapped_on_immediate.erase(it);
                reinterpret_cast<UnmapFn>(g_orig_vtable[kUnmap])(self, res, sub);
                return;
            }
            g_open_def_maps--;
            reinterpret_cast<UnmapFn>(g_handlers[kUnmap] ? g_handlers[kUnmap] : g_def_vtable[kUnmap])(static_cast<ID3D11DeviceContext*>(g_deferred), res, sub);
        }

        void STDMETHODCALLTYPE flush_thunk(ID3D11DeviceContext*) {}
        bool g_no_pred{ false };
        using SetPredFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Predicate*, BOOL);
        void STDMETHODCALLTYPE setpred_thunk(ID3D11DeviceContext*, ID3D11Predicate* p, BOOL v)
        {
            reinterpret_cast<SetPredFn>((*reinterpret_cast<void***>(g_deferred))[30])(static_cast<ID3D11DeviceContext*>(g_deferred), g_no_pred ? nullptr : p, v);
        }

        // Event and timestamp queries are waited on by the game within the frame (frame pacing, GPU
        // timers): they go to the immediate context. Occlusion and statistics queries measure the
        // recorded draws: they stay in the recording.
        uint64_t g_queries_imm{ 0 };
        bool g_route_queries{ true };
        bool query_on_immediate(ID3D11Asynchronous* a)
        {
            if (!g_route_queries) return false;
            if (!a) return true;
            ID3D11Query* q = nullptr;
            if (FAILED(a->QueryInterface(__uuidof(ID3D11Query), reinterpret_cast<void**>(&q))) || !q) return true;
            D3D11_QUERY_DESC d{};
            q->GetDesc(&d);
            q->Release();
            return d.Query == D3D11_QUERY_EVENT || d.Query == D3D11_QUERY_TIMESTAMP || d.Query == D3D11_QUERY_TIMESTAMP_DISJOINT;
        }
        using AsyncFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Asynchronous*);
        void STDMETHODCALLTYPE begin_thunk(ID3D11DeviceContext* self, ID3D11Asynchronous* a)
        {
            if (query_on_immediate(a)) { g_queries_imm++; reinterpret_cast<AsyncFn>(g_orig_vtable[27])(self, a); }
            else reinterpret_cast<AsyncFn>((*reinterpret_cast<void***>(g_deferred))[27])(static_cast<ID3D11DeviceContext*>(g_deferred), a);
        }
        std::unordered_set<ID3D11Asynchronous*> g_deferred_queries{};
        uint64_t g_fake_getdata{ 0 };
        using FlushFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*);
        void STDMETHODCALLTYPE end_thunk(ID3D11DeviceContext* self, ID3D11Asynchronous* a)
        {
            if (query_on_immediate(a)) {
                g_queries_imm++;
                reinterpret_cast<AsyncFn>(g_orig_vtable[28])(self, a);
                // The game polls it with "do not flush": the immediate context has little else queued, so
                // it is submitted now or it would never complete.
                reinterpret_cast<FlushFn>(g_orig_vtable[111])(self);
            } else {
                g_deferred_queries.insert(a);
                reinterpret_cast<AsyncFn>((*reinterpret_cast<void***>(g_deferred))[28])(static_cast<ID3D11DeviceContext*>(g_deferred), a);
            }
        }
        // A query ended inside the recording has not run yet: a neutral answer (visible / zero) instead of
        // letting the game wait for it.
        using GetDataFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Asynchronous*, void*, UINT, UINT);
        HRESULT STDMETHODCALLTYPE getdata_thunk(ID3D11DeviceContext* self, ID3D11Asynchronous* a, void* data, UINT size, UINT flags)
        {
            if (g_route_queries && a && g_deferred_queries.contains(a)) {
                g_fake_getdata++;
                if (data && size) {
                    std::memset(data, 0, size);
                    if (size == 8) *static_cast<uint64_t*>(data) = 1000000; // occlusion: samples passed
                    if (size == 4) *static_cast<uint32_t*>(data) = TRUE;    // occlusion predicate
                }
                return S_OK;
            }
            return reinterpret_cast<GetDataFn>(g_orig_vtable[kGetData])(self, a, data, size, flags);
        }

        // mov rcx, imm64 ; mov rax, imm64 ; jmp qword ptr [rax + disp32]
        uint8_t* emit_jump(uint8_t* p, void* this_ptr, void** vtable, int index)
        {
            *p++ = 0x48; *p++ = 0xB9; std::memcpy(p, &this_ptr, 8); p += 8;
            *p++ = 0x48; *p++ = 0xB8; std::memcpy(p, &vtable, 8); p += 8;
            const int32_t disp = index * 8;
            *p++ = 0xFF; *p++ = 0xA0; std::memcpy(p, &disp, 4); p += 4;
            return p;
        }

        struct State
        {
            // IA
            ID3D11InputLayout* layout{};
            ID3D11Buffer*      vbs[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT]{};
            UINT               strides[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT]{}, offsets[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT]{};
            ID3D11Buffer*      ib{};
            DXGI_FORMAT        ib_fmt{};
            UINT               ib_off{};
            D3D11_PRIMITIVE_TOPOLOGY topo{};
            // Shader stages: VS HS DS GS PS CS
            ID3D11DeviceChild*         shader[6]{};
            ID3D11Buffer*              cbs[6][D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT]{};
            UINT                       cb_first[6][D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT]{}, cb_num[6][D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT]{};
            ID3D11ShaderResourceView*  srvs[6][D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT]{};
            ID3D11SamplerState*        samplers[6][D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT]{};
            ID3D11UnorderedAccessView* cs_uavs[D3D11_1_UAV_SLOT_COUNT]{};
            // RS
            ID3D11RasterizerState* rs{};
            D3D11_VIEWPORT         vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
            UINT                   num_vps{};
            D3D11_RECT             scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
            UINT                   num_scissors{};
            // OM
            ID3D11RenderTargetView*    rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
            ID3D11DepthStencilView*    dsv{};
            ID3D11UnorderedAccessView* om_uavs[D3D11_1_UAV_SLOT_COUNT]{};
            ID3D11BlendState*          blend{};
            float                      blend_factor[4]{};
            UINT                       sample_mask{};
            ID3D11DepthStencilState*   ds_state{};
            UINT                       stencil_ref{};
            // SO, predication
            ID3D11Buffer*    so[D3D11_SO_BUFFER_SLOT_COUNT]{};
            ID3D11Predicate* pred{};
            BOOL             pred_value{};

            void capture(ID3D11DeviceContext1* c)
            {
                release();
                c->IAGetInputLayout(&layout);
                c->IAGetVertexBuffers(0, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT, vbs, strides, offsets);
                c->IAGetIndexBuffer(&ib, &ib_fmt, &ib_off);
                c->IAGetPrimitiveTopology(&topo);
                c->VSGetShader(reinterpret_cast<ID3D11VertexShader**>(&shader[0]), nullptr, nullptr);
                c->HSGetShader(reinterpret_cast<ID3D11HullShader**>(&shader[1]), nullptr, nullptr);
                c->DSGetShader(reinterpret_cast<ID3D11DomainShader**>(&shader[2]), nullptr, nullptr);
                c->GSGetShader(reinterpret_cast<ID3D11GeometryShader**>(&shader[3]), nullptr, nullptr);
                c->PSGetShader(reinterpret_cast<ID3D11PixelShader**>(&shader[4]), nullptr, nullptr);
                c->CSGetShader(reinterpret_cast<ID3D11ComputeShader**>(&shader[5]), nullptr, nullptr);
                constexpr UINT ncb = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, nsrv = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT,
                               nsmp = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT;
                c->VSGetConstantBuffers1(0, ncb, cbs[0], cb_first[0], cb_num[0]);
                c->HSGetConstantBuffers1(0, ncb, cbs[1], cb_first[1], cb_num[1]);
                c->DSGetConstantBuffers1(0, ncb, cbs[2], cb_first[2], cb_num[2]);
                c->GSGetConstantBuffers1(0, ncb, cbs[3], cb_first[3], cb_num[3]);
                c->PSGetConstantBuffers1(0, ncb, cbs[4], cb_first[4], cb_num[4]);
                c->CSGetConstantBuffers1(0, ncb, cbs[5], cb_first[5], cb_num[5]);
                c->VSGetShaderResources(0, nsrv, srvs[0]);
                c->HSGetShaderResources(0, nsrv, srvs[1]);
                c->DSGetShaderResources(0, nsrv, srvs[2]);
                c->GSGetShaderResources(0, nsrv, srvs[3]);
                c->PSGetShaderResources(0, nsrv, srvs[4]);
                c->CSGetShaderResources(0, nsrv, srvs[5]);
                c->VSGetSamplers(0, nsmp, samplers[0]);
                c->HSGetSamplers(0, nsmp, samplers[1]);
                c->DSGetSamplers(0, nsmp, samplers[2]);
                c->GSGetSamplers(0, nsmp, samplers[3]);
                c->PSGetSamplers(0, nsmp, samplers[4]);
                c->CSGetSamplers(0, nsmp, samplers[5]);
                c->CSGetUnorderedAccessViews(0, D3D11_1_UAV_SLOT_COUNT, cs_uavs);
                c->RSGetState(&rs);
                num_vps = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
                c->RSGetViewports(&num_vps, vps);
                num_scissors = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
                c->RSGetScissorRects(&num_scissors, scissors);
                c->OMGetRenderTargetsAndUnorderedAccessViews(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv, 0, D3D11_1_UAV_SLOT_COUNT, om_uavs);
                c->OMGetBlendState(&blend, blend_factor, &sample_mask);
                c->OMGetDepthStencilState(&ds_state, &stencil_ref);
                c->SOGetTargets(D3D11_SO_BUFFER_SLOT_COUNT, so);
                c->GetPredication(&pred, &pred_value);
            }

            void apply(ID3D11DeviceContext1* c) const
            {
                c->IASetInputLayout(layout);
                c->IASetVertexBuffers(0, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT, vbs, strides, offsets);
                c->IASetIndexBuffer(ib, ib_fmt, ib_off);
                c->IASetPrimitiveTopology(topo);
                c->VSSetShader(static_cast<ID3D11VertexShader*>(shader[0]), nullptr, 0);
                c->HSSetShader(static_cast<ID3D11HullShader*>(shader[1]), nullptr, 0);
                c->DSSetShader(static_cast<ID3D11DomainShader*>(shader[2]), nullptr, 0);
                c->GSSetShader(static_cast<ID3D11GeometryShader*>(shader[3]), nullptr, 0);
                c->PSSetShader(static_cast<ID3D11PixelShader*>(shader[4]), nullptr, 0);
                c->CSSetShader(static_cast<ID3D11ComputeShader*>(shader[5]), nullptr, 0);
                constexpr UINT ncb = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, nsrv = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT,
                               nsmp = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT;
                // A buffer bound without an offset reports first 0 / num 0 (or 4096): bind it whole then.
                auto set_cbs = [&](int s, auto fn) {
                    for (UINT i = 0; i < ncb; ++i) {
                        if (!cbs[s][i]) continue;
                        if (cb_num[s][i] > 0 && cb_num[s][i] < 4096) (c->*fn)(i, 1, &cbs[s][i], &cb_first[s][i], &cb_num[s][i]);
                        else (c->*fn)(i, 1, &cbs[s][i], nullptr, nullptr);
                    }
                };
                set_cbs(0, &ID3D11DeviceContext1::VSSetConstantBuffers1);
                set_cbs(1, &ID3D11DeviceContext1::HSSetConstantBuffers1);
                set_cbs(2, &ID3D11DeviceContext1::DSSetConstantBuffers1);
                set_cbs(3, &ID3D11DeviceContext1::GSSetConstantBuffers1);
                set_cbs(4, &ID3D11DeviceContext1::PSSetConstantBuffers1);
                set_cbs(5, &ID3D11DeviceContext1::CSSetConstantBuffers1);
                c->VSSetShaderResources(0, nsrv, srvs[0]);
                c->HSSetShaderResources(0, nsrv, srvs[1]);
                c->DSSetShaderResources(0, nsrv, srvs[2]);
                c->GSSetShaderResources(0, nsrv, srvs[3]);
                c->PSSetShaderResources(0, nsrv, srvs[4]);
                c->CSSetShaderResources(0, nsrv, srvs[5]);
                c->VSSetSamplers(0, nsmp, samplers[0]);
                c->HSSetSamplers(0, nsmp, samplers[1]);
                c->DSSetSamplers(0, nsmp, samplers[2]);
                c->GSSetSamplers(0, nsmp, samplers[3]);
                c->PSSetSamplers(0, nsmp, samplers[4]);
                c->CSSetSamplers(0, nsmp, samplers[5]);
                const UINT keep[D3D11_1_UAV_SLOT_COUNT]{ ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u,
                                                         ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u,
                                                         ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u,
                                                         ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u };
                c->CSSetUnorderedAccessViews(0, D3D11_1_UAV_SLOT_COUNT, cs_uavs, keep);
                c->RSSetState(rs);
                c->RSSetViewports(num_vps, vps);
                c->RSSetScissorRects(num_scissors, scissors);
                // UAVs bound with the render targets start after the last render target slot in use.
                UINT n_rt = 0;
                for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) if (rtvs[i]) n_rt = i + 1;
                UINT n_uav = 0;
                for (UINT i = 0; i < D3D11_1_UAV_SLOT_COUNT; ++i) if (om_uavs[i]) n_uav = i + 1;
                if (n_uav > n_rt) {
                    c->OMSetRenderTargetsAndUnorderedAccessViews(n_rt, rtvs, dsv, n_rt, n_uav - n_rt, om_uavs + n_rt, keep);
                } else {
                    c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
                }
                c->OMSetBlendState(blend, blend_factor, sample_mask);
                c->OMSetDepthStencilState(ds_state, stencil_ref);
                const UINT so_off[D3D11_SO_BUFFER_SLOT_COUNT]{};
                c->SOSetTargets(D3D11_SO_BUFFER_SLOT_COUNT, so, so_off);
                c->SetPredication(g_no_pred ? nullptr : pred, pred_value);
            }

            void release()
            {
                auto rel = [](auto*& p) { if (p) { p->Release(); p = nullptr; } };
                rel(layout);
                for (auto& b : vbs) rel(b);
                rel(ib);
                for (auto& s : shader) rel(s);
                for (auto& st : cbs) for (auto& b : st) rel(b);
                for (auto& st : srvs) for (auto& v : st) rel(v);
                for (auto& st : samplers) for (auto& v : st) rel(v);
                for (auto& u : cs_uavs) rel(u);
                rel(rs);
                for (auto& r : rtvs) rel(r);
                rel(dsv);
                for (auto& u : om_uavs) rel(u);
                rel(blend);
                rel(ds_state);
                for (auto& b : so) rel(b);
                rel(pred);
            }
            ~State() { release(); }
        };

        State g_start{}, g_end{};
        double g_rec_ms{ 0.0 };

        // ---- Stereo: eye-dependent constant buffers ----
        // A buffer the game maps with camera data gets a stand-in (DEFAULT usage, same size) holding the
        // left and right eye contents; the recording binds the stand-in wherever the game binds the
        // buffer, and execute_eye() uploads that eye's contents before each execution.
        // The command list keeps the stand-in's memory as it was when recorded: the stand-in is never
        // written directly (that would give it new memory), only GPU-copied from the eye's source.
        struct Variant
        {
            ID3D11Buffer*        stand_in{};
            ID3D11Buffer*        source[2]{};
            std::vector<uint8_t> data[2];
        };
        bool                                              g_stereo{ false };
        DWORD                                             g_rec_tid{ 0 };
        int                                               g_verify{ 0 };
        bool                                              g_poison{ false };
        int                                               g_log_end{ 0 };
        std::vector<Variant>                              g_variants{};
        std::unordered_map<ID3D11Buffer*, ID3D11Buffer*>  g_subst{};
        std::unordered_map<UINT, std::vector<ID3D11Buffer*>> g_pool{};
        std::unordered_map<UINT, size_t>                  g_pool_used{};
        struct Bound
        {
            ID3D11Buffer* buf{};
            UINT          first{}, num{};
            bool          ranged{};
        };
        Bound g_bound[6][D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT]{};
        uint64_t g_variant_count{ 0 };

        // Constant buffer setters: slot index of the plain and the "1" (ranged) version per stage
        // (VS HS DS GS PS CS).
        constexpr int kSetCb[6]  = { 7, 62, 66, 22, 16, 71 };
        constexpr int kSetCb1[6] = { 119, 120, 121, 122, 123, 124 };
        using SetCbFn  = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*);
        using SetCb1Fn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*, const UINT*, const UINT*);

        void set_cbs(int stage, UINT start, UINT n, ID3D11Buffer* const* bufs, const UINT* first, const UINT* num)
        {
            ID3D11Buffer* sub[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT]{};
            n = std::min<UINT>(n, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - std::min<UINT>(start, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT));
            for (UINT i = 0; i < n; ++i) {
                ID3D11Buffer* b = bufs ? bufs[i] : nullptr;
                auto&         bd = g_bound[stage][start + i];
                bd.buf           = b;
                bd.ranged        = first && num;
                bd.first         = first ? first[i] : 0;
                bd.num           = num ? num[i] : 0;
                auto it          = b ? g_subst.find(b) : g_subst.end();
                sub[i]           = it != g_subst.end() ? it->second : b;
            }
            auto* def = static_cast<ID3D11DeviceContext*>(g_deferred);
            if (first && num) reinterpret_cast<SetCb1Fn>(g_def_vtable[kSetCb1[stage]])(def, start, n, sub, first, num);
            else reinterpret_cast<SetCbFn>(g_def_vtable[kSetCb[stage]])(def, start, n, sub);
        }

        template <int S> void STDMETHODCALLTYPE set_cbs_w(ID3D11DeviceContext*, UINT start, UINT n, ID3D11Buffer* const* bufs) { set_cbs(S, start, n, bufs, nullptr, nullptr); }
        template <int S> void STDMETHODCALLTYPE set_cbs1_w(ID3D11DeviceContext*, UINT start, UINT n, ID3D11Buffer* const* bufs, const UINT* f, const UINT* c)
        {
            set_cbs(S, start, n, bufs, f, c);
        }
        void* const kSetCbW[6]  = { (void*)&set_cbs_w<0>, (void*)&set_cbs_w<1>, (void*)&set_cbs_w<2>, (void*)&set_cbs_w<3>, (void*)&set_cbs_w<4>, (void*)&set_cbs_w<5> };
        void* const kSetCb1W[6] = { (void*)&set_cbs1_w<0>, (void*)&set_cbs1_w<1>, (void*)&set_cbs1_w<2>, (void*)&set_cbs1_w<3>, (void*)&set_cbs1_w<4>, (void*)&set_cbs1_w<5> };

        ID3D11Buffer* pooled(UINT size)
        {
            auto& v    = g_pool[size];
            auto& used = g_pool_used[size];
            if (used < v.size()) return v[used++];
            D3D11_BUFFER_DESC d{};
            d.ByteWidth      = size;
            d.Usage          = D3D11_USAGE_DEFAULT;
            d.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
            ID3D11Buffer* b  = nullptr;
            if (FAILED(g_device->CreateBuffer(&d, nullptr, &b)) || !b) return nullptr;
            v.push_back(b);
            ++used;
            return b;
        }
    }

    bool initialize(ID3D11Device* device, ID3D11DeviceContext* immediate)
    {
        if (g_ready) return true;
        static bool tried = false;
        if (tried) return false;
        tried = true;
        ID3D11DeviceContext4* imm4 = nullptr;
        if (FAILED(immediate->QueryInterface(__uuidof(ID3D11DeviceContext4), reinterpret_cast<void**>(&imm4))) || !imm4) {
            spdlog::error("[Rec] no ID3D11DeviceContext4 on the immediate context");
            return false;
        }
        imm4->Release();
        ID3D11DeviceContext* def = nullptr;
        if (FAILED(device->CreateDeferredContext(0, &def)) || !def) {
            spdlog::error("[Rec] CreateDeferredContext failed");
            return false;
        }
        if (FAILED(def->QueryInterface(__uuidof(ID3D11DeviceContext4), reinterpret_cast<void**>(&g_deferred))) || !g_deferred) {
            def->Release();
            spdlog::error("[Rec] no ID3D11DeviceContext4 on the deferred context");
            return false;
        }
        def->Release();
        g_device        = device;
        g_immediate     = immediate;
        g_orig_vtable   = *reinterpret_cast<void***>(immediate);
        void** def_vt   = *reinterpret_cast<void***>(g_deferred);
        g_def_vtable    = def_vt;
        int    shared   = 0;
        for (int i = 0; i < kVtableSize; ++i) shared += def_vt[i] == g_orig_vtable[i];
        // Thunks: each slot jumps to the deferred context's method with `this` replaced, or to the
        // immediate context's own method for the calls that must stay there.
        // Every redirected slot first checks the calling thread: only the recording thread goes to the
        // deferred context; any other thread (texture streaming, ...) keeps the immediate context.
        g_thunks = VirtualAlloc(nullptr, kVtableSize * 96, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!g_thunks) return false;
        auto* p = static_cast<uint8_t*>(g_thunks);
        for (int i = 0; i < kVtableSize; ++i) {
            const bool stay = i <= kSetPrivateDataInterface || i == kGetType || i == kGetContextFlags || i == kFinishCommandList ||
                              i == kSwapDeviceContextState || i == kSetHardwareProtectionState || i == kGetHardwareProtectionState || i == kSignal || i == kWait;
            g_vtable[i] = p;
            if (stay) {
                p = emit_jump(p, immediate, g_orig_vtable, i);
                while (reinterpret_cast<uintptr_t>(p) % 16) *p++ = 0xCC;
                continue;
            }
            void* cpp = nullptr;
            if (i == kMap) cpp = reinterpret_cast<void*>(&map_thunk);
            if (i == kUnmap) cpp = reinterpret_cast<void*>(&unmap_thunk);
            if (i == kFlush || i == kFlush1) cpp = reinterpret_cast<void*>(&flush_thunk);
            if (i == 27) cpp = reinterpret_cast<void*>(&begin_thunk);
            if (i == 28) cpp = reinterpret_cast<void*>(&end_thunk);
            if (i == kGetData) cpp = reinterpret_cast<void*>(&getdata_thunk);
            if (i == 30) cpp = reinterpret_cast<void*>(&setpred_thunk);
            for (int s = 0; s < 6; ++s) {
                if (i == kSetCb[s] && !g_handlers[i]) cpp = kSetCbW[s];
                if (i == kSetCb1[s]) cpp = kSetCb1W[s];
            }
            // mov eax, gs:[0x48] (TEB thread id) ; mov r10, &g_rec_tid ; cmp eax, [r10] ; jne other
            *p++ = 0x65; *p++ = 0x8B; *p++ = 0x04; *p++ = 0x25; *p++ = 0x48; *p++ = 0x00; *p++ = 0x00; *p++ = 0x00;
            const void* tid_addr = &g_rec_tid;
            *p++ = 0x49; *p++ = 0xBA; std::memcpy(p, &tid_addr, 8); p += 8;
            *p++ = 0x41; *p++ = 0x3B; *p++ = 0x02;
            *p++ = 0x75;
            g_jne_sites.push_back(p - 1);
            uint8_t* jne_disp = p++;
            uint8_t* path     = p;
            {
                // mov r11, &g_slot_count[i] ; lock inc qword ptr [r11]  (bisect: which methods were recorded)
                const void* cnt = &g_slot_count[i];
                *p++ = 0x49; *p++ = 0xBB; std::memcpy(p, &cnt, 8); p += 8;
                *p++ = 0xF0; *p++ = 0x49; *p++ = 0xFF; *p++ = 0x03;
            }
            if (cpp) {
                // mov rax, fn ; jmp rax (the C++ thunk takes the immediate context as `this`)
                *p++ = 0x48; *p++ = 0xB8; std::memcpy(p, &cpp, 8); p += 8;
                *p++ = 0xFF; *p++ = 0xE0;
            } else if (g_handlers[i]) {
                // mov rcx, deferred ; mov rax, handler ; jmp rax
                void* self = g_deferred;
                *p++ = 0x48; *p++ = 0xB9; std::memcpy(p, &self, 8); p += 8;
                *p++ = 0x48; *p++ = 0xB8; std::memcpy(p, &g_handlers[i], 8); p += 8;
                *p++ = 0xFF; *p++ = 0xE0;
            } else {
                p = emit_jump(p, g_deferred, def_vt, i);
            }
            *jne_disp = uint8_t(p - path);
            p = emit_jump(p, immediate, g_orig_vtable, i);
            while (reinterpret_cast<uintptr_t>(p) % 16) *p++ = 0xCC;
        }
        FlushInstructionCache(GetCurrentProcess(), g_thunks, kVtableSize * 96);
        g_ready = true;
        spdlog::info("[Rec] ready: deferred context {}, {} of {} methods shared with the immediate context", (void*)g_deferred, shared, kVtableSize);
        return true;
    }

    bool ready() { return g_ready; }

    // Sanity test outside any recording: a red clear on the deferred context, executed on the immediate one.
    void red_test(ID3D11DeviceContext* immediate, ID3D11Texture2D* target)
    {
        if (!g_ready || !target) return;
        D3D11_TEXTURE2D_DESC td{};
        target->GetDesc(&td);
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format        = td.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ? DXGI_FORMAT_R8G8B8A8_UNORM : td.Format;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        ID3D11RenderTargetView* rtv = nullptr;
        if (FAILED(g_device->CreateRenderTargetView(target, &rd, &rtv)) || !rtv) { spdlog::error("[Rec] red test: no RTV"); return; }
        const float red[4]{ 1, 0, 0, 1 };
        g_deferred->ClearRenderTargetView(rtv, red);
        ID3D11CommandList* cl = nullptr;
        const HRESULT hr = g_deferred->FinishCommandList(FALSE, &cl);
        spdlog::info("[Rec] red test: finish {:08x}, list {}", (uint32_t)hr, (void*)cl);
        if (cl) {
            immediate->ExecuteCommandList(cl, FALSE);
            cl->Release();
        }
        rtv->Release();
    }
    void set_handler(int idx, void* fn) { if (idx >= 0 && idx < kVtableSize) g_handlers[idx] = fn; }
    bool is_deferred(const void* ctx) { return ctx != nullptr && ctx == static_cast<const void*>(static_cast<ID3D11DeviceContext*>(g_deferred)); }
    void* deferred_method(int idx)
    {
        for (int s = 0; s < 6; ++s) {
            if (idx == kSetCb[s]) return kSetCbW[s];
            if (idx == kSetCb1[s]) return kSetCb1W[s];
        }
        return g_def_vtable ? g_def_vtable[idx] : nullptr;
    }

    void set_stereo(bool on) { g_stereo = on; }
    void verify(int n) { g_verify = n; }
    void log_end(int n) { g_log_end = n; }
    void route_queries(bool on) { g_route_queries = on; }
    void no_predication(bool on) { g_no_pred = on; }
    void poison(bool on) { g_poison = on; }
    bool stereo_capture() { return g_stereo && g_recording.load(); }

    void register_variant(ID3D11Buffer* buf, const void* left, const void* right, uint32_t size)
    {
        if (!g_stereo || !g_recording || !buf) return;
        ID3D11Buffer* m  = pooled(size);
        ID3D11Buffer* s0 = pooled(size);
        ID3D11Buffer* s1 = pooled(size);
        if (!m || !s0 || !s1) return;
        Variant v;
        v.stand_in  = m;
        v.source[0] = s0;
        v.source[1] = s1;
        v.data[0].assign(static_cast<const uint8_t*>(left), static_cast<const uint8_t*>(left) + size);
        v.data[1].assign(static_cast<const uint8_t*>(right), static_cast<const uint8_t*>(right) + size);
        g_variants.push_back(std::move(v));
        g_subst[buf] = m;
        g_variant_count++;
        // Already bound somewhere: bind the stand-in there now.
        auto* def = static_cast<ID3D11DeviceContext*>(g_deferred);
        for (int s = 0; s < 6; ++s) {
            for (UINT i = 0; i < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; ++i) {
                const auto& bd = g_bound[s][i];
                if (bd.buf != buf) continue;
                if (bd.ranged) reinterpret_cast<SetCb1Fn>(g_def_vtable[kSetCb1[s]])(def, i, 1, &m, &bd.first, &bd.num);
                else reinterpret_cast<SetCbFn>(g_def_vtable[kSetCb[s]])(def, i, 1, &m);
            }
        }
    }

    void execute_eye(ID3D11DeviceContext* immediate, ID3D11CommandList* cl, int eye)
    {
        for (const auto& v : g_variants) {
            if ((eye & 1) == 0) {
                static std::vector<uint8_t> zeros;
                if (g_poison) zeros.assign(v.data[0].size(), 0);
                immediate->UpdateSubresource(v.source[0], 0, nullptr, g_poison ? zeros.data() : v.data[0].data(), 0, 0);
                immediate->UpdateSubresource(v.source[1], 0, nullptr, v.data[1].data(), 0, 0);
            }
            immediate->CopyResource(v.stand_in, v.source[eye & 1]);
        }
        immediate->ExecuteCommandList(cl, FALSE);
        if (g_verify > 0 && !g_variants.empty()) {
            // Test: read the largest stand-in back and compare with the eye's data.
            g_verify--;
            const Variant* best = &g_variants[0];
            for (const auto& v : g_variants) if (v.data[0].size() > best->data[0].size()) best = &v;
            D3D11_BUFFER_DESC bd{};
            best->stand_in->GetDesc(&bd);
            bd.Usage = D3D11_USAGE_STAGING; bd.BindFlags = 0; bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; bd.MiscFlags = 0;
            ID3D11Buffer* st = nullptr;
            if (SUCCEEDED(g_device->CreateBuffer(&bd, nullptr, &st)) && st) {
                immediate->CopyResource(st, best->stand_in);
                D3D11_MAPPED_SUBRESOURCE m{};
                if (SUCCEEDED(immediate->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
                    const bool same_e = std::memcmp(m.pData, best->data[eye & 1].data(), bd.ByteWidth) == 0;
                    const bool same_o = std::memcmp(m.pData, best->data[(eye & 1) ^ 1].data(), bd.ByteWidth) == 0;
                    spdlog::info("[Rec] verify eye {}: stand-in {} B {} the eye's data ({} the other eye's); {} stand-ins", eye, bd.ByteWidth, same_e ? "matches" : "DIFFERS from",
                                 same_o ? "matches" : "differs from", g_variants.size());
                    immediate->Unmap(st, 0);
                }
                st->Release();
            }
        }
    }

    size_t variants() { return g_variants.size(); }
    bool recording() { return g_recording.load(); }

    bool begin(ID3D11DeviceContext* immediate)
    {
        if (!g_ready || immediate != g_immediate || g_recording) return false;
        const auto t0 = std::chrono::steady_clock::now();
        ID3D11DeviceContext1* imm1 = nullptr;
        immediate->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&imm1));
        g_start.capture(imm1);
        imm1->Release();
        g_start.apply(g_deferred);
        g_variants.clear();
        g_subst.clear();
        g_pool_used.clear();
        for (int s = 0; s < 6; ++s) {
            for (UINT i = 0; i < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; ++i) {
                g_bound[s][i] = { g_start.cbs[s][i], g_start.cb_first[s][i], g_start.cb_num[s][i], g_start.cb_num[s][i] > 0 && g_start.cb_num[s][i] < 4096 };
            }
        }
        g_start.release();
        g_mapped_on_immediate.clear();
        g_deferred_queries.clear();
        g_open_def_maps = 0;
        g_rec_tid = GetCurrentThreadId();
        *reinterpret_cast<void***>(immediate) = g_vtable;
        g_recording = true;
        g_rec_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        return true;
    }

    ID3D11CommandList* end(ID3D11DeviceContext* immediate)
    {
        if (!g_recording) return nullptr;
        *reinterpret_cast<void***>(immediate) = g_orig_vtable;
        g_rec_tid = 0;
        g_recording = false;
        const auto t0 = std::chrono::steady_clock::now();
        g_end.capture(g_deferred);
        ID3D11CommandList* cl = nullptr;
        const HRESULT      hr = g_deferred->FinishCommandList(FALSE, &cl);
        if (FAILED(hr)) spdlog::error("[Rec] FinishCommandList failed {:08x}", (uint32_t)hr);
        if (g_log_end > 0) {
            g_log_end--;
            spdlog::info("[Rec] end: list {}, still mapped on the immediate context {}, deferred maps left open {}, device removed reason {:08x}", (void*)cl, g_mapped_on_immediate.size(), g_open_def_maps,
                         (uint32_t)g_device->GetDeviceRemovedReason());
        }
        g_recordings++;
        g_rec_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        return cl;
    }

    void apply_end_state(ID3D11DeviceContext* immediate, bool release)
    {
        ID3D11DeviceContext1* imm1 = nullptr;
        immediate->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&imm1));
        if (imm1) {
            g_end.apply(imm1);
            imm1->Release();
        }
        if (release) g_end.release();
    }

    std::string stats_line()
    {
        const auto s = fmt::format("{:.1f} eye buffers per recording; {} recordings, maps deferred {} (no-overwrite {}), on the immediate {}, failed {}, queries on the immediate {}, in-recording query reads answered {}, state copy {:.2f} ms per recording", g_recordings ? double(g_variant_count) / double(g_recordings) : 0.0, g_recordings,
                                   g_maps_def, g_nooverwrite, g_maps_imm, g_map_fail, g_queries_imm, g_fake_getdata, g_recordings ? g_rec_ms / double(g_recordings) : 0.0);
        g_recordings = g_maps_def = g_nooverwrite = g_maps_imm = 0;
        g_rec_ms                                  = 0.0;
        g_variant_count                           = 0;
        return s;
    }
}

namespace fc5::rec
{
    // Bisect test: does a list cut after the N-th recorded draw execute? A red clear of a small test
    // texture is appended, the list executed, the texture read back.
    namespace
    {
        std::atomic<int> g_bisect_at{ -1 };
        int              g_bisect_draws{ 0 };
        int              g_bisect_window{ 0 };
        ID3D11Texture2D* g_bisect_tex{ nullptr };
        ID3D11RenderTargetView* g_bisect_rtv{ nullptr };
        ID3D11Texture2D* g_bisect_staging{ nullptr };
    }

    void bisect(int n) { g_bisect_at = n; }
    void bisect_window(int n) { g_bisect_window = n; }

    void bisect_on_draw(ID3D11DeviceContext* ctx)
    {
        if (!g_recording || !is_deferred(ctx)) return;
        if (g_bisect_window > 0 && g_bisect_draws + 1 == g_bisect_window) std::memcpy(g_slot_snap, g_slot_count, sizeof(g_slot_count));
        if (g_bisect_window > 0 && g_bisect_draws + 1 == g_bisect_window + 1) {
            std::string s;
            for (int i = 0; i < kVtableSize; ++i) if (g_slot_count[i] != g_slot_snap[i]) s += fmt::format(" #{}x{}", i, g_slot_count[i] - g_slot_snap[i]);
            spdlog::info("[Rec] methods recorded between draw {} and {}:{}", g_bisect_window, g_bisect_window + 1, s);
            g_bisect_window = 0;
        }
        if (g_bisect_at.load() < 0) { ++g_bisect_draws; return; }
        if (++g_bisect_draws < g_bisect_at.load()) return;
        const int n   = g_bisect_draws;
        g_bisect_at   = -1;
        if (!g_bisect_tex) {
            D3D11_TEXTURE2D_DESC d{};
            d.Width = d.Height = 4; d.MipLevels = d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET;
            g_device->CreateTexture2D(&d, nullptr, &g_bisect_tex);
            if (g_bisect_tex) g_device->CreateRenderTargetView(g_bisect_tex, nullptr, &g_bisect_rtv);
            d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            g_device->CreateTexture2D(&d, nullptr, &g_bisect_staging);
        }
        if (!g_bisect_rtv || !g_bisect_staging) return;
        // Black now on the immediate context, red at the end of the cut list.
        const float black[4]{ 0, 0, 0, 1 }, red[4]{ 1, 0, 0, 1 };
        reinterpret_cast<void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11RenderTargetView*, const FLOAT*)>(g_orig_vtable[50])(g_immediate, g_bisect_rtv, black);
        g_deferred->ClearRenderTargetView(g_bisect_rtv, red);
        ID3D11CommandList* cl = end(g_immediate);
        if (cl) {
            g_immediate->ExecuteCommandList(cl, FALSE);
            cl->Release();
        }
        apply_end_state(g_immediate, true);
        g_immediate->CopyResource(g_bisect_staging, g_bisect_tex);
        D3D11_MAPPED_SUBRESOURCE m{};
        uint32_t px = 0;
        if (SUCCEEDED(g_immediate->Map(g_bisect_staging, 0, D3D11_MAP_READ, 0, &m))) {
            px = *static_cast<const uint32_t*>(m.pData);
            g_immediate->Unmap(g_bisect_staging, 0);
        }
        spdlog::info("[Rec] bisect: list cut after {} recorded draws {} (test pixel {:08x})", n, (px & 0xff) == 0xff ? "EXECUTED" : "did NOT execute", px);
    }

    void bisect_reset() { g_bisect_draws = 0; }
}

namespace fc5::rec
{
    // Test: every thread's calls recorded (the thread check's jumps patched to fall through) or only the
    // recording thread's.
    void all_threads(bool on)
    {
        if (g_jne_disp.empty()) for (auto* s : g_jne_sites) g_jne_disp.push_back(s[1]);
        for (size_t i = 0; i < g_jne_sites.size(); ++i) {
            g_jne_sites[i][0] = on ? 0x90 : 0x75;
            g_jne_sites[i][1] = on ? 0x90 : g_jne_disp[i];
        }
        FlushInstructionCache(GetCurrentProcess(), g_thunks, kVtableSize * 96);
        spdlog::info("[Rec] calls from {} recorded", on ? "every thread" : "the recording thread only");
    }
}
