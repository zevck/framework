#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <Ultralight/platform/GPUDriver.h>
#include <map>
#include <vector>
#include <memory>

using Microsoft::WRL::ComPtr;

namespace PrismaUI {


// Standalone D3D11 GPUDriver for Ultralight that uses an existing D3D11 device/context.
// No GPUContextD3D11 wrapper needed - takes raw device/context from the game.

class GPUDriverD3D11 : public ultralight::GPUDriver {
public:
    GPUDriverD3D11(ID3D11Device* device, ID3D11DeviceContext* context);
    virtual ~GPUDriverD3D11();

    // ── GPUDriver interface ─────────────────────────────────────
    void BeginSynchronize() override;
    void EndSynchronize() override;
    uint32_t NextTextureId() override;
    uint32_t NextRenderBufferId() override;
    uint32_t NextGeometryId() override;
    void UpdateCommandList(const ultralight::CommandList& list) override;

    void CreateTexture(uint32_t texture_id, ultralight::RefPtr<ultralight::Bitmap> bitmap) override;
    void UpdateTexture(uint32_t texture_id, ultralight::RefPtr<ultralight::Bitmap> bitmap) override;
    void DestroyTexture(uint32_t texture_id) override;

    void CreateRenderBuffer(uint32_t render_buffer_id, const ultralight::RenderBuffer& buffer) override;
    void DestroyRenderBuffer(uint32_t render_buffer_id) override;

    void CreateGeometry(uint32_t geometry_id, const ultralight::VertexBuffer& vertices,
                        const ultralight::IndexBuffer& indices) override;
    void UpdateGeometry(uint32_t geometry_id, const ultralight::VertexBuffer& vertices,
                        const ultralight::IndexBuffer& indices) override;
    void DestroyGeometry(uint32_t geometry_id) override;

    // PrismaUI integration
    bool HasCommandsPending() const { return !command_list_.empty(); }
    void DrawCommandList();

    // Get the SRV for a texture (used by ViewRenderer to composite onto game)
    ID3D11ShaderResourceView* GetTextureSRV(uint32_t texture_id);

private:
    void LoadShaders();
    void BindShader(ultralight::ShaderType shader);
    void BindVertexLayout(ultralight::VertexBufferFormat format);
    void BindGeometry(uint32_t id);
    void BindTexture(uint8_t texture_unit, uint32_t texture_id);
    void BindRenderBuffer(uint32_t render_buffer_id);
    void ClearRenderBuffer(uint32_t render_buffer_id);
    void DrawGeometry(uint32_t geometry_id, uint32_t indices_count,
                      uint32_t indices_offset, const ultralight::GPUState& state);
    void SetViewport(uint32_t width, uint32_t height);
    void UpdateConstantBuffer(const ultralight::GPUState& state);

    ID3D11Device* device_;
    ID3D11DeviceContext* context_;

    // Shaders
    ComPtr<ID3D11InputLayout> vertex_layout_2f_4ub_2f_;
    ComPtr<ID3D11InputLayout> vertex_layout_2f_4ub_2f_2f_28f_;
    ComPtr<ID3D11SamplerState> sampler_state_;
    ComPtr<ID3D11Buffer> constant_buffer_;
    ComPtr<ID3D11BlendState> blend_state_;
    ComPtr<ID3D11BlendState> disabled_blend_state_;
    ComPtr<ID3D11RasterizerState> rasterizer_state_;
    ComPtr<ID3D11RasterizerState> scissored_rasterizer_state_;

    typedef std::map<ultralight::ShaderType, std::pair<ComPtr<ID3D11VertexShader>, ComPtr<ID3D11PixelShader>>> ShaderMap;
    ShaderMap shaders_;

    // Geometry
    struct GeometryEntry {
        ultralight::VertexBufferFormat format;
        ComPtr<ID3D11Buffer> vertexBuffer;
        ComPtr<ID3D11Buffer> indexBuffer;
    };
    std::map<uint32_t, GeometryEntry> geometry_;

    // Textures
    struct TextureEntry {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> texture_srv;
        bool is_msaa_render_target = false;
        bool needs_resolve = false;
        // MSAA resolve target (single-sample copy for shader reads)
        ComPtr<ID3D11Texture2D> resolve_texture;
        ComPtr<ID3D11ShaderResourceView> resolve_texture_srv;
    };
    std::map<uint32_t, TextureEntry> textures_;

    // Render targets
    struct RenderTargetEntry {
        ComPtr<ID3D11RenderTargetView> render_target_view;
        uint32_t render_target_texture_id;
    };
    std::map<uint32_t, RenderTargetEntry> render_targets_;

    // Command list & IDs
    std::vector<ultralight::Command> command_list_;
    uint32_t next_texture_id_ = 1;
    uint32_t next_render_buffer_id_ = 1;
    uint32_t next_geometry_id_ = 1;
};

}
