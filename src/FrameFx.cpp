#include "FrameFx.h"
#include "AimDot.h"
#include "CamBlur.h"
#include "FovFx.h"

#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>

namespace FrameFx {

    namespace {
        using Microsoft::WRL::ComPtr;

        std::mutex  s_statusLock;
        std::string s_status = "nothing on yet";

        void SetStatus(std::string a_text, bool a_log = true) {
            std::scoped_lock lock(s_statusLock);
            if (a_log && s_status != a_text) SKSE::log::info("FrameFx: {}", a_text);
            s_status = std::move(a_text);
        }

        // One file: a full-screen triangle and the pass.
        constexpr const char* kHlsl = R"(
cbuffer FxCB : register(b0) {
    float  gAspect;      // width / height
    float  gVignette;    // 0..1 darker corners (the archer camera's)
    float  gEdgeBlur;    // edge blur: mip levels of blur at the screen's edge, 0 = off
    float  gEdgeSharp;   // how far out from the sharp middle the blur starts (0..1, 1 = the edge)
    float2 gEdgeCenter;  // uv the sharp middle sits on
    float2 gInvSize;     // one texel
    float2 gDotCenter;   // the aim dot: uv where the arrow will land
    float  gDotSize;     // its radius, pixels
    float  gDotOn;       // 0..1
};
Texture2D    gSrc : register(t0);
SamplerState gLin : register(s0);

struct VsOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VsOut VS(uint id : SV_VertexID) {
    VsOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv  = uv;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float4 PSFx(VsOut i) : SV_Target {
    float2 uv = i.uv;
    float3 c  = gSrc.SampleLevel(gLin, uv, 0).rgb;
    [branch] if (gEdgeBlur > 0.0) {
        // Blurry round the edge, sharp in the middle (a focus vignette). The picture's own smaller mips, more of them
        // the further out - nine taps a texel apart at that mip so the mip's blocks do not show. 1 = the screen's edge
        // (an ellipse), the corners further still.
        float2 d = (uv - gEdgeCenter) * 2.0;
        float  m = smoothstep(gEdgeSharp, 1.0, length(d));
        [branch] if (m > 0.001) {
            float  lod = gEdgeBlur * m;
            float2 px  = gInvSize * exp2(lod) * 0.75;
            float3 b   = gSrc.SampleLevel(gLin, uv, lod).rgb * 2.0;
            b += gSrc.SampleLevel(gLin, uv + float2( px.x,  0.0), lod).rgb;
            b += gSrc.SampleLevel(gLin, uv + float2(-px.x,  0.0), lod).rgb;
            b += gSrc.SampleLevel(gLin, uv + float2( 0.0,  px.y), lod).rgb;
            b += gSrc.SampleLevel(gLin, uv + float2( 0.0, -px.y), lod).rgb;
            b += gSrc.SampleLevel(gLin, uv + float2( px.x,  px.y), lod).rgb;
            b += gSrc.SampleLevel(gLin, uv + float2(-px.x,  px.y), lod).rgb;
            b += gSrc.SampleLevel(gLin, uv + float2( px.x, -px.y), lod).rgb;
            b += gSrc.SampleLevel(gLin, uv + float2(-px.x, -px.y), lod).rgb;
            c = lerp(c, b / 10.0, saturate(m * 3.0));
        }
    }
    if (gVignette > 0.0) {
        float2 d = (i.uv - 0.5) * float2(gAspect, 1.0) * (2.0 / sqrt(gAspect * gAspect + 1.0));  // 1 in the corners
        c *= 1.0 - gVignette * smoothstep(0.35, 1.0, dot(d, d));
    }
    [branch] if (gDotOn > 0.0) {
        // a tiny white dot with a dark rim where the arrow will land
        float dd   = length((i.uv - gDotCenter) / gInvSize);  // pixels
        float core = 1.0 - smoothstep(gDotSize - 0.7, gDotSize + 0.7, dd);
        float rim  = 1.0 - smoothstep(gDotSize + 1.2, gDotSize + 2.4, dd);
        c = lerp(c, float3(0.0, 0.0, 0.0), rim * 0.75 * gDotOn);
        c = lerp(c, float3(1.0, 1.0, 1.0), core * gDotOn);
    }
    return float4(c, 1.0);
}
)";

        struct alignas(16) FxCB {
            float aspect;
            float vignette;
            float edgeBlur;
            float edgeSharp;
            float edgeCenter[2];
            float invSize[2];
            float dotCenter[2];
            float dotSize;
            float dotOn;
        };
        static_assert(sizeof(FxCB) == 48);

        struct Gpu {
            ID3D11Device*                    dev = nullptr;
            ID3D11DeviceContext*             ctx = nullptr;
            ComPtr<ID3D11VertexShader>       vs;
            ComPtr<ID3D11PixelShader>        ps;
            ComPtr<ID3D11Buffer>             cb;
            ComPtr<ID3D11SamplerState>       smp;
            ComPtr<ID3D11RasterizerState>    rs;
            ComPtr<ID3D11BlendState>         blend;
            ComPtr<ID3D11DepthStencilState>  dss;
            ComPtr<ID3D11Texture2D>          copy;   // the picture, copied (the pass reads it)
            ComPtr<ID3D11ShaderResourceView> fxSrv;  // the render target's own format
            UINT                             w = 0, h = 0;
            DXGI_FORMAT                      texFmt = DXGI_FORMAT_UNKNOWN, viewFmt = DXGI_FORMAT_UNKNOWN;
            bool                             mips = false;  // the copy has a mip chain (the edge blur reads it)
        };
        Gpu g;
        int s_init = 0;  // 0 not tried, 1 ready, -1 failed

        using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT,
            UINT, ID3DBlob**, ID3DBlob**);

        ComPtr<ID3DBlob> Compile(D3DCompileFn a_fn, const char* a_entry, const char* a_target) {
            ComPtr<ID3DBlob> code, errors;
            const HRESULT    hr = a_fn(kHlsl, std::strlen(kHlsl), "TFCamFrameFx", nullptr, nullptr, a_entry, a_target,
                   D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
            if (FAILED(hr)) {
                SKSE::log::error("FrameFx: shader {} did not compile ({:08X}): {}", a_entry, static_cast<unsigned>(hr),
                    errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : "");
                return nullptr;
            }
            return code;
        }

        bool EnsureInit() {
            if (s_init != 0) return s_init > 0;
            s_init     = -1;
            auto* r    = RE::BSGraphics::Renderer::GetSingleton();
            if (!r) return false;
            auto& data = r->GetRuntimeData();
            g.dev      = reinterpret_cast<ID3D11Device*>(data.forwarder);
            g.ctx      = reinterpret_cast<ID3D11DeviceContext*>(data.context);
            if (!g.dev || !g.ctx) {
                SetStatus("no D3D11 device yet");
                s_init = 0;  // try again next frame
                return false;
            }
            // d3dcompiler_47 ships with Windows 10; loaded here so TFCam itself never depends on it
            HMODULE    lib = ::LoadLibraryW(L"d3dcompiler_47.dll");
            const auto fn  = lib ? reinterpret_cast<D3DCompileFn>(::GetProcAddress(lib, "D3DCompile")) : nullptr;
            if (!fn) {
                SetStatus("off - d3dcompiler_47.dll was not found");
                return false;
            }
            auto vs = Compile(fn, "VS", "vs_5_0");
            auto ps = Compile(fn, "PSFx", "ps_5_0");
            if (!vs || !ps) {
                SetStatus("off - the shaders did not compile (see TFCam.log)");
                return false;
            }
            bool ok = SUCCEEDED(g.dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g.vs));
            ok = ok && SUCCEEDED(g.dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g.ps));
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth      = sizeof(FxCB);
            bd.Usage          = D3D11_USAGE_DYNAMIC;
            bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            ok = ok && SUCCEEDED(g.dev->CreateBuffer(&bd, nullptr, &g.cb));
            D3D11_SAMPLER_DESC sd{};
            sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.MaxLOD   = D3D11_FLOAT32_MAX;
            ok = ok && SUCCEEDED(g.dev->CreateSamplerState(&sd, &g.smp));
            D3D11_RASTERIZER_DESC rd{};
            rd.FillMode        = D3D11_FILL_SOLID;
            rd.CullMode        = D3D11_CULL_NONE;
            rd.DepthClipEnable = TRUE;
            ok = ok && SUCCEEDED(g.dev->CreateRasterizerState(&rd, &g.rs));
            D3D11_BLEND_DESC bl{};
            bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            ok = ok && SUCCEEDED(g.dev->CreateBlendState(&bl, &g.blend));
            D3D11_DEPTH_STENCIL_DESC dd{};
            dd.DepthEnable = FALSE;
            dd.DepthFunc   = D3D11_COMPARISON_ALWAYS;
            ok = ok && SUCCEEDED(g.dev->CreateDepthStencilState(&dd, &g.dss));
            if (!ok) {
                SetStatus("off - could not create its GPU objects");
                return false;
            }
            s_init = 1;
            SetStatus("ready");
            return true;
        }

        // sRGB render targets: the copy is typeless so a view in the render target's own format can read it.
        DXGI_FORMAT TypelessOf(DXGI_FORMAT a_f) {
            switch (a_f) {
            case DXGI_FORMAT_R8G8B8A8_UNORM:
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            case DXGI_FORMAT_R8G8B8A8_TYPELESS:
                return DXGI_FORMAT_R8G8B8A8_TYPELESS;
            case DXGI_FORMAT_B8G8R8A8_UNORM:
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            case DXGI_FORMAT_B8G8R8A8_TYPELESS:
                return DXGI_FORMAT_B8G8R8A8_TYPELESS;
            case DXGI_FORMAT_R10G10B10A2_UNORM:
            case DXGI_FORMAT_R10G10B10A2_TYPELESS:
                return DXGI_FORMAT_R10G10B10A2_TYPELESS;
            default:
                return a_f;
            }
        }

        bool EnsureCopy(const D3D11_TEXTURE2D_DESC& a_td, DXGI_FORMAT a_view) {
            if (g.copy && g.w == a_td.Width && g.h == a_td.Height && g.texFmt == a_td.Format && g.viewFmt == a_view) return true;
            g.copy.Reset();
            g.fxSrv.Reset();
            D3D11_TEXTURE2D_DESC d{};
            d.Width            = a_td.Width;
            d.Height           = a_td.Height;
            d.MipLevels        = 0;  // the whole chain - the edge blur samples the smaller mips
            d.ArraySize        = 1;
            d.Format           = TypelessOf(a_td.Format);
            d.SampleDesc.Count = 1;
            d.Usage            = D3D11_USAGE_DEFAULT;
            d.BindFlags        = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;  // GenerateMips wants both
            d.MiscFlags        = D3D11_RESOURCE_MISC_GENERATE_MIPS;
            g.mips             = true;
            if (FAILED(g.dev->CreateTexture2D(&d, nullptr, &g.copy))) {
                g.mips      = false;  // a format without mip generation: one level, no edge blur
                d.MipLevels = 1;
                d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                d.MiscFlags = 0;
                if (FAILED(g.dev->CreateTexture2D(&d, nullptr, &g.copy))) {
                    SetStatus(std::format("off - no copy of the picture ({}x{}, format {})", a_td.Width, a_td.Height, static_cast<int>(a_td.Format)));
                    return false;
                }
            }
            D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
            sv.Texture2D.MipLevels = g.mips ? static_cast<UINT>(-1) : 1;
            sv.Format              = a_view;
            if (FAILED(g.dev->CreateShaderResourceView(g.copy.Get(), &sv, &g.fxSrv))) {
                g.copy.Reset();
                SetStatus(std::format("off - the picture's format {} / {} cannot be read", static_cast<int>(a_td.Format), static_cast<int>(a_view)));
                return false;
            }
            g.w       = a_td.Width;
            g.h       = a_td.Height;
            g.texFmt  = a_td.Format;
            g.viewFmt = a_view;
            SKSE::log::info("FrameFx: the picture is {}x{} (texture format {}, view {}{})", g.w, g.h, static_cast<int>(a_td.Format),
                static_cast<int>(a_view), g.mips ? ", with mips" : ", no mips - no edge blur");
            return true;
        }

        // Everything our draw touches, put back afterwards (the pattern of Dear ImGui's DX11 backend).
        struct StateBackup {
            UINT                      scissorCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
            UINT                      viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
            D3D11_RECT                scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
            D3D11_VIEWPORT            viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
            ID3D11RasterizerState*    rs = nullptr;
            ID3D11BlendState*         blend = nullptr;
            FLOAT                     blendFactor[4]{};
            UINT                      sampleMask = 0;
            UINT                      stencilRef = 0;
            ID3D11DepthStencilState*  dss = nullptr;
            ID3D11ShaderResourceView* psSrv = nullptr;
            ID3D11SamplerState*       psSampler = nullptr;
            ID3D11PixelShader*        ps = nullptr;
            ID3D11VertexShader*       vs = nullptr;
            ID3D11GeometryShader*     gs = nullptr;
            ID3D11HullShader*         hs = nullptr;
            ID3D11DomainShader*       ds = nullptr;
            ID3D11ClassInstance*      psInst[256]{};
            ID3D11ClassInstance*      vsInst[256]{};
            ID3D11ClassInstance*      gsInst[256]{};
            UINT                      psInstCount = 256, vsInstCount = 256, gsInstCount = 256;
            ID3D11Buffer*             vsCb = nullptr;
            ID3D11Buffer*             psCb = nullptr;
            D3D11_PRIMITIVE_TOPOLOGY  topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
            ID3D11Buffer*             ib = nullptr;
            DXGI_FORMAT               ibFormat = DXGI_FORMAT_UNKNOWN;
            UINT                      ibOffset = 0;
            ID3D11Buffer*             vb = nullptr;
            UINT                      vbStride = 0, vbOffset = 0;
            ID3D11InputLayout*        layout = nullptr;
            ID3D11RenderTargetView*   rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
            ID3D11DepthStencilView*   dsv = nullptr;

            void Capture(ID3D11DeviceContext* c) {
                c->RSGetScissorRects(&scissorCount, scissors);
                c->RSGetViewports(&viewportCount, viewports);
                c->RSGetState(&rs);
                c->OMGetBlendState(&blend, blendFactor, &sampleMask);
                c->OMGetDepthStencilState(&dss, &stencilRef);
                c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
                c->PSGetShaderResources(0, 1, &psSrv);
                c->PSGetSamplers(0, 1, &psSampler);
                c->PSGetShader(&ps, psInst, &psInstCount);
                c->VSGetShader(&vs, vsInst, &vsInstCount);
                c->GSGetShader(&gs, gsInst, &gsInstCount);
                c->HSGetShader(&hs, nullptr, nullptr);
                c->DSGetShader(&ds, nullptr, nullptr);
                c->VSGetConstantBuffers(0, 1, &vsCb);
                c->PSGetConstantBuffers(0, 1, &psCb);
                c->IAGetPrimitiveTopology(&topology);
                c->IAGetIndexBuffer(&ib, &ibFormat, &ibOffset);
                c->IAGetVertexBuffers(0, 1, &vb, &vbStride, &vbOffset);
                c->IAGetInputLayout(&layout);
            }

            template <class T>
            static void Drop(T*& a_p) {
                if (a_p) a_p->Release();
                a_p = nullptr;
            }

            void Restore(ID3D11DeviceContext* c) {
                c->RSSetScissorRects(scissorCount, scissors);
                c->RSSetViewports(viewportCount, viewports);
                c->RSSetState(rs);
                c->OMSetBlendState(blend, blendFactor, sampleMask);
                c->OMSetDepthStencilState(dss, stencilRef);
                c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
                c->PSSetShaderResources(0, 1, &psSrv);
                c->PSSetSamplers(0, 1, &psSampler);
                c->PSSetShader(ps, psInst, psInstCount);
                c->VSSetShader(vs, vsInst, vsInstCount);
                c->GSSetShader(gs, gsInst, gsInstCount);
                c->HSSetShader(hs, nullptr, 0);
                c->DSSetShader(ds, nullptr, 0);
                c->VSSetConstantBuffers(0, 1, &vsCb);
                c->PSSetConstantBuffers(0, 1, &psCb);
                c->IASetPrimitiveTopology(topology);
                c->IASetIndexBuffer(ib, ibFormat, ibOffset);
                c->IASetVertexBuffers(0, 1, &vb, &vbStride, &vbOffset);
                c->IASetInputLayout(layout);
                Drop(rs), Drop(blend), Drop(dss), Drop(psSrv), Drop(psSampler), Drop(ps), Drop(vs), Drop(gs), Drop(hs), Drop(ds);
                for (UINT i = 0; i < psInstCount; ++i) Drop(psInst[i]);
                for (UINT i = 0; i < vsInstCount; ++i) Drop(vsInst[i]);
                for (UINT i = 0; i < gsInstCount; ++i) Drop(gsInst[i]);
                Drop(vsCb), Drop(psCb), Drop(ib), Drop(vb), Drop(layout), Drop(dsv);
                for (auto& r : rtvs) Drop(r);
            }
        };

        void Draw(ID3D11RenderTargetView* a_rtv, ID3D11ShaderResourceView* a_src, UINT a_w, UINT a_h) {
            auto* c = g.ctx;
            c->OMSetRenderTargets(1, &a_rtv, nullptr);
            const D3D11_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(a_w), static_cast<float>(a_h), 0.0f, 1.0f };
            c->RSSetViewports(1, &vp);
            c->RSSetState(g.rs.Get());
            c->OMSetBlendState(g.blend.Get(), nullptr, 0xFFFFFFFF);
            c->OMSetDepthStencilState(g.dss.Get(), 0);
            c->IASetInputLayout(nullptr);
            c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ID3D11Buffer* none = nullptr;
            UINT          zero = 0;
            c->IASetVertexBuffers(0, 1, &none, &zero, &zero);
            c->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
            c->VSSetShader(g.vs.Get(), nullptr, 0);
            c->PSSetShader(g.ps.Get(), nullptr, 0);
            c->GSSetShader(nullptr, nullptr, 0);
            c->HSSetShader(nullptr, nullptr, 0);
            c->DSSetShader(nullptr, nullptr, 0);
            c->PSSetShaderResources(0, 1, &a_src);
            ID3D11SamplerState* smp = g.smp.Get();
            c->PSSetSamplers(0, 1, &smp);
            ID3D11Buffer* cb = g.cb.Get();
            c->PSSetConstantBuffers(0, 1, &cb);
            c->Draw(3, 0);
            ID3D11ShaderResourceView* noSrv = nullptr;
            c->PSSetShaderResources(0, 1, &noSrv);
        }

        bool FreeCamera() {
            auto* pc = RE::PlayerCamera::GetSingleton();
            return pc && pc->IsInFreeCameraMode();
        }

        double Now() {
            static const double freq = [] {
                LARGE_INTEGER f;
                ::QueryPerformanceFrequency(&f);
                return static_cast<double>(f.QuadPart);
            }();
            LARGE_INTEGER t;
            ::QueryPerformanceCounter(&t);
            return static_cast<double>(t.QuadPart) / freq;
        }

        // ---- the edge blur (Background blur, Edges style), eased so it never pops -----------------------------------
        struct EdgeNow {
            float lod = 0.0f;  // mip levels of blur at the screen's edge
            float cx = 0.5f, cy = 0.5f;
        };

        // Every frame (main thread): what CamBlur wants, eased in and out over ~0.25 s; the sharp middle follows the
        // subject on screen (eased ~0.15 s), or sits in the middle.
        EdgeNow EdgeTick() {
            static EdgeNow s_now;
            static double  s_last = 0.0;
            const double   now    = Now();
            const float    dt     = s_last > 0.0 ? static_cast<float>(std::clamp(now - s_last, 0.0, 0.1)) : 0.0f;
            s_last                = now;
            const auto want       = CamBlur::EdgeWant();
            float      lod = 0.0f, cx = s_now.cx, cy = s_now.cy;
            // (not gated on g.mips here: the copy is only made once the pass runs, and this is what makes it run)
            if (want.on && CamBlur::Get().style == CamBlur::kStyleEdges) {
                lod = 1.5f + 4.5f * std::clamp(want.strength, 0.0f, 1.0f);  // 0 -> 1.5 mips (soft), 1 -> 6 mips (a wash)
                cx = 0.5f, cy = 0.5f;
                if (want.haveSubject) {
                    if (auto* nc = RE::Main::WorldRootCamera()) {
                        float x = 0.0f, y = 0.0f, z = 0.0f;
                        if (nc->WorldPtToScreenPt3(want.subject, x, y, z, 1e-5f) && z > 0.0f && std::isfinite(x) && std::isfinite(y)) {
                            cx = std::clamp(x, 0.25f, 0.75f);
                            cy = std::clamp(1.0f - y, 0.25f, 0.75f);
                        }
                    }
                }
            }
            const float kl = dt > 0.0f ? 1.0f - std::exp(-dt / 0.25f) : 1.0f;
            const float kc = dt > 0.0f ? 1.0f - std::exp(-dt / 0.15f) : 1.0f;
            s_now.lod += (lod - s_now.lod) * kl;
            if (lod <= 0.0f && s_now.lod < 0.02f) s_now.lod = 0.0f;
            if (lod > 0.0f) {
                s_now.cx += (cx - s_now.cx) * kc;
                s_now.cy += (cy - s_now.cy) * kc;
            }
            return s_now;
        }

        // ---- the aim dot - where the arrow will land ---------------------------------------------------------------
        struct DotNow {
            float on = 0.0f;  // 0..1
            float cx = 0.5f, cy = 0.5f;
        };

        DotNow DotTick() {
            DotNow       d;
            RE::NiPoint3 aim{};
            float        in = 0.0f;
            if (!FovFx::Get().aimDot || FreeCamera()) return d;  // never in the free camera
            // The projectiles go through the middle of the screen (TFCam's own aim, or Hit Where You Aim): the dot sits
            // there whenever something ranged is out. With Hit Where You Aim only while Action FOV is on, so a player who
            // turned nothing on in TFCam sees no change.
            if (AimDot::Active() || (FovFx::HitWhereYouAim() && FovFx::Get().enabled)) {
                if (AimDot::DotWanted()) d.on = 1.0f;
                return d;
            }
            if (!FovFx::ArcherAim(aim, in) || in < 0.6f) return d;
            d.on = std::clamp((in - 0.6f) / 0.3f, 0.0f, 1.0f);
            // the arrow's own line: the aim point on screen (the view settles on it in a moment)
            if (auto* nc = RE::Main::WorldRootCamera()) {
                float x = 0.0f, y = 0.0f, z = 0.0f;
                if (nc->WorldPtToScreenPt3(aim, x, y, z, 1e-5f) && z > 0.0f && std::isfinite(x) && std::isfinite(y)) {
                    d.cx = std::clamp(x, 0.0f, 1.0f);
                    d.cy = std::clamp(1.0f - y, 0.0f, 1.0f);
                } else {
                    d.on = 0.0f;
                }
            }
            return d;
        }

        // The pass's constants for this frame; false = nothing to do.
        bool Constants(const EdgeNow& a_e, const DotNow& a_d, float a_vignette, UINT a_w, UINT a_h, FxCB& a_cb) {
            a_cb            = {};
            a_cb.aspect     = a_h > 0 ? static_cast<float>(a_w) / static_cast<float>(a_h) : 1.7778f;
            a_cb.invSize[0] = a_w > 0 ? 1.0f / static_cast<float>(a_w) : 0.0f;
            a_cb.invSize[1] = a_h > 0 ? 1.0f / static_cast<float>(a_h) : 0.0f;
            bool any        = false;
            if (a_e.lod > 0.01f && g.mips) {
                a_cb.edgeBlur      = a_e.lod;
                a_cb.edgeSharp     = CamBlur::Get().sharp;
                a_cb.edgeCenter[0] = a_e.cx;
                a_cb.edgeCenter[1] = a_e.cy;
                any                = true;
            }
            if (a_d.on > 0.0f) {
                a_cb.dotCenter[0] = a_d.cx;
                a_cb.dotCenter[1] = a_d.cy;
                a_cb.dotSize      = FovFx::Get().aimDotSize * (std::max)(static_cast<float>(a_h) / 1080.0f, 0.5f);
                a_cb.dotOn        = a_d.on;
                any               = true;
            }
            if (a_vignette > 0.001f) {  // darker corners while the string is drawn
                a_cb.vignette = a_vignette;
                any           = true;
            }
            return any;
        }

        void Frame() {
            const EdgeNow e   = EdgeTick();  // every frame, so it eases
            const DotNow  dot = DotTick();
            const float   vig = FovFx::ArcherVignette();
            if (e.lod <= 0.01f && dot.on <= 0.0f && vig <= 0.001f) {
                if (s_init > 0) SetStatus("idle (nothing to show right now)", false);
                return;
            }
            if (!EnsureInit()) return;
            // The frame buffer, not whatever is bound: the menu draw binds it itself after this hook, so usually
            // nothing is bound here, and during a load another target can be (seen: 1920x1080 R8).
            auto& fb = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kFRAMEBUFFER];
            ComPtr<ID3D11RenderTargetView> rtv = reinterpret_cast<ID3D11RenderTargetView*>(fb.RTV);
            if (!rtv) {
                SetStatus("waiting - the game's frame buffer is not made yet");
                return;
            }
            ComPtr<ID3D11Resource> res;
            rtv->GetResource(&res);
            ComPtr<ID3D11Texture2D> tex;
            if (!res || FAILED(res.As(&tex))) return;
            D3D11_TEXTURE2D_DESC          td{};
            D3D11_RENDER_TARGET_VIEW_DESC rd{};
            tex->GetDesc(&td);
            rtv->GetDesc(&rd);
            if (td.SampleDesc.Count != 1 || rd.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D) {
                SetStatus(std::format("off - the picture is multisampled ({}x) or not a plain texture", td.SampleDesc.Count));
                return;
            }
            if (!EnsureCopy(td, rd.Format)) return;
            FxCB cb{};
            if (!Constants(e, dot, vig, td.Width, td.Height, cb)) return;
            StateBackup backup;
            backup.Capture(g.ctx);
            // the top mip only (the copy may have a mip chain, the frame buffer has not)
            g.ctx->CopySubresourceRegion(g.copy.Get(), 0, 0, 0, 0, tex.Get(), 0, nullptr);
            if (cb.edgeBlur > 0.0f) g.ctx->GenerateMips(g.fxSrv.Get());  // the edge blur's smaller pictures
            D3D11_MAPPED_SUBRESOURCE map{};
            if (SUCCEEDED(g.ctx->Map(g.cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
                std::memcpy(map.pData, &cb, sizeof(cb));
                g.ctx->Unmap(g.cb.Get(), 0);
                Draw(rtv.Get(), g.fxSrv.Get(), td.Width, td.Height);
            }
            backup.Restore(g.ctx);
            SetStatus("on", false);  // changes every time the string is drawn / let go
        }

        // Main::Draw's call that draws the menus (SKSE Menu Framework's spot): the world picture is finished here.
        struct MenuDrawHook {
            static std::int64_t thunk(std::int64_t a_menus) {
                Frame();
                return func(a_menus);
            }
            static inline REL::Relocation<decltype(thunk)> func;
        };
    }

    void Install() {
        if (REL::Module::IsVR()) {
            SKSE::log::info("FrameFx: not on VR");
            return;
        }
        REL::Relocation<std::uintptr_t> draw{ RELOCATION_ID(35556, 36555) };
        const std::uintptr_t site = draw.address() + REL::Relocate(0x3ab, 0x371);
        if (*reinterpret_cast<const std::uint8_t*>(site) != 0xE8) {
            SKSE::log::warn("FrameFx: Main::Draw's menu call is not a call here (byte {:02X}) - no edge blur / aim dot",
                *reinterpret_cast<const std::uint8_t*>(site));
            SetStatus("off - the game's draw call was not found");
            return;
        }
        MenuDrawHook::func = SKSE::GetTrampoline().write_call<5>(site, MenuDrawHook::thunk);
        SKSE::log::info("FrameFx: Main::Draw's menu draw hooked (edge blur, archer vignette, aim dot - before the UI)");
    }

    std::string Status() {
        std::scoped_lock lock(s_statusLock);
        return s_status;
    }
}
