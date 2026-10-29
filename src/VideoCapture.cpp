#include "VideoCapture.h"
#include "FrameFx.h"

#include <Windows.h>
#include <audioclient.h>
#include <d3d11.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mmdeviceapi.h>
#include <propidl.h>
#include <shlobj.h>
#include <wrl/client.h>
#include <wrl/ftm.h>
#include <wrl/implements.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace VideoCapture {

    namespace {
        using Microsoft::WRL::ComPtr;

        // What TFCam's Windows 7 build target hides (the same values as the Windows 10 SDK).
        constexpr GUID kContainerType = { 0x150ff23f, 0x4abc, 0x478b, { 0xac, 0x4f, 0xe1, 0x91, 0x6f, 0xba, 0x1c, 0xca } };  // MF_TRANSCODE_CONTAINERTYPE
        constexpr GUID kFmpeg4        = { 0x9ba876f1, 0x419f, 0x4b77, { 0xa1, 0xe0, 0x35, 0x95, 0x9d, 0x9d, 0x40, 0x04 } };  // MFTranscodeContainerType_FMPEG4
        constexpr GUID kHardwareMfts  = { 0xa634a91c, 0x822b, 0x41b9, { 0xa4, 0x94, 0x4d, 0xe4, 0x64, 0x36, 0x12, 0xb0 } };  // MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS
        struct LoopbackParams {                  // AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS
            DWORD targetProcessId;
            int   mode;                          // 0 = include the target's process tree
        };
        struct ActivationParams {                // AUDIOCLIENT_ACTIVATION_PARAMS
            int            type;                 // 1 = process loopback
            LoopbackParams loopback;
        };

        constexpr UINT     kRate      = 48000;
        constexpr UINT     kChannels  = 2;
        constexpr UINT     kFrameSize = kChannels * 2;  // 16-bit samples
        constexpr int      kSlots     = 4;              // read-back ring (2 frames late)
        constexpr size_t   kMaxQueued = 45;             // pictures waiting for the encoder before new ones are dropped

        Settings s_settings;

        double QpcFreq() {
            static const double f = [] {
                LARGE_INTEGER x;
                ::QueryPerformanceFrequency(&x);
                return static_cast<double>(x.QuadPart);
            }();
            return f;
        }
        LONGLONG Qpc() {
            LARGE_INTEGER t;
            ::QueryPerformanceCounter(&t);
            return t.QuadPart;
        }

        void Notify(std::string a_text) {
            if (auto* tasks = SKSE::GetTaskInterface()) {
                tasks->AddTask([text = std::move(a_text)] { RE::SendHUDMessage::ShowHUDMessage(text.c_str()); });
            }
        }

        // ---- a recording, shared by the main thread, the writer and the sound thread --------------------------------
        struct VFrame {
            std::vector<BYTE> nv12;
            LONGLONG          time = 0, dur = 0;  // 100 ns
        };
        struct AChunk {
            std::vector<BYTE> pcm;
            LONGLONG          time = 0, dur = 0;
        };

        struct Session {
            std::filesystem::path   path;
            UINT                    w = 0, h = 0, fps = 30, bitrate = 0;
            bool                    audio = true;
            LONGLONG                startQpc = 0;
            std::mutex              m;
            std::condition_variable cv;
            std::deque<VFrame>      video;
            std::deque<AChunk>      sound;
            bool                    stop = false;
            std::thread             soundThread;
            std::atomic<bool>       soundStop{ false };
            std::atomic<int>        soundState{ 0 };  // 0 starting, 1 on, -1 off
            std::atomic<bool>       failed{ false };  // the writer gave up: the main thread ends the recording
            std::atomic<uint64_t>   frames{ 0 }, dropped{ 0 };

            LONGLONG Since() const { return static_cast<LONGLONG>(static_cast<double>(Qpc() - startQpc) * 1e7 / QpcFreq()); }
        };

        std::mutex  s_statusLock;
        std::string s_status = "not recording";
        void SetStatus(std::string a_text) {
            std::scoped_lock lock(s_statusLock);
            s_status = std::move(a_text);
        }

        std::filesystem::path VideosFolder() {
            PWSTR raw = nullptr;
            std::filesystem::path out;
            if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &raw)) && raw) out = std::filesystem::path(raw) / L"TFCam";
            if (raw) ::CoTaskMemFree(raw);
            if (out.empty()) out = std::filesystem::path(L"Data\\SKSE\\Plugins\\TFCam\\Videos");
            std::error_code ec;
            std::filesystem::create_directories(out, ec);
            return out;
        }

        std::string Name(const std::filesystem::path& a_p) {
            const auto u8 = a_p.filename().u8string();
            return std::string(u8.begin(), u8.end());
        }

        // ---- the sound: only Skyrim's own (process loopback, Windows 10 2004+) ------------------------------------
        class Activator : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                              Microsoft::WRL::FtmBase, IActivateAudioInterfaceCompletionHandler> {
        public:
            HANDLE done = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
            ~Activator() override {
                if (done) ::CloseHandle(done);
            }
            STDMETHOD(ActivateCompleted)(IActivateAudioInterfaceAsyncOperation*) override {
                ::SetEvent(done);
                return S_OK;
            }
        };

        void PushSound(Session& a_s, const BYTE* a_data, UINT32 a_frames, uint64_t& a_written) {
            AChunk c;
            c.pcm.resize(static_cast<size_t>(a_frames) * kFrameSize);
            if (a_data) {
                std::memcpy(c.pcm.data(), a_data, c.pcm.size());
            }  // else silence (zeros)
            c.time = static_cast<LONGLONG>(a_written * 10000000ull / kRate);
            c.dur  = static_cast<LONGLONG>(static_cast<uint64_t>(a_frames) * 10000000ull / kRate);
            a_written += a_frames;
            {
                std::scoped_lock lock(a_s.m);
                a_s.sound.push_back(std::move(c));
            }
            a_s.cv.notify_one();
        }

        // Silence up to a_upTo sample frames, in pieces of at most half a second.
        void PadSilence(Session& a_s, uint64_t a_upTo, uint64_t& a_written) {
            while (a_written < a_upTo) {
                const auto n = static_cast<UINT32>((std::min<uint64_t>)(a_upTo - a_written, kRate / 2));
                PushSound(a_s, nullptr, n, a_written);
            }
        }

        void SoundThread(std::shared_ptr<Session> a_s) {
            const HRESULT co = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            auto          fail = [&](const char* a_what, HRESULT a_hr) {
                SKSE::log::warn("VideoCapture: no sound - {} ({:08X}); the video is recorded without it", a_what, static_cast<unsigned>(a_hr));
                a_s->soundState = -1;
            };
            ActivationParams ap{ 1, { ::GetCurrentProcessId(), 0 } };
            PROPVARIANT      pv{};
            pv.vt             = VT_BLOB;
            pv.blob.cbSize    = sizeof(ap);
            pv.blob.pBlobData = reinterpret_cast<BYTE*>(&ap);
            auto                                          act = Microsoft::WRL::Make<Activator>();
            ComPtr<IActivateAudioInterfaceAsyncOperation> op;
            HRESULT hr = act ? ::ActivateAudioInterfaceAsync(L"VAD\\Process_Loopback", __uuidof(IAudioClient), &pv, act.Get(), &op) : E_OUTOFMEMORY;
            ComPtr<IAudioClient>        client;
            ComPtr<IAudioCaptureClient> capture;
            HANDLE                      ev = nullptr;
            if (FAILED(hr)) {
                fail("process loopback is not available (Windows 10 2004 or newer)", hr);
            } else if (::WaitForSingleObject(act->done, 5000) != WAIT_OBJECT_0) {
                fail("the sound device did not answer", E_ABORT);
            } else {
                HRESULT          ar = E_FAIL;
                ComPtr<IUnknown> unk;
                op->GetActivateResult(&ar, &unk);
                if (FAILED(ar) || !unk || FAILED(unk.As(&client))) {
                    fail("could not open Skyrim's sound", FAILED(ar) ? ar : E_NOINTERFACE);
                } else {
                    WAVEFORMATEX wf{};
                    wf.wFormatTag      = WAVE_FORMAT_PCM;
                    wf.nChannels       = kChannels;
                    wf.nSamplesPerSec  = kRate;
                    wf.wBitsPerSample  = 16;
                    wf.nBlockAlign     = kFrameSize;
                    wf.nAvgBytesPerSec = kRate * kFrameSize;
                    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                        AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM, 200000, 0, &wf,
                        nullptr);
                    ev = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
                    if (SUCCEEDED(hr)) hr = client->SetEventHandle(ev);
                    if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&capture));
                    if (SUCCEEDED(hr)) hr = client->Start();
                    if (FAILED(hr)) fail("could not start the sound capture", hr);
                }
            }
            if (capture) {
                a_s->soundState = 1;
                SKSE::log::info("VideoCapture: recording Skyrim's own sound (48 kHz stereo)");
                uint64_t written = 0;  // sample frames
                while (!a_s->soundStop) {
                    ::WaitForSingleObject(ev, 30);
                    UINT32 packet = 0;
                    while (SUCCEEDED(capture->GetNextPacketSize(&packet)) && packet > 0) {
                        BYTE*  data   = nullptr;
                        UINT32 frames = 0;
                        DWORD  flags  = 0;
                        if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
                        // the sound keeps step with the picture's clock: a gap (nothing playing) is filled with silence
                        const uint64_t now = static_cast<uint64_t>((std::max<LONGLONG>)(a_s->Since(), 0)) * kRate / 10000000ull;
                        if (now > written + frames + kRate / 25) PadSilence(*a_s, now - frames, written);
                        PushSound(*a_s, (flags & AUDCLNT_BUFFERFLAGS_SILENT) ? nullptr : data, frames, written);
                        capture->ReleaseBuffer(frames);
                    }
                    const uint64_t now = static_cast<uint64_t>((std::max<LONGLONG>)(a_s->Since(), 0)) * kRate / 10000000ull;
                    if (now > written + kRate / 16) PadSilence(*a_s, now - kRate / 50, written);
                }
                client->Stop();
            }
            if (ev) ::CloseHandle(ev);
            if (SUCCEEDED(co)) ::CoUninitialize();
        }

        // ---- the writer: H.264 + AAC into a fragmented MP4 --------------------------------------------------------
        HRESULT VideoStream(IMFSinkWriter* a_w, const Session& a_s, DWORD& a_index) {
            ComPtr<IMFMediaType> out, in;
            HRESULT              hr = ::MFCreateMediaType(&out);
            if (SUCCEEDED(hr)) hr = out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            if (SUCCEEDED(hr)) hr = out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
            if (SUCCEEDED(hr)) hr = out->SetUINT32(MF_MT_AVG_BITRATE, a_s.bitrate);
            if (SUCCEEDED(hr)) hr = out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            if (SUCCEEDED(hr)) hr = ::MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, a_s.w, a_s.h);
            if (SUCCEEDED(hr)) hr = ::MFSetAttributeRatio(out.Get(), MF_MT_FRAME_RATE, a_s.fps, 1);
            if (SUCCEEDED(hr)) hr = ::MFSetAttributeRatio(out.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
            if (SUCCEEDED(hr)) hr = a_w->AddStream(out.Get(), &a_index);
            if (SUCCEEDED(hr)) hr = ::MFCreateMediaType(&in);
            if (SUCCEEDED(hr)) hr = in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            if (SUCCEEDED(hr)) hr = in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
            if (SUCCEEDED(hr)) hr = in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            if (SUCCEEDED(hr)) hr = in->SetUINT32(MF_MT_DEFAULT_STRIDE, a_s.w);
            if (SUCCEEDED(hr)) hr = ::MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, a_s.w, a_s.h);
            if (SUCCEEDED(hr)) hr = ::MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, a_s.fps, 1);
            if (SUCCEEDED(hr)) hr = ::MFSetAttributeRatio(in.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
            if (SUCCEEDED(hr)) hr = a_w->SetInputMediaType(a_index, in.Get(), nullptr);
            return hr;
        }

        HRESULT SoundStream(IMFSinkWriter* a_w, DWORD& a_index) {
            ComPtr<IMFMediaType> out, in;
            HRESULT              hr = ::MFCreateMediaType(&out);
            if (SUCCEEDED(hr)) hr = out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            if (SUCCEEDED(hr)) hr = out->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
            if (SUCCEEDED(hr)) hr = out->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            if (SUCCEEDED(hr)) hr = out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kRate);
            if (SUCCEEDED(hr)) hr = out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kChannels);
            if (SUCCEEDED(hr)) hr = out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000);  // 192 kbit/s
            if (SUCCEEDED(hr)) hr = a_w->AddStream(out.Get(), &a_index);
            if (SUCCEEDED(hr)) hr = ::MFCreateMediaType(&in);
            if (SUCCEEDED(hr)) hr = in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            if (SUCCEEDED(hr)) hr = in->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
            if (SUCCEEDED(hr)) hr = in->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            if (SUCCEEDED(hr)) hr = in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kRate);
            if (SUCCEEDED(hr)) hr = in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kChannels);
            if (SUCCEEDED(hr)) hr = in->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, kFrameSize);
            if (SUCCEEDED(hr)) hr = in->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kRate * kFrameSize);
            if (SUCCEEDED(hr)) hr = a_w->SetInputMediaType(a_index, in.Get(), nullptr);
            return hr;
        }

        HRESULT WriteBytes(IMFSinkWriter* a_w, DWORD a_index, const std::vector<BYTE>& a_bytes, LONGLONG a_time, LONGLONG a_dur) {
            ComPtr<IMFMediaBuffer> buf;
            HRESULT                hr = ::MFCreateMemoryBuffer(static_cast<DWORD>(a_bytes.size()), &buf);
            BYTE*                  p  = nullptr;
            if (SUCCEEDED(hr)) hr = buf->Lock(&p, nullptr, nullptr);
            if (SUCCEEDED(hr)) {
                std::memcpy(p, a_bytes.data(), a_bytes.size());
                buf->Unlock();
                hr = buf->SetCurrentLength(static_cast<DWORD>(a_bytes.size()));
            }
            ComPtr<IMFSample> sample;
            if (SUCCEEDED(hr)) hr = ::MFCreateSample(&sample);
            if (SUCCEEDED(hr)) hr = sample->AddBuffer(buf.Get());
            if (SUCCEEDED(hr)) hr = sample->SetSampleTime(a_time);
            if (SUCCEEDED(hr)) hr = sample->SetSampleDuration(a_dur);
            if (SUCCEEDED(hr)) hr = a_w->WriteSample(a_index, sample.Get());
            return hr;
        }

        void Writer(std::shared_ptr<Session> a_s) {
            const HRESULT co = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            HRESULT       hr = ::MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
            ComPtr<IMFSinkWriter> writer;
            DWORD                 vi = 0, ai = 0;
            bool                  sound = false;
            std::string           error;
            if (FAILED(hr)) {
                error = std::format("Windows' media framework did not start ({:08X}) - Windows N needs the Media Feature Pack", static_cast<unsigned>(hr));
            } else {
                ComPtr<IMFAttributes> attrs;
                hr = ::MFCreateAttributes(&attrs, 2);
                if (SUCCEEDED(hr)) attrs->SetUINT32(kHardwareMfts, TRUE);
                if (SUCCEEDED(hr)) attrs->SetGUID(kContainerType, kFmpeg4);
                if (SUCCEEDED(hr)) hr = ::MFCreateSinkWriterFromURL(a_s->path.c_str(), nullptr, attrs.Get(), &writer);
                if (FAILED(hr) && attrs) {  // no fragmented MP4 here: a plain one (finished when the recording stops)
                    attrs->DeleteItem(kContainerType);
                    hr = ::MFCreateSinkWriterFromURL(a_s->path.c_str(), nullptr, attrs.Get(), &writer);
                }
                if (SUCCEEDED(hr)) hr = VideoStream(writer.Get(), *a_s, vi);
                if (SUCCEEDED(hr) && a_s->audio) {
                    sound = SUCCEEDED(SoundStream(writer.Get(), ai));
                    if (!sound) SKSE::log::warn("VideoCapture: the AAC sound stream could not be set up - no sound");
                }
                if (SUCCEEDED(hr)) hr = writer->BeginWriting();
                if (FAILED(hr)) error = std::format("the H.264 writer could not start ({:08X})", static_cast<unsigned>(hr));
            }
            if (!error.empty()) {
                SKSE::log::error("VideoCapture: {}", error);
                SetStatus("Recording failed: " + error);
                Notify("Video recording failed - see FreeCam > Image & Video");
                a_s->soundStop = true;
                a_s->failed    = true;
            }
            uint64_t written = 0;
            for (;;) {
                std::deque<VFrame> video;
                std::deque<AChunk> snd;
                bool               stopping = false;
                {
                    std::unique_lock lock(a_s->m);
                    a_s->cv.wait(lock, [&] { return a_s->stop || !a_s->video.empty() || !a_s->sound.empty(); });
                    stopping = a_s->stop;
                }
                if (stopping && a_s->soundThread.joinable()) a_s->soundThread.join();  // its last pieces are queued now
                {
                    std::scoped_lock lock(a_s->m);
                    video.swap(a_s->video);
                    snd.swap(a_s->sound);
                }
                if (writer && error.empty()) {
                    for (const auto& f : video) {
                        if (FAILED(hr = WriteBytes(writer.Get(), vi, f.nv12, f.time, f.dur))) break;
                        ++written;
                    }
                    if (sound && SUCCEEDED(hr)) {
                        for (const auto& c : snd) {
                            if (FAILED(hr = WriteBytes(writer.Get(), ai, c.pcm, c.time, c.dur))) break;
                        }
                    }
                    if (FAILED(hr)) {
                        error = std::format("writing failed ({:08X})", static_cast<unsigned>(hr));
                        SKSE::log::error("VideoCapture: {}", error);
                        SetStatus("Recording failed: " + error);
                        Notify("Video recording failed - see FreeCam > Image & Video");
                        a_s->soundStop = true;
                        a_s->failed    = true;
                    }
                }
                if (stopping) break;
            }
            if (writer) {
                const HRESULT fin = writer->Finalize();
                writer.Reset();
                if (error.empty()) {
                    const auto name = Name(a_s->path);
                    SKSE::log::info("VideoCapture: saved {} - {} frames, {} dropped ({:08X})", a_s->path.string(), written,
                        a_s->dropped.load(), static_cast<unsigned>(fin));
                    SetStatus(std::format("Last video: {} ({} frames{})", name, written,
                        a_s->dropped ? std::format(", {} dropped", a_s->dropped.load()) : ""));
                    Notify("Video saved: Videos\\TFCam\\" + name);
                }
            }
            ::MFShutdown();
            if (SUCCEEDED(co)) ::CoUninitialize();
        }

        // ---- the main thread side (FrameFx's hook) ---------------------------------------------------------------
        std::atomic<bool> s_want{ false };
        std::atomic<bool> s_active{ false };

        struct Slot {
            ComPtr<ID3D11Texture2D> y, uv;  // staging
            bool                    filled = false;
            uint64_t                call   = 0;
            LONGLONG                time = 0, dur = 0;
        };
        struct Gpu {
            ID3D11Device*                  dev = nullptr;
            ID3D11DeviceContext*           ctx = nullptr;
            ComPtr<ID3D11Texture2D>        yTex, uvTex;
            ComPtr<ID3D11RenderTargetView> yRtv, uvRtv;
            Slot                           slots[kSlots];
        };
        Gpu                      g;
        std::shared_ptr<Session> s_session;
        uint64_t                 s_call     = 0;
        LONGLONG                 s_nextDue  = 0;
        LONGLONG                 s_frameDur = 0;

        bool MakeTex(UINT a_w, UINT a_h, DXGI_FORMAT a_f, bool a_staging, ComPtr<ID3D11Texture2D>& a_out) {
            D3D11_TEXTURE2D_DESC d{};
            d.Width            = a_w;
            d.Height           = a_h;
            d.MipLevels        = 1;
            d.ArraySize        = 1;
            d.Format           = a_f;
            d.SampleDesc.Count = 1;
            d.Usage            = a_staging ? D3D11_USAGE_STAGING : D3D11_USAGE_DEFAULT;
            d.BindFlags        = a_staging ? 0 : D3D11_BIND_RENDER_TARGET;
            d.CPUAccessFlags   = a_staging ? D3D11_CPU_ACCESS_READ : 0;
            return SUCCEEDED(g.dev->CreateTexture2D(&d, nullptr, &a_out));
        }

        void ReleaseGpu() {
            g.yTex.Reset();
            g.uvTex.Reset();
            g.yRtv.Reset();
            g.uvRtv.Reset();
            for (auto& s : g.slots) s = {};
        }

        bool Begin(unsigned a_w, unsigned a_h) {
            auto* r = RE::BSGraphics::Renderer::GetSingleton();
            if (!r) return false;
            g.dev = reinterpret_cast<ID3D11Device*>(r->GetRuntimeData().forwarder);
            g.ctx = reinterpret_cast<ID3D11DeviceContext*>(r->GetRuntimeData().context);
            if (!g.dev || !g.ctx || a_w < 64 || a_h < 64) return false;
            if (!::LoadLibraryW(L"mfplat.dll") || !::LoadLibraryW(L"mfreadwrite.dll")) {
                SetStatus("Recording is not possible: Windows' media framework is missing (Windows N: install the Media Feature Pack)");
                Notify("Video recording needs Windows' media framework");
                return false;
            }
            const auto& s  = s_settings;
            UINT        oh = s.height <= 0 || static_cast<unsigned>(s.height) >= a_h ? a_h : static_cast<UINT>(s.height);
            UINT        ow = static_cast<UINT>(std::lround(static_cast<double>(a_w) * oh / a_h));
            ow &= ~1u;
            oh &= ~1u;
            bool ok = MakeTex(ow, oh, DXGI_FORMAT_R8_UNORM, false, g.yTex) && MakeTex(ow / 2, oh / 2, DXGI_FORMAT_R8G8_UNORM, false, g.uvTex);
            ok = ok && SUCCEEDED(g.dev->CreateRenderTargetView(g.yTex.Get(), nullptr, &g.yRtv)) &&
                 SUCCEEDED(g.dev->CreateRenderTargetView(g.uvTex.Get(), nullptr, &g.uvRtv));
            for (auto& sl : g.slots) {
                ok = ok && MakeTex(ow, oh, DXGI_FORMAT_R8_UNORM, true, sl.y) && MakeTex(ow / 2, oh / 2, DXGI_FORMAT_R8G8_UNORM, true, sl.uv);
            }
            if (!ok) {
                ReleaseGpu();
                SetStatus(std::format("Recording failed: no GPU textures for {}x{}", ow, oh));
                return false;
            }
            auto ses      = std::make_shared<Session>();
            ses->w        = ow;
            ses->h        = oh;
            ses->fps      = static_cast<UINT>(s.fps);
            ses->audio    = s.audio;
            const double bpp[3] = { 0.10, 0.17, 0.28 };  // bits per pixel per frame
            ses->bitrate  = static_cast<UINT>(std::clamp(static_cast<double>(ow) * oh * s.fps * bpp[std::clamp(s.quality, 0, 2)], 2e6, 120e6));
            std::time_t now = std::time(nullptr);
            std::tm     tm{};
            ::localtime_s(&tm, &now);
            ses->path     = VideosFolder() / std::format(L"TFCam {:04}-{:02}-{:02} {:02}-{:02}-{:02}.mp4", tm.tm_year + 1900, tm.tm_mon + 1,
                tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
            ses->startQpc = Qpc();
            if (ses->audio) {
                ses->soundThread = std::thread(SoundThread, ses);
            } else {
                ses->soundState = -1;
            }
            std::thread(Writer, ses).detach();
            s_session  = ses;
            s_call     = 0;
            s_nextDue  = 0;
            s_frameDur = 10000000LL / ses->fps;
            s_active   = true;
            SKSE::log::info("VideoCapture: recording {} - {}x{} (picture {}x{}), {} fps, {:.1f} Mbit/s, sound {}", ses->path.string(), ow, oh, a_w, a_h,
                ses->fps, ses->bitrate / 1e6, ses->audio ? "on" : "off");
            Notify(std::format("Recording video ({}x{}, {} fps)", ow, oh, ses->fps));
            return true;
        }

        // A filled slot's picture to the writer. a_wait: block for the GPU (stopping), else try again next frame.
        void ReadBack(Slot& a_sl, bool a_wait) {
            auto*                    c = g.ctx;
            D3D11_MAPPED_SUBRESOURCE my{}, muv{};
            const UINT               flags = a_wait ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT;
            if (FAILED(c->Map(a_sl.y.Get(), 0, D3D11_MAP_READ, flags, &my))) return;
            if (FAILED(c->Map(a_sl.uv.Get(), 0, D3D11_MAP_READ, flags, &muv))) {
                c->Unmap(a_sl.y.Get(), 0);
                return;
            }
            auto&  s = *s_session;
            VFrame f;
            f.time = a_sl.time;
            f.dur  = a_sl.dur;
            f.nv12.resize(static_cast<size_t>(s.w) * s.h * 3 / 2);
            BYTE* dst = f.nv12.data();
            for (UINT row = 0; row < s.h; ++row) std::memcpy(dst + static_cast<size_t>(row) * s.w, static_cast<const BYTE*>(my.pData) + static_cast<size_t>(row) * my.RowPitch, s.w);
            dst += static_cast<size_t>(s.w) * s.h;
            for (UINT row = 0; row < s.h / 2; ++row) std::memcpy(dst + static_cast<size_t>(row) * s.w, static_cast<const BYTE*>(muv.pData) + static_cast<size_t>(row) * muv.RowPitch, s.w);
            c->Unmap(a_sl.uv.Get(), 0);
            c->Unmap(a_sl.y.Get(), 0);
            a_sl.filled = false;
            {
                std::scoped_lock lock(s.m);
                if (s.video.size() >= kMaxQueued) {
                    ++s.dropped;
                    return;
                }
                s.video.push_back(std::move(f));
            }
            ++s.frames;
            s.cv.notify_one();
        }

        void End(std::string_view a_why) {
            if (!s_session) return;
            for (auto& sl : g.slots) {
                if (sl.filled) ReadBack(sl, true);
            }
            auto ses       = std::move(s_session);
            ses->soundStop = true;
            {
                std::scoped_lock lock(ses->m);
                ses->stop = true;
            }
            ses->cv.notify_one();
            ReleaseGpu();
            s_active = false;
            SKSE::log::info("VideoCapture: stopping ({}) - {} frames sent, {} dropped", a_why, ses->frames.load(), ses->dropped.load());
            if (!ses->failed) SetStatus("Saving the video...");
        }
    }

    Settings& Get() { return s_settings; }

    void Clamp() {
        auto& s   = s_settings;
        s.height  = s.height <= 0 ? 0 : std::clamp(s.height, 360, 4320);
        s.fps     = s.fps <= 24 ? 24 : (s.fps <= 30 ? 30 : 60);
        s.quality = std::clamp(s.quality, 0, 2);
    }

    bool Recording() { return s_want || s_active; }
    bool WantsFrames() { return s_want || s_active; }

    void RequestStart(std::string_view a_why) {
        if (!s_want.exchange(true)) SKSE::log::info("VideoCapture: start requested ({})", a_why);
    }

    void RequestStop(std::string_view a_why) {
        if (s_want.exchange(false)) SKSE::log::info("VideoCapture: stop requested ({})", a_why);
    }

    void Toggle(std::string_view a_why) {
        if (s_want) {
            RequestStop(a_why);
        } else {
            RequestStart(a_why);
        }
    }

    void OnFrame(ID3D11ShaderResourceView* a_src, unsigned a_w, unsigned a_h) {
        if (!s_session) {
            if (!s_want) return;
            if (!Begin(a_w, a_h)) {
                s_want = false;
                return;
            }
        }
        ++s_call;
        for (auto& sl : g.slots) {
            if (sl.filled && s_call - sl.call >= 2) ReadBack(sl, false);
        }
        if (!s_want || s_session->failed) {
            s_want = false;
            End(s_session->failed ? "the writer failed" : "stopped");
            return;
        }
        auto&          ses = *s_session;
        const LONGLONG now = ses.Since();
        if (now < s_nextDue) return;  // the game runs faster than the video: this frame is not needed
        const LONGLONG first = s_nextDue;
        int            count = 0;
        while (now >= s_nextDue && count < 120) {
            s_nextDue += s_frameDur;
            ++count;
        }
        if (now >= s_nextDue) s_nextDue = now + s_frameDur;  // a long stall: no burst afterwards
        Slot* slot = nullptr;
        for (auto& sl : g.slots) {
            if (!sl.filled) {
                slot = &sl;
                break;
            }
        }
        if (!slot) {
            ses.dropped += static_cast<uint64_t>(count);
            return;
        }
        FrameFx::Blit(g.yRtv.Get(), a_src, ses.w, ses.h, FrameFx::kShaderLuma);
        FrameFx::Blit(g.uvRtv.Get(), a_src, ses.w / 2, ses.h / 2, FrameFx::kShaderChroma);
        g.ctx->CopyResource(slot->y.Get(), g.yTex.Get());
        g.ctx->CopyResource(slot->uv.Get(), g.uvTex.Get());
        slot->filled = true;
        slot->call   = s_call;
        slot->time   = first;
        slot->dur    = static_cast<LONGLONG>(count) * s_frameDur;
        if (ses.frames % 30 == 0) {
            const LONGLONG secs  = now / 10000000LL;
            const int      sound = ses.soundState.load();
            SetStatus(std::format("Recording {}:{:02} - {}x{}, {} fps, sound {}{}", secs / 60, secs % 60, ses.w, ses.h, ses.fps,
                sound > 0 ? "on" : (sound < 0 ? "off" : "starting"),
                ses.dropped ? std::format(", {} frames dropped", ses.dropped.load()) : ""));
        }
    }

    void OnGameLoad() { RequestStop("a game load"); }

    std::string Status() {
        std::scoped_lock lock(s_statusLock);
        return s_status;
    }
}
