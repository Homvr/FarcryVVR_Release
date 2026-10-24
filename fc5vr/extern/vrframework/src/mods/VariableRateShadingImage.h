#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <d3d12.h>

#include <Mod.hpp>
#include <mods/vr/d3d12/ComPtr.hpp>
#include <mods/vr/d3d12/CommandContext.hpp>

#ifndef VRS_MAX_TRACKED_RTVS
#define VRS_MAX_TRACKED_RTVS 1024
#endif

using Microsoft::WRL::ComPtr;

/**
 * Needs to be added before VR
 */
class VariableRateShadingImage : public Mod {
public:
    enum class LensDevice : uint8_t {
        Generic = 0,
        OculusRiftCV1,
        HTCVive,
        HTCVivePro,
        Count
    };

    enum class LensQuality : uint8_t {
        Conservative = 0,
        Balanced,
        Aggressive,
        Count
    };

    struct LensConfiguration {
        float warpLeft{0.0f};
        float warpRight{0.0f};
        float warpUp{0.0f};
        float warpDown{0.0f};
        float relativeSizeLeft{0.5f};
        float relativeSizeRight{0.5f};
        float relativeSizeUp{0.5f};
        float relativeSizeDown{0.5f};
    };

    struct UpdateConfig {
        float fineRadius{0.2f};
        float mediumRadius{0.5f};
        bool enableLensOptimization{false};
        LensDevice lensDevice{LensDevice::Generic};
        LensQuality lensQuality{LensQuality::Balanced};
    };

    struct ResourceSlot {
//        static constexpr uint32_t kMaxActiveContexts = 2;
//        int  activeContext{0};
        struct D3D12ResourceWrapper
        {
            bool dirty{true};
            bool lost{true};
            int frameLastUsed{0};
            ComPtr<ID3D12Resource> texture{};
            ComPtr<ID3D12Resource> upload{};
#if defined(_DEBUG)
            ComPtr<ID3D12Resource>       debugTexture{};
            ComPtr<ID3D12Resource>       debugUpload{};
            ComPtr<ID3D12DescriptorHeap> debugSrvHeap{};
            D3D12_CPU_DESCRIPTOR_HANDLE  debugSrvCpuHandle{};
            D3D12_GPU_DESCRIPTOR_HANDLE  debugSrvGpuHandle{};
#endif
            D3D12_RESOURCE_STATES state{ D3D12_RESOURCE_STATE_COMMON };
            inline void Reset()
            {
                texture.Reset();
                upload.Reset();
                state = D3D12_RESOURCE_STATE_COMMON;
                dirty = true;
                frameLastUsed = 0;
#if defined(_DEBUG)
                debugTexture.Reset();
                debugUpload.Reset();
                debugSrvHeap.Reset();
                debugSrvCpuHandle = {};
                debugSrvGpuHandle = {};
#endif
            };
            inline void transition(d3d12::CommandContext& ctx, D3D12_RESOURCE_STATES targetState)
            {
                if (!texture || state == targetState) {
                    return;
                }
                D3D12_RESOURCE_BARRIER barrier{};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                barrier.Transition.pResource = texture.Get();
                barrier.Transition.StateBefore = state;
                barrier.Transition.StateAfter = targetState;
                barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                ctx.cmd_list->ResourceBarrier(1, &barrier);
                state = targetState;
                ctx.has_commands = true;
            };
        } resource{};

        UINT tilesX{0};
        UINT tilesY{0};

        inline void Reset() {
//            for(auto & context : resource) {
                resource.Reset();
//            }
            tilesX = 0;
            tilesY = 0;
//            activeContext = 0;
        }
//        inline void MarkDirty() {
////            resource[activeContext].dirty = true;
//            resource.dirty = true;
////            activeContext = (activeContext + 1) % kMaxActiveContexts;
//        }
        [[nodiscard]] inline const bool IsReady() const {
//            return resource[activeContext].dirty;
            return !resource.dirty && !resource.lost;
        }
//        inline auto& GetActiveResource() {
////            return resource[activeContext];
//            return resource;
//        }

//        inline const auto& GetActiveResource() const {
////            return resource[activeContext];
//            return resource;
//        }

        inline int MatchResolution(UINT inTilesX, UINT inTilesY) const {
            if (tilesX == inTilesX && tilesY == inTilesY) {
                return 0;
            } else if(tilesX*tilesY < inTilesX * inTilesY) {
                return 1;
            }
            return -1;
        }
    };

    inline static std::shared_ptr<VariableRateShadingImage>& Get() {
        static auto instance = std::make_shared<VariableRateShadingImage>();
        return instance;
    }

    std::optional<std::string> on_initialize() override;
    void on_device_reset() override;
    void on_d3d12_initialize(ID3D12Device4 *pDevice4, D3D12_RESOURCE_DESC &desc) override;
    void on_draw_ui() override;
    void on_config_load(const utility::Config& cfg, bool set_defaults) override;
    void on_config_save(utility::Config& cfg) override;
    void on_d3d12_set_scissor_rects(ID3D12GraphicsCommandList5 *cmd_list, UINT num_rects, const D3D12_RECT *rects) override;
    void on_d3d12_set_render_targets(ID3D12GraphicsCommandList5 *cmd_list, UINT num_rtvs, const D3D12_CPU_DESCRIPTOR_HANDLE *rtvs, BOOL single_handle, D3D12_CPU_DESCRIPTOR_HANDLE *dsv) override;
    void on_d3d12_create_render_target_view(ID3D12Device *device, ID3D12Resource *pResource, const D3D12_RENDER_TARGET_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override;
    void on_frame() override;

    bool Setup(ID3D12Device* device);
    void Update(/*const UpdateConfig& config*/);

    void Reset();

    [[nodiscard]] bool isInitialized() const {
        return m_initialized;
    }

    [[nodiscard]] ID3D12Resource* GetResource(UINT rtWidth, UINT rtHeight);
    void populateTileData(UINT tilesX, UINT tilesY, std::vector<uint8_t>& outData);

    std::string_view get_name() const override {
        return "VRS";
    }

#if defined(_DEBUG)
    const ResourceSlot* GetLargestDebugSlot() const;
#endif

private:
    struct RTVDesc {
        int w{0};
        int h{0};
    };
    class LRUCache {
        struct Node {
            uintptr_t key{0};
            RTVDesc val{};
            Node* prev{};
            Node* next{};
        };

        int size;
        int capacity;
        Node* head;
        Node* tail;
        std::unordered_map<uintptr_t, Node*> cache;
        std::mutex m_lru_mutex{};


        void removeNode(Node* node) {
            node->prev->next = node->next;
            node->next->prev = node->prev;
        }

        void addToHead(Node* node) {
            node->next = head->next;
            node->prev = head;
            head->next->prev = node;
            head->next = node;
        }

    public:
        explicit LRUCache(const int capacity)
            : size(0)
            , capacity(capacity) {
            head = new Node();
            tail = new Node();
            head->next = tail;
            tail->prev = head;
        }

        bool get(const uintptr_t key, RTVDesc& outVal) {
            std::scoped_lock _(m_lru_mutex);
            if (!cache.contains(key)) {
                return false;
            }
            Node* node = cache[key];
            outVal = node->val;
            removeNode(node);
            addToHead(node);
            return true;
        }

        void put(const uintptr_t key, const RTVDesc value) {
            std::scoped_lock _(m_lru_mutex);
            if (cache.contains(key)) {
                Node* node = cache[key];
                node->val = value;
                removeNode(node);
                addToHead(node);
            } else {
                auto node = new Node();
                node->key = key;
                node->val = value;
                cache[key] = node;
                addToHead(node);
                if (++size > capacity) {
                    node = tail->prev;
                    cache.erase(node->key);
                    removeNode(node);
                    delete node;
                    --size;
                }
            }
        }
    };

    int findSlot(UINT tilesX, UINT tilesY, int& outSlot);
    bool recreateResources(ResourceSlot::D3D12ResourceWrapper& resource, UINT tilesX, UINT tilesY) const;
    bool updateContents(ResourceSlot::D3D12ResourceWrapper& resource, UINT i, UINT i1);
    void generateImagePattern(UINT tilesX, UINT tilesY, int i);

    static constexpr uint32_t kMaxSlots = 2;
    ResourceSlot m_slots[kMaxSlots]{};
    UINT m_tileSize{0};

    float m_radii[2]{0.0f, 0.0f};
    bool m_initialized{false};
    bool m_supportsAdditionalRates{false};
    int frameCount{0};

    d3d12::CommandContext m_commandContext{};
    std::mutex m_mtx{};

    LRUCache m_rtv{VRS_MAX_TRACKED_RTVS};

    const ModSlider::Ptr m_fine_radius{ ModSlider::create(generate_name("VRSFineRadius"), 0.0f, 1.0f, 0.7f) };
    const ModSlider::Ptr m_coarse_radius{ ModSlider::create(generate_name("VRSCoarseRadius"), 0.0f, 1.0f, 0.7f) };
//    const ModToggle::Ptr m_g_buffer_vrs{ ModToggle::create(generate_name("VRSAggressive"), true) };
    const ModToggle::Ptr m_enabled{ ModToggle::create(generate_name("VRSEnabled"), false) };
    ValueList m_options{
        *m_fine_radius,
        *m_coarse_radius,
//        *m_g_buffer_vrs,
        *m_enabled
    };
};
