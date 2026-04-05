#include "GPUDriverD3D11.h"
#include <DirectXMath.h>
#include <directxcolors.h>
#include <string>
#include <Ultralight/platform/Platform.h>
#include <Ultralight/Geometry.h>
#include <Ultralight/Matrix.h>

// Pre-compiled shader bytecode from Ultralight SDK
#include "d3d11/shaders.h"

namespace {

struct Uniforms {
    DirectX::XMFLOAT4 State;
    DirectX::XMMATRIX Transform;
    DirectX::XMINT4 Integer4[2];
    DirectX::XMFLOAT4 Scalar4[2];
    DirectX::XMFLOAT4 Vector[8];
    DirectX::XMINT4 ClipData;
    DirectX::XMMATRIX Clip[8];
};

} // namespace

namespace PrismaUI {

using namespace ultralight;

GPUDriverD3D11::GPUDriverD3D11(ID3D11Device* device, ID3D11DeviceContext* context)
    : device_(device), context_(context) {

    // Create blend states
    D3D11_BLEND_DESC blend_desc;
    ZeroMemory(&blend_desc, sizeof(blend_desc));
    blend_desc.AlphaToCoverageEnable = FALSE;
    blend_desc.IndependentBlendEnable = FALSE;
    blend_desc.RenderTarget[0].BlendEnable = TRUE;
    blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_INV_DEST_ALPHA;
    blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device_->CreateBlendState(&blend_desc, blend_state_.GetAddressOf());

    blend_desc.RenderTarget[0].BlendEnable = FALSE;
    device_->CreateBlendState(&blend_desc, disabled_blend_state_.GetAddressOf());

    // Rasterizer states
    D3D11_RASTERIZER_DESC raster_desc;
    ZeroMemory(&raster_desc, sizeof(raster_desc));
    raster_desc.FillMode = D3D11_FILL_SOLID;
    raster_desc.CullMode = D3D11_CULL_NONE;
    raster_desc.FrontCounterClockwise = FALSE;
    raster_desc.DepthClipEnable = FALSE;
    raster_desc.ScissorEnable = FALSE;
    device_->CreateRasterizerState(&raster_desc, rasterizer_state_.GetAddressOf());

    raster_desc.ScissorEnable = TRUE;
    device_->CreateRasterizerState(&raster_desc, scissored_rasterizer_state_.GetAddressOf());
}

GPUDriverD3D11::~GPUDriverD3D11() {}

// GPUDriver interface (ID/sync/commands)

void GPUDriverD3D11::BeginSynchronize() {}
void GPUDriverD3D11::EndSynchronize() {}

uint32_t GPUDriverD3D11::NextTextureId() { return next_texture_id_++; }
uint32_t GPUDriverD3D11::NextRenderBufferId() { return next_render_buffer_id_++; }
uint32_t GPUDriverD3D11::NextGeometryId() { return next_geometry_id_++; }

void GPUDriverD3D11::UpdateCommandList(const CommandList& list) {
    if (list.size) {
        command_list_.resize(list.size);
        memcpy(command_list_.data(), list.commands, sizeof(Command) * list.size);
    }
}

void GPUDriverD3D11::DrawCommandList() {
    if (command_list_.empty()) return;

    for (auto& cmd : command_list_) {
        if (cmd.command_type == CommandType::DrawGeometry) {
            DrawGeometry(cmd.geometry_id, cmd.indices_count, cmd.indices_offset, cmd.gpu_state);
        } else if (cmd.command_type == CommandType::ClearRenderBuffer) {
            ClearRenderBuffer(cmd.gpu_state.render_buffer_id);
        }
    }
    command_list_.clear();
}

// Textures

void GPUDriverD3D11::CreateTexture(uint32_t texture_id, RefPtr<Bitmap> bitmap) {
    auto& entry = textures_[texture_id];

    D3D11_TEXTURE2D_DESC desc;
    ZeroMemory(&desc, sizeof(desc));
    desc.Width = bitmap->width();
    desc.Height = bitmap->height();
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = bitmap->format() == BitmapFormat::BGRA8_UNORM_SRGB
                      ? DXGI_FORMAT_B8G8R8A8_UNORM
                      : DXGI_FORMAT_A8_UNORM;
    desc.SampleDesc.Count = 1;

    if (bitmap->IsEmpty()) {
        // Render target texture - use 4x MSAA for anti-aliased rendering
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.CPUAccessFlags = 0;
        desc.SampleDesc.Count = 4;
        desc.SampleDesc.Quality = D3D11_STANDARD_MULTISAMPLE_PATTERN;
        entry.is_msaa_render_target = true;

        device_->CreateTexture2D(&desc, nullptr, entry.texture.GetAddressOf());

        // SRV for the MSAA texture (used internally by Ultralight shaders)
        D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc;
        ZeroMemory(&srv_desc, sizeof(srv_desc));
        srv_desc.Format = desc.Format;
        srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
        device_->CreateShaderResourceView(entry.texture.Get(), &srv_desc, entry.texture_srv.GetAddressOf());

        // Create single-sample resolve texture for final output
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        device_->CreateTexture2D(&desc, nullptr, entry.resolve_texture.GetAddressOf());

        D3D11_SHADER_RESOURCE_VIEW_DESC resolve_srv_desc;
        ZeroMemory(&resolve_srv_desc, sizeof(resolve_srv_desc));
        resolve_srv_desc.Format = desc.Format;
        resolve_srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        resolve_srv_desc.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(entry.resolve_texture.Get(), &resolve_srv_desc, entry.resolve_texture_srv.GetAddressOf());
    } else {
        // Regular texture with initial data
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        D3D11_SUBRESOURCE_DATA tex_data;
        ZeroMemory(&tex_data, sizeof(tex_data));
        tex_data.pSysMem = bitmap->LockPixels();
        tex_data.SysMemPitch = bitmap->row_bytes();
        device_->CreateTexture2D(&desc, &tex_data, entry.texture.GetAddressOf());
        bitmap->UnlockPixels();

        D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc;
        ZeroMemory(&srv_desc, sizeof(srv_desc));
        srv_desc.Format = desc.Format;
        srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(entry.texture.Get(), &srv_desc, entry.texture_srv.GetAddressOf());
    }
}

void GPUDriverD3D11::UpdateTexture(uint32_t texture_id, RefPtr<Bitmap> bitmap) {
    auto i = textures_.find(texture_id);
    if (i == textures_.end()) return;

    auto& entry = i->second;
    D3D11_MAPPED_SUBRESOURCE res;
    context_->Map(entry.texture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &res);

    if (res.RowPitch == bitmap->row_bytes()) {
        memcpy(res.pData, bitmap->LockPixels(), bitmap->size());
        bitmap->UnlockPixels();
    } else {
        RefPtr<Bitmap> mapped_bitmap = Bitmap::Create(
            bitmap->width(), bitmap->height(), bitmap->format(),
            res.RowPitch, res.pData, res.RowPitch * bitmap->height(), false);
        IntRect dest_rect = {0, 0, (int)bitmap->width(), (int)bitmap->height()};
        mapped_bitmap->DrawBitmap(dest_rect, dest_rect, bitmap, false);
    }

    context_->Unmap(entry.texture.Get(), 0);
}

void GPUDriverD3D11::DestroyTexture(uint32_t texture_id) {
    textures_.erase(texture_id);
}

// Render Buffers

void GPUDriverD3D11::CreateRenderBuffer(uint32_t render_buffer_id, const RenderBuffer& buffer) {
    auto tex_entry = textures_.find(buffer.texture_id);
    if (tex_entry == textures_.end()) return;

    D3D11_RENDER_TARGET_VIEW_DESC rtv_desc;
    ZeroMemory(&rtv_desc, sizeof(rtv_desc));
    rtv_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    rtv_desc.ViewDimension = tex_entry->second.is_msaa_render_target
        ? D3D11_RTV_DIMENSION_TEXTURE2DMS
        : D3D11_RTV_DIMENSION_TEXTURE2D;

    auto& rt_entry = render_targets_[render_buffer_id];
    device_->CreateRenderTargetView(tex_entry->second.texture.Get(), &rtv_desc,
                                    rt_entry.render_target_view.GetAddressOf());
    rt_entry.render_target_texture_id = buffer.texture_id;
}

void GPUDriverD3D11::DestroyRenderBuffer(uint32_t render_buffer_id) {
    render_targets_.erase(render_buffer_id);
}

// Geometry

void GPUDriverD3D11::CreateGeometry(uint32_t geometry_id, const VertexBuffer& vertices,
                                    const IndexBuffer& indices) {
    if (geometry_.count(geometry_id)) return;

    GeometryEntry geo;
    geo.format = vertices.format;

    D3D11_BUFFER_DESC vb_desc;
    ZeroMemory(&vb_desc, sizeof(vb_desc));
    vb_desc.Usage = D3D11_USAGE_DYNAMIC;
    vb_desc.ByteWidth = vertices.size;
    vb_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    D3D11_SUBRESOURCE_DATA vb_data = {};
    vb_data.pSysMem = vertices.data;
    device_->CreateBuffer(&vb_desc, &vb_data, geo.vertexBuffer.GetAddressOf());

    D3D11_BUFFER_DESC ib_desc;
    ZeroMemory(&ib_desc, sizeof(ib_desc));
    ib_desc.Usage = D3D11_USAGE_DYNAMIC;
    ib_desc.ByteWidth = indices.size;
    ib_desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    ib_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    D3D11_SUBRESOURCE_DATA ib_data = {};
    ib_data.pSysMem = indices.data;
    device_->CreateBuffer(&ib_desc, &ib_data, geo.indexBuffer.GetAddressOf());

    geometry_.insert({geometry_id, std::move(geo)});
}

void GPUDriverD3D11::UpdateGeometry(uint32_t geometry_id, const VertexBuffer& vertices,
                                    const IndexBuffer& indices) {
    auto i = geometry_.find(geometry_id);
    if (i == geometry_.end()) return;

    D3D11_MAPPED_SUBRESOURCE res;
    context_->Map(i->second.vertexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &res);
    memcpy(res.pData, vertices.data, vertices.size);
    context_->Unmap(i->second.vertexBuffer.Get(), 0);

    context_->Map(i->second.indexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &res);
    memcpy(res.pData, indices.data, indices.size);
    context_->Unmap(i->second.indexBuffer.Get(), 0);
}

void GPUDriverD3D11::DestroyGeometry(uint32_t geometry_id) {
    geometry_.erase(geometry_id);
}

// Public helpers

ID3D11ShaderResourceView* GPUDriverD3D11::GetTextureSRV(uint32_t texture_id) {
    auto i = textures_.find(texture_id);
    if (i == textures_.end()) return nullptr;

    auto& entry = i->second;
    if (entry.is_msaa_render_target) {
        // Always resolve before returning - SpriteBatch needs single-sample texture
        context_->ResolveSubresource(
            entry.resolve_texture.Get(), 0,
            entry.texture.Get(), 0,
            DXGI_FORMAT_B8G8R8A8_UNORM);
        entry.needs_resolve = false;
        return entry.resolve_texture_srv.Get();
    }
    return entry.texture_srv.Get();
}

// Internal rendering

void GPUDriverD3D11::LoadShaders() {
    if (!shaders_.empty()) return;

    // Path vertex layout
    const D3D11_INPUT_ELEMENT_DESC layout_2f_4ub_2f[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };

    // Quad vertex layout
    const D3D11_INPUT_ELEMENT_DESC layout_2f_4ub_2f_2f_28f[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 4, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 5, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 6, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 7, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };

    auto loadVS = [&](const unsigned char* data, unsigned int len,
                      ComPtr<ID3D11VertexShader>& vs,
                      const D3D11_INPUT_ELEMENT_DESC* layout, UINT count,
                      ComPtr<ID3D11InputLayout>& il) {
        device_->CreateVertexShader(data, len, nullptr, vs.GetAddressOf());
        device_->CreateInputLayout(layout, count, data, len, il.GetAddressOf());
    };

    auto loadPS = [&](const unsigned char* data, unsigned int len, ComPtr<ID3D11PixelShader>& ps) {
        device_->CreatePixelShader(data, len, nullptr, ps.GetAddressOf());
    };

    // FillPath (path vertex shader)
    auto& sp = shaders_[ShaderType::FillPath];
    loadVS(vertex_path_vs_data, vertex_path_vs_size, sp.first,
           layout_2f_4ub_2f, ARRAYSIZE(layout_2f_4ub_2f), vertex_layout_2f_4ub_2f_);
    loadPS(fill_path_ps_data, fill_path_ps_size, sp.second);

    // Fill (quad vertex shader)
    auto& sf = shaders_[ShaderType::Fill];
    loadVS(vertex_quad_vs_data, vertex_quad_vs_size, sf.first,
           layout_2f_4ub_2f_2f_28f, ARRAYSIZE(layout_2f_4ub_2f_2f_28f), vertex_layout_2f_4ub_2f_2f_28f_);
    loadPS(fill_ps_data, fill_ps_size, sf.second);

    // FilterBasic
    auto& sfb = shaders_[ShaderType::FilterBasic];
    loadVS(vertex_quad_vs_data, vertex_quad_vs_size, sfb.first,
           layout_2f_4ub_2f_2f_28f, ARRAYSIZE(layout_2f_4ub_2f_2f_28f), vertex_layout_2f_4ub_2f_2f_28f_);
    loadPS(filter_basic_ps_data, filter_basic_ps_size, sfb.second);

    // FilterBlur
    auto& sbl = shaders_[ShaderType::FilterBlur];
    loadVS(vertex_quad_vs_data, vertex_quad_vs_size, sbl.first,
           layout_2f_4ub_2f_2f_28f, ARRAYSIZE(layout_2f_4ub_2f_2f_28f), vertex_layout_2f_4ub_2f_2f_28f_);
    loadPS(filter_blur_ps_data, filter_blur_ps_size, sbl.second);

    // FilterDropShadow
    auto& sds = shaders_[ShaderType::FilterDropShadow];
    loadVS(vertex_quad_vs_data, vertex_quad_vs_size, sds.first,
           layout_2f_4ub_2f_2f_28f, ARRAYSIZE(layout_2f_4ub_2f_2f_28f), vertex_layout_2f_4ub_2f_2f_28f_);
    loadPS(filter_dropshadow_ps_data, filter_dropshadow_ps_size, sds.second);
}

void GPUDriverD3D11::BindShader(ShaderType shader) {
    LoadShaders();
    auto i = shaders_.find(shader);
    if (i == shaders_.end()) return;
    context_->VSSetShader(i->second.first.Get(), nullptr, 0);
    context_->PSSetShader(i->second.second.Get(), nullptr, 0);
}

void GPUDriverD3D11::BindVertexLayout(VertexBufferFormat format) {
    LoadShaders();
    if (format == VertexBufferFormat::_2f_4ub_2f)
        context_->IASetInputLayout(vertex_layout_2f_4ub_2f_.Get());
    else
        context_->IASetInputLayout(vertex_layout_2f_4ub_2f_2f_28f_.Get());
}

void GPUDriverD3D11::BindGeometry(uint32_t id) {
    auto i = geometry_.find(id);
    if (i == geometry_.end()) return;

    auto& geo = i->second;
    UINT stride = geo.format == VertexBufferFormat::_2f_4ub_2f ? 20 : 140; // sizeof vertex structs
    UINT offset = 0;
    context_->IASetVertexBuffers(0, 1, geo.vertexBuffer.GetAddressOf(), &stride, &offset);
    context_->IASetIndexBuffer(geo.indexBuffer.Get(), DXGI_FORMAT_R32_UINT, 0);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    BindVertexLayout(geo.format);
}

void GPUDriverD3D11::BindTexture(uint8_t texture_unit, uint32_t texture_id) {
    auto i = textures_.find(texture_id);
    if (i == textures_.end()) return;

    auto& entry = i->second;
    if (entry.is_msaa_render_target) {
        if (entry.needs_resolve) {
            context_->ResolveSubresource(
                entry.resolve_texture.Get(), 0,
                entry.texture.Get(), 0,
                DXGI_FORMAT_B8G8R8A8_UNORM);
            entry.needs_resolve = false;
        }
        context_->PSSetShaderResources(texture_unit, 1, entry.resolve_texture_srv.GetAddressOf());
    } else {
        context_->PSSetShaderResources(texture_unit, 1, entry.texture_srv.GetAddressOf());
    }
}

void GPUDriverD3D11::BindRenderBuffer(uint32_t render_buffer_id) {
    // Unbind SRVs to avoid D3D warnings
    ID3D11ShaderResourceView* nullSRV[1] = {nullptr};
    context_->PSSetShaderResources(0, 1, nullSRV);
    context_->PSSetShaderResources(1, 1, nullSRV);
    context_->PSSetShaderResources(2, 1, nullSRV);

    auto i = render_targets_.find(render_buffer_id);
    if (i == render_targets_.end()) return;

    // Flag the backing texture for MSAA resolve on next shader read
    auto tex = textures_.find(i->second.render_target_texture_id);
    if (tex != textures_.end() && tex->second.is_msaa_render_target) {
        tex->second.needs_resolve = true;
    }

    ID3D11RenderTargetView* target = i->second.render_target_view.Get();
    context_->OMSetRenderTargets(1, &target, nullptr);
}

void GPUDriverD3D11::ClearRenderBuffer(uint32_t render_buffer_id) {
    float color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    auto i = render_targets_.find(render_buffer_id);
    if (i == render_targets_.end()) return;
    context_->ClearRenderTargetView(i->second.render_target_view.Get(), color);
}

void GPUDriverD3D11::DrawGeometry(uint32_t geometry_id, uint32_t indices_count,
                                  uint32_t indices_offset, const GPUState& state) {
    BindRenderBuffer(state.render_buffer_id);
    SetViewport(state.viewport_width, state.viewport_height);

    if (state.texture_1_id) BindTexture(0, state.texture_1_id);
    if (state.texture_2_id) BindTexture(1, state.texture_2_id);

    UpdateConstantBuffer(state);
    BindGeometry(geometry_id);

    // Sampler
    if (!sampler_state_) {
        D3D11_SAMPLER_DESC sd;
        ZeroMemory(&sd, sizeof(sd));
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        device_->CreateSamplerState(&sd, sampler_state_.GetAddressOf());
    }
    context_->PSSetSamplers(0, 1, sampler_state_.GetAddressOf());

    BindShader(state.shader_type);

    // Blend
    if (state.enable_blend) {
        float blendFactor[4] = {1, 1, 1, 1};
        context_->OMSetBlendState(blend_state_.Get(), blendFactor, 0xffffffff);
    } else {
        float blendFactor[4] = {1, 1, 1, 1};
        context_->OMSetBlendState(disabled_blend_state_.Get(), blendFactor, 0xffffffff);
    }

    // Scissor
    if (state.enable_scissor) {
        context_->RSSetState(scissored_rasterizer_state_.Get());
        D3D11_RECT rect = {(LONG)state.scissor_rect.left, (LONG)state.scissor_rect.top,
                           (LONG)state.scissor_rect.right, (LONG)state.scissor_rect.bottom};
        context_->RSSetScissorRects(1, &rect);
    } else {
        context_->RSSetState(rasterizer_state_.Get());
    }

    context_->VSSetConstantBuffers(0, 1, constant_buffer_.GetAddressOf());
    context_->PSSetConstantBuffers(0, 1, constant_buffer_.GetAddressOf());
    context_->DrawIndexed(indices_count, indices_offset, 0);
}

void GPUDriverD3D11::SetViewport(uint32_t width, uint32_t height) {
    D3D11_VIEWPORT vp = {};
    vp.Width = (float)width;
    vp.Height = (float)height;
    vp.MaxDepth = 1.0f;
    context_->RSSetViewports(1, &vp);
}

void GPUDriverD3D11::UpdateConstantBuffer(const GPUState& state) {
    if (!constant_buffer_) {
        D3D11_BUFFER_DESC desc = {};
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.ByteWidth = sizeof(Uniforms);
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        device_->CreateBuffer(&desc, nullptr, constant_buffer_.GetAddressOf());
    }

    // Build orthographic projection and multiply with transform
    ultralight::Matrix transform_mat;
    transform_mat.Set(state.transform);

    ultralight::Matrix proj;
    proj.SetOrthographicProjection(
        (float)state.viewport_width, (float)state.viewport_height, false);
    proj.Transform(transform_mat);

    Uniforms uniforms;
    uniforms.State = {0.0f, (float)state.viewport_width, (float)state.viewport_height, 1.0f};
    uniforms.Transform = DirectX::XMMATRIX(proj.GetMatrix4x4().data);

    uniforms.Integer4[0] = {state.uniform_integer[0], state.uniform_integer[1],
                            state.uniform_integer[2], state.uniform_integer[3]};
    uniforms.Integer4[1] = {state.uniform_integer[4], state.uniform_integer[5],
                            state.uniform_integer[6], state.uniform_integer[7]};
    uniforms.Scalar4[0] = {state.uniform_scalar[0], state.uniform_scalar[1],
                           state.uniform_scalar[2], state.uniform_scalar[3]};
    uniforms.Scalar4[1] = {state.uniform_scalar[4], state.uniform_scalar[5],
                           state.uniform_scalar[6], state.uniform_scalar[7]};
    for (size_t i = 0; i < 8; ++i)
        uniforms.Vector[i] = {state.uniform_vector[i].x, state.uniform_vector[i].y,
                              state.uniform_vector[i].z, state.uniform_vector[i].w};
    uniforms.ClipData = {(int)state.clip_size, 0, 0, 0};
    for (size_t i = 0; i < state.clip_size; ++i)
        uniforms.Clip[i] = DirectX::XMMATRIX(state.clip[i].data);

    D3D11_MAPPED_SUBRESOURCE res;
    context_->Map(constant_buffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &res);
    memcpy(res.pData, &uniforms, sizeof(Uniforms));
    context_->Unmap(constant_buffer_.Get(), 0);
}

}
