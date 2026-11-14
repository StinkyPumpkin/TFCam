#include "FrameFx.h"
#include "AimDot.h"
#include "CamBlur.h"
#include "FixedCam.h"
#include "FovFx.h"
#include "VideoCapture.h"

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

        Settings s_settings;

        std::mutex  s_statusLock;
        std::string s_status = "nothing on yet";

        void SetStatus(std::string a_text, bool a_log = true) {
            std::scoped_lock lock(s_statusLock);
            if (a_log && s_status != a_text) SKSE::log::info("FrameFx: {}", a_text);
            s_status = std::move(a_text);
        }

        // One file: a full-screen triangle, the effects, a copy, and the two halves of the video's NV12 picture.
        constexpr const char* kHlsl = R"(
cbuffer FxCB : register(b0) {
    float2 gBlur;        // uv: the whole smear of the camera's turn
    float  gZoom;        // radial smear: part of the distance to the middle
    float  gDistort;     // barrel + / pincushion -
    float  gFringe;
    float  gVignette;
    float  gStreaks;
    float  gStreakFrom;
    float  gAspect;      // width / height
    float  gBars;        // uv height of each cinema bar, 0 = none
    float  gTaps;        // blur taps, 1 = no blur
    float  gEdgeBlur;    // 10-31 edge blur: mip levels of blur at the screen's edge, 0 = off
    float  gEdgeSharp;   // how far out from the sharp middle the blur starts (0..1, 1 = the edge)
    float  gPad;
    float2 gEdgeCenter;  // uv the sharp middle sits on
    float2 gInvSize;     // one texel
    float2 gDotCenter;   // 10-31 the archer dot: uv where the arrow will land
    float  gDotSize;     // its radius, pixels
    float  gDotOn;       // 0..1
    float2 gPad2;
    float  gDepthMix;    // 11-13: 0..1, the edge blur spares the subject and what is in front of it (depth)
    float  gSubjZ;       // the subject's view depth, game units
    float  gDepthNear;   // view depth where the blur starts behind the subject
    float  gDepthFar;    // view depth where it is full
    float  gNear;        // the world camera's near and far planes (the depth buffer: 0 at near, 1 at far)
    float  gFar;
    float2 gProbeUv;     // the subject on screen (the one-time depth check)
    float2 gDepthSize;   // the depth buffer's size, texels
    float2 gPad3;
};
Texture2D        gSrc   : register(t0);
Texture2D<float> gDepth : register(t1);  // 11-13: the game's main depth buffer, bound only while gDepthMix > 0
SamplerState     gLin   : register(s0);

struct VsOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VsOut VS(uint id : SV_VertexID) {
    VsOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv  = uv;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float2 Lens(float2 uv, float k) {
    float2 d = uv - 0.5;
    d.x *= gAspect;
    d *= 1.0 + k * dot(d, d);
    d.x /= gAspect;
    return d + 0.5;
}

float3 Tap(float2 uv) {
    float3 c = gSrc.SampleLevel(gLin, uv, 0).rgb;
    [branch] if (gFringe > 0.0) {
        float2 f = (uv - 0.5) * gFringe * 0.012;
        c.r = gSrc.SampleLevel(gLin, uv + f, 0).r;
        c.b = gSrc.SampleLevel(gLin, uv - f, 0).b;
    }
    return c;
}

float Bright(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// 11-13: the depth buffer is the standard D3D one (0 at the near plane, 1 at the far plane - CamBlur::ShaderDepth and
// ENB's ADOF read it the same way), so view depth = near * far / (far - d * (far - near)).
float DepthAt(float2 uv) {
    uint2 p = min(uint2(saturate(uv) * gDepthSize), uint2(max(gDepthSize - 1.0, 0.0)));
    return gDepth.Load(int3(p, 0));
}
float LinearZ(float d) { return gNear * gFar / max(gFar - d * (gFar - gNear), 1e-4); }

float4 PSFx(VsOut i) : SV_Target {
    if (gBars > 0.0 && (i.uv.y < gBars || i.uv.y > 1.0 - gBars)) return float4(0.0, 0.0, 0.0, 1.0);
    float2 uv  = Lens(i.uv, gDistort * 0.35);
    float2 toC = uv - 0.5;
    float  n   = max(gTaps, 1.0);
    float3 c   = 0.0;
    [loop] for (float k = 0.0; k < n; k += 1.0) {
        float t = n > 1.0 ? k / (n - 1.0) : 0.5;
        c += Tap(uv + gBlur * (t - 0.5) - toC * gZoom * t);
    }
    c /= n;
    if (gStreaks > 0.0) {
        float s = 0.0;
        [loop] for (int j = 1; j <= 12; ++j) {
            float o = j * 0.0045;
            float w = 1.0 - j / 13.0;
            s += (max(0.0, Bright(gSrc.SampleLevel(gLin, i.uv + float2(o, 0.0), 0).rgb) - gStreakFrom) +
                  max(0.0, Bright(gSrc.SampleLevel(gLin, i.uv - float2(o, 0.0), 0).rgb) - gStreakFrom)) * w;
        }
        c += s * gStreaks * 0.35 * float3(0.45, 0.65, 1.0);
    }
    [branch] if (gEdgeBlur > 0.0) {
        // 10-31: blurry round the edge, sharp in the middle (a focus vignette). The picture's own smaller mips, more of
        // them the further out - nine taps a texel apart at that mip so the mip's blocks do not show. 1 = the screen's
        // edge (an ellipse), the corners further still.
        float2 d = (uv - gEdgeCenter) * 2.0;
        float  m = smoothstep(gEdgeSharp, 1.0, length(d));
        // 11-13 (user: "can we make it not blur the player"): the eye-shaped ellipse stays, but only what is behind the
        // subject blurs - she, her hair and clothes and anything in front of her stay sharp, the background fades in.
        [branch] if (gDepthMix > 0.0 && m > 0.001) {
            m *= lerp(1.0, smoothstep(gDepthNear, gDepthFar, LinearZ(DepthAt(uv))), gDepthMix);
        }
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
        // 10-31: a tiny white dot with a dark rim where the arrow will land (the archer camera)
        float dd   = length((i.uv - gDotCenter) / gInvSize);  // pixels
        float core = 1.0 - smoothstep(gDotSize - 0.7, gDotSize + 0.7, dd);
        float rim  = 1.0 - smoothstep(gDotSize + 1.2, gDotSize + 2.4, dd);
        c = lerp(c, float3(0.0, 0.0, 0.0), rim * 0.75 * gDotOn);
        c = lerp(c, float3(1.0, 1.0, 1.0), core * gDotOn);
    }
    return float4(c, 1.0);
}

float4 PSCopy(VsOut i) : SV_Target { return float4(gSrc.SampleLevel(gLin, i.uv, 0).rgb, 1.0); }

// 11-13: the one-time depth check (a 1x1 target): the raw depth at the subject, it as game units, the subject's own depth
float4 PSProbe(VsOut i) : SV_Target {
    float raw = DepthAt(gProbeUv);
    return float4(raw, LinearZ(raw), gSubjZ, 1.0);
}

// BT.709, limited range (what players expect from an H.264 file)
float PSLuma(VsOut i) : SV_Target {
    float3 c = saturate(gSrc.SampleLevel(gLin, i.uv, 0).rgb);
    return (16.0 + dot(c, float3(46.559, 156.629, 15.812))) / 255.0;
}

float2 PSChroma(VsOut i) : SV_Target {
    float3 c = saturate(gSrc.SampleLevel(gLin, i.uv, 0).rgb);
    return float2(128.0 + dot(c, float3(-25.664, -86.336, 112.0)), 128.0 + dot(c, float3(112.0, -101.730, -10.270))) / 255.0;
}
)";

        struct alignas(16) FxCB {
            float blur[2];
            float zoom;
            float distort;
            float fringe;
            float vignette;
            float streaks;
            float streakFrom;
            float aspect;
            float bars;
            float taps;
            float edgeBlur;
            float edgeSharp;
            float pad;
            float edgeCenter[2];
            float invSize[2];
            float dotCenter[2];
            float dotSize;
            float dotOn;
            float pad2[2];
            float depthMix;  // 11-13: the edge blur keeps the subject sharp by depth
            float subjZ;
            float depthNear;
            float depthFar;
            float zNear;
            float zFar;
            float probeUv[2];
            float depthSize[2];
            float pad3[2];
        };
        static_assert(sizeof(FxCB) == 144);

        struct Gpu {
            ID3D11Device*                    dev = nullptr;
            ID3D11DeviceContext*             ctx = nullptr;
            ComPtr<ID3D11VertexShader>       vs;
            ComPtr<ID3D11PixelShader>        ps[5];  // fx, copy, luma, chroma, 11-13 probe (the depth check)
            ComPtr<ID3D11Buffer>             cb;
            ComPtr<ID3D11SamplerState>       smp;
            ComPtr<ID3D11RasterizerState>    rs;
            ComPtr<ID3D11BlendState>         blend;
            ComPtr<ID3D11DepthStencilState>  dss;
            // the picture, copied (effects read it; the video reads it after the effects)
            ComPtr<ID3D11Texture2D>          copy;
            ComPtr<ID3D11ShaderResourceView> fxSrv;     // the render target's own format
            ComPtr<ID3D11ShaderResourceView> videoSrv;  // never sRGB-decoded: the video wants the picture's own values
            UINT                             w = 0, h = 0;
            DXGI_FORMAT                      texFmt = DXGI_FORMAT_UNKNOWN, viewFmt = DXGI_FORMAT_UNKNOWN;
            bool                             mips = false;  // 10-31: the copy has a mip chain (the edge blur reads it)
            // 11-13: the game's main depth buffer, so the edge blur keeps the subject sharp. depthSrv is the game's own
            // view or ownDepthSrv (made here when the game has none); checked again whenever the game's pointers change.
            ID3D11ShaderResourceView*        depthSrv = nullptr;
            ComPtr<ID3D11ShaderResourceView> ownDepthSrv;
            const void*                      depthTexKey = nullptr;
            const void*                      depthSrvKey = nullptr;
            bool                             depthChecked = false, depthOk = false;
            UINT                             depthW = 0, depthH = 0;
            // 11-13: the one-time depth check - a 1x1 target the probe shader writes, and its CPU-readable copy
            ComPtr<ID3D11Texture2D>          probe, probeRead;
            ComPtr<ID3D11RenderTargetView>   probeRtv;
            int                              probeState = 0;  // 0 not yet, 1 waiting for the GPU, 2 done (or gave up)
        };
        Gpu  g;
        int  s_init = 0;  // 0 not tried, 1 ready, -1 failed
        bool s_inHook = false;

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
            const char* entries[5] = { "PSFx", "PSCopy", "PSLuma", "PSChroma", "PSProbe" };
            ComPtr<ID3DBlob> ps[5];
            for (int i = 0; i < 5; ++i) ps[i] = Compile(fn, entries[i], "ps_5_0");
            if (!vs || !ps[0] || !ps[1] || !ps[2] || !ps[3] || !ps[4]) {
                SetStatus("off - the shaders did not compile (see TFCam.log)");
                return false;
            }
            bool ok = SUCCEEDED(g.dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g.vs));
            for (int i = 0; i < 5 && ok; ++i) ok = SUCCEEDED(g.dev->CreatePixelShader(ps[i]->GetBufferPointer(), ps[i]->GetBufferSize(), nullptr, &g.ps[i]));
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

        // sRGB render targets: the copy is typeless so the video can read the picture's own (gamma) values.
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

        DXGI_FORMAT PlainOf(DXGI_FORMAT a_view) {
            switch (a_view) {
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
            default: return a_view;
            }
        }

        bool EnsureCopy(const D3D11_TEXTURE2D_DESC& a_td, DXGI_FORMAT a_view) {
            if (g.copy && g.w == a_td.Width && g.h == a_td.Height && g.texFmt == a_td.Format && g.viewFmt == a_view) return true;
            g.copy.Reset();
            g.fxSrv.Reset();
            g.videoSrv.Reset();
            D3D11_TEXTURE2D_DESC d{};
            d.Width            = a_td.Width;
            d.Height           = a_td.Height;
            d.MipLevels        = 0;  // 10-31: the whole chain - the edge blur samples the smaller mips
            d.ArraySize        = 1;
            d.Format           = TypelessOf(a_td.Format);
            d.SampleDesc.Count = 1;
            d.Usage            = D3D11_USAGE_DEFAULT;
            d.BindFlags        = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;  // GenerateMips wants both
            d.MiscFlags        = D3D11_RESOURCE_MISC_GENERATE_MIPS;
            g.mips             = true;
            if (FAILED(g.dev->CreateTexture2D(&d, nullptr, &g.copy))) {
                g.mips       = false;  // a format without mip generation: the copy as before, no edge blur
                d.MipLevels  = 1;
                d.BindFlags  = D3D11_BIND_SHADER_RESOURCE;
                d.MiscFlags  = 0;
                if (FAILED(g.dev->CreateTexture2D(&d, nullptr, &g.copy))) {
                    SetStatus(std::format("off - no copy of the picture ({}x{}, format {})", a_td.Width, a_td.Height, static_cast<int>(a_td.Format)));
                    return false;
                }
            }
            D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
            sv.Texture2D.MipLevels = g.mips ? static_cast<UINT>(-1) : 1;
            sv.Format              = a_view;
            const bool ok1         = SUCCEEDED(g.dev->CreateShaderResourceView(g.copy.Get(), &sv, &g.fxSrv));
            sv.Format              = PlainOf(a_view);
            sv.Texture2D.MipLevels = 1;  // the video reads the full picture only
            const bool ok2         = SUCCEEDED(g.dev->CreateShaderResourceView(g.copy.Get(), &sv, &g.videoSrv));
            if (!ok1 || !ok2) {
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

        // Everything our draws touch, put back afterwards (the pattern of Dear ImGui's DX11 backend).
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
            ID3D11ShaderResourceView* psSrv[2]{};  // 11-13: t0 and t1 (the edge blur's depth)
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
                c->PSGetShaderResources(0, 2, psSrv);
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
                c->PSSetShaderResources(0, 2, psSrv);
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
                Drop(rs), Drop(blend), Drop(dss), Drop(psSrv[0]), Drop(psSrv[1]), Drop(psSampler), Drop(ps), Drop(vs), Drop(gs), Drop(hs), Drop(ds);
                for (UINT i = 0; i < psInstCount; ++i) Drop(psInst[i]);
                for (UINT i = 0; i < vsInstCount; ++i) Drop(vsInst[i]);
                for (UINT i = 0; i < gsInstCount; ++i) Drop(gsInst[i]);
                Drop(vsCb), Drop(psCb), Drop(ib), Drop(vb), Drop(layout), Drop(dsv);
                for (auto& r : rtvs) Drop(r);
            }
        };

        // a_depth (11-13): the game's depth buffer at t1 for this draw only. The depth-stencil view is unbound first
        // (OMSetRenderTargets below), so the depth can be read; t1 is emptied again after the draw.
        void Draw(ID3D11RenderTargetView* a_rtv, ID3D11ShaderResourceView* a_src, UINT a_w, UINT a_h, ID3D11PixelShader* a_ps,
            ID3D11ShaderResourceView* a_depth = nullptr) {
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
            c->PSSetShader(a_ps, nullptr, 0);
            c->GSSetShader(nullptr, nullptr, 0);
            c->HSSetShader(nullptr, nullptr, 0);
            c->DSSetShader(nullptr, nullptr, 0);
            c->PSSetShaderResources(0, 1, &a_src);
            if (a_depth) c->PSSetShaderResources(1, 1, &a_depth);
            ID3D11SamplerState* smp = g.smp.Get();
            c->PSSetSamplers(0, 1, &smp);
            ID3D11Buffer* cb = g.cb.Get();
            c->PSSetConstantBuffers(0, 1, &cb);
            c->Draw(3, 0);
            ID3D11ShaderResourceView* noSrv[2] = {};
            c->PSSetShaderResources(0, a_depth ? 2 : 1, noSrv);
        }

        // ---- 11-13: the game's main depth buffer for the edge blur ---------------------------------------------------
        std::string s_depthStatus = "not needed yet";  // under s_statusLock

        void SetDepthStatus(std::string a_text) {
            std::scoped_lock lock(s_statusLock);
            s_depthStatus = std::move(a_text);
        }

        // The depth data sits at a versioned place in the renderer (CommonLib's GetDepthStencilData: SE 0x1FB8, AE before
        // 1.6.1130 0x1FB8, AE 1.6.1130+ incl. 1.6.1170 0x2018, VR 0x21D0 - the 1130 move is the one CommonLib already
        // knows). Before calling into what it holds, check the pointer is readable memory whose vtable lies in a loaded
        // module other than the game exe (11-13 review: a game object's slot 0 is usually its deleting destructor, so the
        // QueryInterface below would be fatal on one; D3D objects' vtables live in d3d11.dll or a wrapper DLL). The caller
        // then asks for the D3D type with QueryInterface, never a raw cast, so a wrong offset ends in a log line, not a crash.
        bool LooksLikeObject(const void* a_p) {
            if (!a_p) return false;
            MEMORY_BASIC_INFORMATION mbi{};
            if (!::VirtualQuery(a_p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
            constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                                        PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
            if (!(mbi.Protect & kReadable) || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
            const auto p   = reinterpret_cast<std::uintptr_t>(a_p);
            const auto end = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
            if (p + sizeof(void*) > end) return false;
            const void* vtbl = *static_cast<const void* const*>(a_p);
            HMODULE     mod  = nullptr;
            return ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCWSTR>(vtbl), &mod) &&
                   mod && mod != ::GetModuleHandleW(nullptr);
        }

        // A typeless depth texture's readable depth format (DXGI_FORMAT_UNKNOWN = not one we can read).
        DXGI_FORMAT DepthReadFormat(DXGI_FORMAT a_tex) {
            switch (a_tex) {
            case DXGI_FORMAT_R24G8_TYPELESS:    return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
            case DXGI_FORMAT_R32_TYPELESS:      return DXGI_FORMAT_R32_FLOAT;
            case DXGI_FORMAT_R16_TYPELESS:      return DXGI_FORMAT_R16_UNORM;
            default:                            return DXGI_FORMAT_UNKNOWN;
            }
        }

        bool ReadableDepthView(DXGI_FORMAT a_view) {
            return a_view == DXGI_FORMAT_R24_UNORM_X8_TYPELESS || a_view == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS ||
                   a_view == DXGI_FORMAT_R32_FLOAT || a_view == DXGI_FORMAT_R16_UNORM;
        }

        bool DepthFail(std::string a_why) {
            g.depthOk = false;
            g.depthSrv = nullptr;
            g.ownDepthSrv.Reset();
            SKSE::log::info("FrameFx: depth for the edge blur - {}; the edge blur blurs everything round the edge as before", a_why);
            SetDepthStatus("not available (" + a_why + ") - the whole edge blurs");
            return false;
        }

        // Main thread, in the hook: the main depth buffer's view for t1, or false (today's edge blur, one log line).
        bool EnsureDepth(UINT a_picW, UINT a_picH) {
            auto* r = RE::BSGraphics::Renderer::GetSingleton();
            if (!r) return false;
            const auto& ds  = r->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
            const void* tex = ds.texture;
            const void* srv = ds.depthSRV;
            if (g.depthChecked && tex == g.depthTexKey && srv == g.depthSrvKey) return g.depthOk;
            g.depthChecked = true;
            g.depthTexKey  = tex;
            g.depthSrvKey  = srv;
            g.depthOk      = false;
            g.depthSrv     = nullptr;
            g.ownDepthSrv.Reset();
            if (!tex && !srv) return DepthFail("the game's main depth buffer is not made yet");
            if ((srv && !LooksLikeObject(srv)) || (tex && !LooksLikeObject(tex))) {
                return DepthFail(std::format("the renderer's depth data does not look like D3D objects (texture {}, view {}) - "
                                             "CommonLib's offset may be wrong for this game version",
                    tex, srv));
            }
            // 11-13 (review): QueryInterface (slot 0, the same on every COM object) instead of raw casts, so a different
            // D3D object at a wrong offset is refused instead of having the wrong vtable slot called on it. srvRef holds
            // only the extra reference QueryInterface adds; the game keeps its own, which g.depthSrv relies on as before.
            ComPtr<ID3D11ShaderResourceView> srvRef;
            if (srv && FAILED(static_cast<IUnknown*>(const_cast<void*>(srv))->QueryInterface(IID_PPV_ARGS(&srvRef)))) {
                return DepthFail("the renderer's depth view is not a shader resource view - CommonLib's offset may be wrong "
                                 "for this game version");
            }
            ID3D11ShaderResourceView* gameSrv = srvRef.Get();
            ComPtr<ID3D11Texture2D>   t;
            if (gameSrv) {
                ComPtr<ID3D11Resource> res;
                gameSrv->GetResource(&res);
                if (!res || FAILED(res.As(&t))) return DepthFail("the game's depth view is not on a 2D texture");
            } else if (FAILED(static_cast<IUnknown*>(const_cast<void*>(tex))->QueryInterface(IID_PPV_ARGS(&t)))) {
                return DepthFail("the renderer's depth texture is not a 2D texture - CommonLib's offset may be wrong for this "
                                 "game version");
            }
            D3D11_TEXTURE2D_DESC td{};
            t->GetDesc(&td);
            if (!(td.BindFlags & D3D11_BIND_DEPTH_STENCIL)) {
                return DepthFail(std::format("the texture found is not a depth buffer (bind flags {:X}, format {})", td.BindFlags,
                    static_cast<int>(td.Format)));
            }
            if (td.SampleDesc.Count != 1) return DepthFail(std::format("the depth buffer is multisampled ({}x)", td.SampleDesc.Count));
            const char* whose = "the game's own view";
            DXGI_FORMAT viewFmt = DXGI_FORMAT_UNKNOWN;
            if (gameSrv) {
                D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
                gameSrv->GetDesc(&sd);
                if (sd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D && ReadableDepthView(sd.Format)) {
                    g.depthSrv = gameSrv;
                    viewFmt    = sd.Format;
                }
            }
            if (!g.depthSrv) {
                // no usable view from the game: make one, if the texture is typeless and may be read by shaders
                const DXGI_FORMAT f = DepthReadFormat(td.Format);
                if (f == DXGI_FORMAT_UNKNOWN) {
                    return DepthFail(std::format("the depth buffer's format {} is not a typeless one a shader can read",
                        static_cast<int>(td.Format)));
                }
                if (!(td.BindFlags & D3D11_BIND_SHADER_RESOURCE)) return DepthFail("the depth buffer cannot be bound to a shader");
                D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
                sd.Format                    = f;
                sd.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
                sd.Texture2D.MostDetailedMip = 0;
                sd.Texture2D.MipLevels       = 1;
                if (FAILED(g.dev->CreateShaderResourceView(t.Get(), &sd, &g.ownDepthSrv))) {
                    return DepthFail(std::format("could not make a view of the depth buffer (format {})", static_cast<int>(f)));
                }
                g.depthSrv = g.ownDepthSrv.Get();
                viewFmt    = f;
                whose      = "a view TFCam made";
            }
            g.depthW  = td.Width;
            g.depthH  = td.Height;
            g.depthOk = true;
            SKSE::log::info("FrameFx: depth for the edge blur - the game's main depth buffer, {}x{} (texture format {}, read "
                            "through {} in format {}){}; the subject and what is in front of it stay sharp",
                td.Width, td.Height, static_cast<int>(td.Format), whose, static_cast<int>(viewFmt),
                td.Width == a_picW && td.Height == a_picH ? "" : std::format(" - NOT the picture's size {}x{}", a_picW, a_picH));
            SetDepthStatus(std::format("reading the game's depth buffer ({}x{})", td.Width, td.Height));
            return true;
        }

        // ---- how the camera moved since the last frame ----------------------------------------------------------
        struct Motion {
            float x = 0.0f, y = 0.0f;  // uv: where last frame's middle is now, from the middle (y down)
            float zoom = 0.0f;         // > 0: rushing in (moving forward / zooming in)
            bool  valid = false;
        };

        Motion Measure() {
            static bool         s_have = false;
            static RE::NiPoint3 s_prevFar{}, s_prevPos{};
            static float        s_prevWide = 0.0f;
            Motion              m;
            auto*               nc = RE::Main::WorldRootCamera();
            if (!nc) {
                s_have = false;
                return m;
            }
            RE::NiPoint3 o{}, d{};
            if (!nc->WindowPointToRay(1000, 1000, o, d, 2000.0f, 2000.0f)) {
                s_have = false;
                return m;
            }
            const auto& fr   = nc->GetRuntimeData2().viewFrustum;
            const float wide = fr.fRight - fr.fLeft;  // the view's width at unit distance
            const RE::NiPoint3 farPt = o + d * 4000.0f;
            if (s_have) {
                float x = 0.0f, y = 0.0f, z = 0.0f;
                if (nc->WorldPtToScreenPt3(s_prevFar, x, y, z, 1e-5f) && z > 0.0f && std::isfinite(x) && std::isfinite(y)) {
                    m.x = x - 0.5f;
                    m.y = 0.5f - y;
                    const float forward = (o - s_prevPos).Dot(d);  // units moved along the view this frame
                    const float fovRate = wide > 1e-4f && s_prevWide > 1e-4f ? s_prevWide / wide - 1.0f : 0.0f;
                    m.zoom  = std::fabs(fovRate) + (std::max)(forward, 0.0f) / 300.0f;
                    m.valid = std::fabs(m.x) < 0.25f && std::fabs(m.y) < 0.25f && m.zoom < 0.5f;  // a cut / teleport: no smear
                }
            }
            s_prevFar  = farPt;
            s_prevPos  = o;
            s_prevWide = wide;
            s_have     = true;
            return m;
        }

        bool CameraShot() {
            auto* pc = RE::PlayerCamera::GetSingleton();
            return (pc && pc->IsInFreeCameraMode()) || FixedCam::IsOn();
        }

        bool On(int a_when, bool a_camera) { return a_when == kWhenAlways || (a_when == kWhenCamera && a_camera); }

        // ---- 10-31: the edge blur (Background blur, Edges style), eased so it never pops ---------------------------
        struct EdgeNow {
            float lod = 0.0f;  // mip levels of blur at the screen's edge
            float cx = 0.5f, cy = 0.5f;
            // 11-13: keep the subject sharp by depth. depthMix eases like lod; the subject's depth and the planes are
            // this frame's (kept through the fade out); probe* = the subject on screen, for the one-time depth check.
            float depthMix = 0.0f;
            float subjZ = 0.0f, zNear = 0.0f, zFar = 0.0f;
            float probeU = 0.5f, probeV = 0.5f, projZ = 0.0f;
            bool  probeOk = false;
        };

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
            float      depthWant = 0.0f;
            s_now.probeOk = false;
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
                            s_now.probeU  = x;
                            s_now.probeV  = 1.0f - y;
                            s_now.projZ   = z;
                            s_now.probeOk = x >= 0.0f && x <= 1.0f && y >= 0.0f && y <= 1.0f;
                        }
                        // 11-13 (user: "not blur the player"): the subject's view depth - along the world camera's
                        // forward axis (NiCamera looks along its local +X, column 0 of its world rotation; CamBlur's
                        // one-time view axis check logs it), the same depth the depth buffer holds.
                        if (want.byDepth) {
                            const auto&        m   = nc->world.rotate.entry;
                            const RE::NiPoint3 fwd{ m[0][0], m[1][0], m[2][0] };
                            const float        sz  = (want.subject - nc->world.translate).Dot(fwd);
                            const auto&        fr  = nc->GetRuntimeData2().viewFrustum;
                            if (std::isfinite(sz) && fr.fNear > 0.0f && fr.fFar > fr.fNear && sz > fr.fNear) {
                                depthWant   = 1.0f;
                                s_now.subjZ = sz;
                                s_now.zNear = fr.fNear;
                                s_now.zFar  = fr.fFar;
                            }
                        }
                    }
                }
            }
            const float kl = dt > 0.0f ? 1.0f - std::exp(-dt / 0.25f) : 1.0f;
            const float kc = dt > 0.0f ? 1.0f - std::exp(-dt / 0.15f) : 1.0f;
            // 11-13: from fully off it starts at its target (no blurred subject while the blur fades in); while the blur
            // fades out it keeps its last value (no pop); a subject lost mid-blur eases it off.
            if (s_now.lod <= 0.0f) {
                s_now.depthMix = depthWant;
            } else if (lod > 0.0f) {
                s_now.depthMix += (depthWant - s_now.depthMix) * kl;
            }
            s_now.lod += (lod - s_now.lod) * kl;
            if (lod <= 0.0f && s_now.lod < 0.02f) {
                s_now.lod      = 0.0f;
                s_now.depthMix = 0.0f;
            }
            if (lod > 0.0f) {
                s_now.cx += (cx - s_now.cx) * kc;
                s_now.cy += (cy - s_now.cy) * kc;
            }
            return s_now;
        }

        // ---- 10-31: the archer dot - where the arrow will land, while the string is drawn ---------------------------
        struct DotNow {
            float on = 0.0f;  // 0..1
            float cx = 0.5f, cy = 0.5f;
        };

        DotNow DotTick() {
            DotNow       d;
            RE::NiPoint3 aim{};
            float        in = 0.0f;
            if (!FovFx::Get().aimDot || CameraShot()) return d;  // never in the free / fixed camera
            // The projectiles go through the middle of the screen (TFCam's own aim, or Hit Where You Aim): the dot sits
            // there whenever something ranged is out. With Hit Where You Aim only while Action FOV is on, so a player who
            // turned nothing on in TFCam sees no change.
            if (AimDot::DotInCharge()) {
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

        // The effects' constants for this frame; false = nothing to do.
        // a_depth (11-13): the depth buffer is bound for this draw (the edge blur may keep the subject sharp).
        bool Constants(const Motion& a_m, const EdgeNow& a_e, const DotNow& a_d, bool a_depth, UINT a_w, UINT a_h, FxCB& a_cb) {
            const auto& s      = s_settings;
            const bool  camera = CameraShot();
            a_cb               = {};
            a_cb.aspect        = a_h > 0 ? static_cast<float>(a_w) / static_cast<float>(a_h) : 1.7778f;
            a_cb.taps          = 1.0f;
            a_cb.invSize[0]    = a_w > 0 ? 1.0f / static_cast<float>(a_w) : 0.0f;
            a_cb.invSize[1]    = a_h > 0 ? 1.0f / static_cast<float>(a_h) : 0.0f;
            bool any           = false;
            if (a_e.lod > 0.01f && g.mips) {
                a_cb.edgeBlur      = a_e.lod;
                a_cb.edgeSharp     = CamBlur::Get().sharp;
                a_cb.edgeCenter[0] = a_e.cx;
                a_cb.edgeCenter[1] = a_e.cy;
                any                = true;
                if (a_depth && a_e.depthMix > 0.001f) {
                    // 11-13: sharp up to fSubjectDepth behind the subject, fully blurred fBlurRampDepth further on
                    const auto& bl     = CamBlur::Get();
                    a_cb.depthMix      = (std::min)(a_e.depthMix, 1.0f);
                    a_cb.subjZ         = a_e.subjZ;
                    a_cb.depthNear     = a_e.subjZ + bl.subjectDepth;
                    a_cb.depthFar      = a_cb.depthNear + (std::max)(bl.rampDepth, 1.0f);
                    a_cb.zNear         = a_e.zNear;
                    a_cb.zFar          = a_e.zFar;
                    a_cb.probeUv[0]    = a_e.probeU;
                    a_cb.probeUv[1]    = a_e.probeV;
                    a_cb.depthSize[0]  = static_cast<float>(g.depthW);
                    a_cb.depthSize[1]  = static_cast<float>(g.depthH);
                }
            }
            if (a_d.on > 0.0f) {
                a_cb.dotCenter[0] = a_d.cx;
                a_cb.dotCenter[1] = a_d.cy;
                a_cb.dotSize      = FovFx::Get().aimDotSize * (std::max)(static_cast<float>(a_h) / 1080.0f, 0.5f);
                a_cb.dotOn        = a_d.on;
                any               = true;
            }
            if (On(s.motionBlur, camera) && a_m.valid) {
                float bx = a_m.x * s.shutter, by = a_m.y * s.shutter;
                const float px  = std::hypot(bx * static_cast<float>(a_w), by * static_cast<float>(a_h));
                const float cap = s.maxBlur * static_cast<float>(a_w);
                if (px > cap && px > 0.0f) {
                    bx *= cap / px;
                    by *= cap / px;
                }
                float zoom = s.zoomBlur ? (std::min)(a_m.zoom * s.shutter, 0.12f) : 0.0f;
                const float zoomPx = zoom * 0.5f * static_cast<float>(a_w);  // the smear at the screen's edge
                const float longest = (std::max)((std::min)(px, cap), zoomPx);
                if (longest >= 1.0f) {
                    a_cb.blur[0] = bx;
                    a_cb.blur[1] = by;
                    a_cb.zoom    = zoom;
                    a_cb.taps    = std::clamp(std::ceil(longest / 2.0f), 4.0f, 24.0f);  // about a tap every 2 pixels
                    any          = true;
                }
            }
            if (On(s.lens, camera)) {
                a_cb.distort    = s.distortion;
                a_cb.fringe     = s.fringe;
                a_cb.vignette   = s.vignette;
                a_cb.streaks    = s.streaks;
                a_cb.streakFrom = s.streakFrom;
                a_cb.bars       = s.bars ? (std::max)(0.0f, 0.5f * (1.0f - a_cb.aspect / 2.39f)) : 0.0f;
                any = any || s.distortion != 0.0f || s.fringe > 0.0f || s.vignette > 0.0f || s.streaks > 0.0f || a_cb.bars > 0.0f;
            }
            if (const float av = FovFx::ArcherVignette(); av > 0.001f) {  // 10-31: darker corners while the string is drawn
                a_cb.vignette = (std::max)(a_cb.vignette, av);
                any           = true;
            }
            return any;
        }

        // 11-13: the one-time depth check. Once the edge blur has kept a subject sharp for a moment, a 1x1 draw (the same
        // constants) reads the depth buffer at the subject; a later frame reads it back without ever waiting on the GPU and
        // logs it next to the subject's own view depth, so the next test shows whether the depth reads right.
        void Probe(const EdgeNow& a_e) {
            static int     s_frames = 0;
            static EdgeNow s_at;
            if (g.probeState == 2) return;
            if (g.probeState == 1) {
                D3D11_MAPPED_SUBRESOURCE map{};
                const HRESULT hr = g.ctx->Map(g.probeRead.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &map);
                if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return;
                g.probeState = 2;
                if (FAILED(hr)) {
                    SKSE::log::warn("FrameFx: subject depth check - could not read the probe back ({:08X})", static_cast<unsigned>(hr));
                    return;
                }
                float v[4]{};
                std::memcpy(v, map.pData, sizeof(v));
                g.ctx->Unmap(g.probeRead.Get(), 0);
                SKSE::log::info("FrameFx: subject depth check - the depth buffer at the subject (screen {:.3f}, {:.3f}) holds "
                                "{:.6f} = {:.1f} units from the camera; the subject's point is {:.1f} units away (the shader "
                                "saw {:.1f}); near {:.2f}, far {:.0f}, the game's projected z {:.6f}. Difference {:+.1f}: a few "
                                "to some tens of units in front (negative) = the depth reads right; a big positive difference "
                                "or a value of 1 = the depth is cleared or reversed here",
                    s_at.probeU, s_at.probeV, v[0], v[1], s_at.subjZ, v[2], s_at.zNear, s_at.zFar, s_at.projZ, v[1] - s_at.subjZ);
                return;
            }
            if (!a_e.probeOk || a_e.depthMix < 0.99f || ++s_frames < 30) return;
            if (!g.probe) {
                D3D11_TEXTURE2D_DESC d{};
                d.Width = d.Height = 1;
                d.MipLevels        = 1;
                d.ArraySize        = 1;
                d.Format           = DXGI_FORMAT_R32G32B32A32_FLOAT;
                d.SampleDesc.Count = 1;
                d.Usage            = D3D11_USAGE_DEFAULT;
                d.BindFlags        = D3D11_BIND_RENDER_TARGET;
                bool ok            = SUCCEEDED(g.dev->CreateTexture2D(&d, nullptr, &g.probe));
                ok                 = ok && SUCCEEDED(g.dev->CreateRenderTargetView(g.probe.Get(), nullptr, &g.probeRtv));
                d.Usage            = D3D11_USAGE_STAGING;
                d.BindFlags        = 0;
                d.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;
                ok                 = ok && SUCCEEDED(g.dev->CreateTexture2D(&d, nullptr, &g.probeRead));
                if (!ok) {
                    g.probeState = 2;
                    SKSE::log::warn("FrameFx: subject depth check - could not make its 1x1 probe; no check this session");
                    return;
                }
            }
            Draw(g.probeRtv.Get(), nullptr, 1, 1, g.ps[4].Get(), g.depthSrv);
            g.ctx->CopyResource(g.probeRead.Get(), g.probe.Get());
            s_at         = a_e;
            g.probeState = 1;
        }

        void Frame() {
            const Motion  m   = Measure();  // every frame, so the next frame's motion is right
            const EdgeNow e   = EdgeTick();  // every frame, so it eases
            const DotNow  dot = DotTick();
            const bool    rec = VideoCapture::WantsFrames();
            const auto&   s   = s_settings;
            if (!rec && s.motionBlur == kWhenOff && s.lens == kWhenOff && e.lod <= 0.01f && dot.on <= 0.0f) return;
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
            // 11-13: the depth buffer only when the edge blur wants to keep a subject sharp
            const bool depth = e.lod > 0.01f && e.depthMix > 0.001f && g.mips && EnsureDepth(td.Width, td.Height);
            FxCB       cb{};
            const bool fx = Constants(m, e, dot, depth, td.Width, td.Height, cb);
            if (!fx && !rec) return;
            StateBackup backup;
            backup.Capture(g.ctx);
            // the top mip only (the copy may have a mip chain, the frame buffer has not)
            g.ctx->CopySubresourceRegion(g.copy.Get(), 0, 0, 0, 0, tex.Get(), 0, nullptr);
            if (fx) {
                if (cb.edgeBlur > 0.0f) g.ctx->GenerateMips(g.fxSrv.Get());  // 10-31: the edge blur's smaller pictures
                D3D11_MAPPED_SUBRESOURCE map{};
                if (SUCCEEDED(g.ctx->Map(g.cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
                    std::memcpy(map.pData, &cb, sizeof(cb));
                    g.ctx->Unmap(g.cb.Get(), 0);
                    Draw(rtv.Get(), g.fxSrv.Get(), td.Width, td.Height, g.ps[0].Get(), cb.depthMix > 0.0f ? g.depthSrv : nullptr);
                    if (cb.depthMix > 0.0f) Probe(e);
                    if (rec) g.ctx->CopySubresourceRegion(g.copy.Get(), 0, 0, 0, 0, tex.Get(), 0, nullptr);  // the video gets the effects too
                }
            }
            if (rec) {
                s_inHook = true;
                VideoCapture::OnFrame(g.videoSrv.Get(), td.Width, td.Height);
                s_inHook = false;
            }
            backup.Restore(g.ctx);
            SetStatus(fx ? "on" : "idle (nothing to show right now)", false);  // changes every time the camera stops / moves
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

    Settings& Get() { return s_settings; }

    void Clamp() {
        auto& s     = s_settings;
        auto  range = [](float& v, float lo, float hi, float def) { v = std::isfinite(v) ? std::clamp(v, lo, hi) : def; };
        s.motionBlur = std::clamp(s.motionBlur, 0, 2);
        s.lens       = std::clamp(s.lens, 0, 2);
        range(s.shutter, 0.05f, 1.0f, 0.5f);
        range(s.maxBlur, 0.01f, 0.15f, 0.05f);
        range(s.distortion, -1.0f, 1.0f, 0.0f);
        range(s.fringe, 0.0f, 1.0f, 0.0f);
        range(s.vignette, 0.0f, 1.0f, 0.0f);
        range(s.streaks, 0.0f, 1.0f, 0.0f);
        range(s.streakFrom, 0.3f, 1.0f, 0.85f);
    }

    void Install() {
        if (REL::Module::IsVR()) {
            SKSE::log::info("FrameFx: not on VR");
            return;
        }
        REL::Relocation<std::uintptr_t> draw{ RELOCATION_ID(35556, 36555) };
        const std::uintptr_t site = draw.address() + REL::Relocate(0x3ab, 0x371);
        if (*reinterpret_cast<const std::uint8_t*>(site) != 0xE8) {
            SKSE::log::warn("FrameFx: Main::Draw's menu call is not a call here (byte {:02X}) - no image effects / video",
                *reinterpret_cast<const std::uint8_t*>(site));
            SetStatus("off - the game's draw call was not found");
            return;
        }
        MenuDrawHook::func = SKSE::GetTrampoline().write_call<5>(site, MenuDrawHook::thunk);
        SKSE::log::info("FrameFx: Main::Draw's menu draw hooked (image effects + video, before the UI)");
    }

    std::string Status() {
        std::scoped_lock lock(s_statusLock);
        return s_status;
    }

    std::string DepthStatus() {
        std::scoped_lock lock(s_statusLock);
        return s_depthStatus;
    }

    void Blit(ID3D11RenderTargetView* a_rtv, ID3D11ShaderResourceView* a_src, unsigned a_w, unsigned a_h, int a_shader) {
        if (!s_inHook || s_init <= 0 || a_shader < 0 || a_shader > 2) return;
        Draw(a_rtv, a_src, a_w, a_h, g.ps[1 + a_shader].Get());
    }
}
