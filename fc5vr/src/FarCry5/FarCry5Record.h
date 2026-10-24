#pragma once

// Stereo research: the scene's D3D11 work recorded into a command list (the game's immediate context
// is redirected to a deferred context while the scene function runs), so it can be executed once per
// eye. Phase 1: record and execute once, to prove the game renders through a command list.

#include <d3d11_4.h>
#include <cstdint>
#include <string>

namespace fc5::rec
{
    bool initialize(ID3D11Device* device, ID3D11DeviceContext* immediate);
    bool ready();
    // Starts recording: the immediate context's current state is copied into the deferred context and
    // every call the game makes on the immediate context goes to the deferred one.
    bool begin(ID3D11DeviceContext* immediate);
    // Stops recording; returns the command list (caller releases) and keeps the deferred context's
    // final state for apply_end_state().
    ID3D11CommandList* end(ID3D11DeviceContext* immediate);
    // After ExecuteCommandList(cl, FALSE) the immediate context is reset: this puts the state the
    // scene left behind back, as the game expects.
    void apply_end_state(ID3D11DeviceContext* immediate, bool release = true);
    bool recording();
    // The mod's own context hooks also see the recorded calls: the handler for vtable slot idx is
    // called with the deferred context, and calls the deferred context's method for the original.
    void  set_handler(int idx, void* fn);
    bool  is_deferred(const void* ctx);
    void* deferred_method(int idx);
    // Stereo: eye-dependent constant buffers (left and right contents of a buffer the game just
    // mapped, while recording) and execution per eye.
    void   set_stereo(bool on);
    void   verify(int n);
    void   poison(bool on);
    void   log_end(int n);
    void   route_queries(bool on);
    void   bisect(int n);
    void   all_threads(bool on);
    void   bisect_window(int n);
    void   bisect_on_draw(ID3D11DeviceContext* ctx);
    void   bisect_reset();
    void   no_predication(bool on);
    void   red_test(ID3D11DeviceContext* immediate, ID3D11Texture2D* target);
    bool   stereo_capture();
    void   register_variant(ID3D11Buffer* buf, const void* left, const void* right, uint32_t size);
    void   execute_eye(ID3D11DeviceContext* immediate, ID3D11CommandList* cl, int eye);
    size_t variants();
    std::string stats_line();
}
