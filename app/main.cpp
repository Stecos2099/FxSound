#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <memory>
#include <cmath>
#include <cstdint>
#include "DfxDsp.h"

#pragma comment(lib, "avrt.lib")

using Microsoft::WRL::ComPtr;

static std::wstring lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), towlower);
    return s;
}

static bool contains_ci(const std::wstring& s, const std::wstring& q) {
    return lower(s).find(lower(q)) != std::wstring::npos;
}

struct Endpoint {
    ComPtr<IMMDevice> dev;
    std::wstring id, name;
};

static std::vector<Endpoint> playback_devices() {
    std::vector<Endpoint> out;

    ComPtr<IMMDeviceEnumerator> en;
    CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        IID_PPV_ARGS(&en)
    );

    if (!en)
        return out;

    ComPtr<IMMDeviceCollection> col;

    if (FAILED(en->EnumAudioEndpoints(
        eRender,
        DEVICE_STATE_ACTIVE,
        &col)))
        return out;

    UINT n = 0;
    col->GetCount(&n);

    for (UINT i = 0; i < n; i++) {
        ComPtr<IMMDevice> d;

        if (FAILED(col->Item(i, &d)))
            continue;

        LPWSTR id = nullptr;
        d->GetId(&id);

        PROPVARIANT pv;
        PropVariantInit(&pv);

        std::wstring name = L"(unknown)";

        ComPtr<IPropertyStore> ps;

        if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps)) &&
            SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) &&
            pv.vt == VT_LPWSTR) {
            name = pv.pwszVal;
        }

        if (id) {
            out.push_back({ d, id, name });
            CoTaskMemFree(id);
        }

        PropVariantClear(&pv);
    }

    return out;
}

static Endpoint find_device(
    const std::vector<Endpoint>& ds,
    const std::wstring& query) {

    for (auto& d : ds)
        if (contains_ci(d.name, query))
            return d;

    for (auto& d : ds)
        if (contains_ci(d.id, query))
            return d;

    return {};
}

static int16_t f32_to_i16(float x) {
    x = std::max(-1.0f, std::min(1.0f, x));

    // Avoid lrintf() here because of the MSVC/Windows SDK
    // compilation issue encountered with the original source.
    return static_cast<int16_t>(
        std::round(x * 32767.0f)
    );
}

static float i16_to_f32(int16_t x) {
    return static_cast<float>(x) / 32768.0f;
}

class Path {
public:
    int index;
    std::wstring inputQ;
    std::vector<std::wstring> outputQ;
    std::atomic<bool> stop{ false };
    std::thread th;

    Path(
        int i,
        std::wstring in,
        std::vector<std::wstring> out)
        : index(i),
          inputQ(std::move(in)),
          outputQ(std::move(out)) {
    }

    void start() {
        th = std::thread(&Path::run, this);
    }

    void join() {
        if (th.joinable())
            th.join();
    }

private:
    void run() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        auto ds = playback_devices();

        auto in = find_device(ds, inputQ);

        if (!in.dev) {
            std::wcerr
                << L"[PATH " << index
                << L"] Input device not found: "
                << inputQ << L"\n";

            CoUninitialize();
            return;
        }

        struct Out {
            Endpoint ep;
            ComPtr<IAudioClient> client;
            ComPtr<IAudioRenderClient> render;
            WAVEFORMATEX* fmt = nullptr;
        };

        std::vector<Out> outs;

        for (auto& q : outputQ) {
            auto ep = find_device(ds, q);

            if (!ep.dev) {
                std::wcerr
                    << L"[PATH " << index
                    << L"] Output device not found: "
                    << q << L"\n";

                continue;
            }

            outs.push_back({ ep });
        }

        if (outs.empty()) {
            CoUninitialize();
            return;
        }

        std::wcout
            << L"[PATH " << index << L"] "
            << in.name
            << L" -> FxDSP -> ";

        for (size_t i = 0; i < outs.size(); i++) {
            std::wcout
                << outs[i].ep.name
                << (i + 1 < outs.size() ? L" + " : L"\n");
        }

        ComPtr<IAudioClient> ic;

        if (FAILED(in.dev->Activate(
            __uuidof(IAudioClient),
            CLSCTX_ALL,
            nullptr,
            (void**)&ic))) {

            std::wcerr
                << L"[PATH " << index
                << L"] Input IAudioClient activation failed\n";

            CoUninitialize();
            return;
        }

        WAVEFORMATEX* ifmt = nullptr;

        if (FAILED(ic->GetMixFormat(&ifmt))) {
            std::wcerr
                << L"[PATH " << index
                << L"] GetMixFormat failed\n";

            CoUninitialize();
            return;
        }

        REFERENCE_TIME dur = 10000000;

        HRESULT hr = ic->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK,
            dur,
            0,
            ifmt,
            nullptr
        );

        if (FAILED(hr)) {
            std::wcerr
                << L"[PATH " << index
                << L"] Loopback Initialize failed: 0x"
                << std::hex
                << hr
                << std::dec
                << L"\n";

            CoTaskMemFree(ifmt);
            CoUninitialize();
            return;
        }

        for (auto& o : outs) {
            if (FAILED(o.ep.dev->Activate(
                __uuidof(IAudioClient),
                CLSCTX_ALL,
                nullptr,
                (void**)&o.client))) {
                continue;
            }

            if (FAILED(o.client->GetMixFormat(&o.fmt)))
                continue;

            hr = o.client->Initialize(
                AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
                dur,
                0,
                o.fmt,
                nullptr
            );

            if (FAILED(hr)) {
                o.client.Reset();
                continue;
            }

            if (FAILED(o.client->GetService(
                IID_PPV_ARGS(&o.render)))) {
                o.client.Reset();
                continue;
            }
        }

        outs.erase(
            std::remove_if(
                outs.begin(),
                outs.end(),
                [](const Out& o) {
                    return !o.client || !o.render;
                }),
            outs.end()
        );

        if (outs.empty()) {
            std::wcerr
                << L"[PATH " << index
                << L"] No usable outputs\n";

            CoTaskMemFree(ifmt);
            CoUninitialize();
            return;
        }

        UINT inCh = ifmt->nChannels;
        UINT rate = ifmt->nSamplesPerSec;

        for (auto& o : outs) {
            if (o.fmt->nChannels != inCh ||
                o.fmt->nSamplesPerSec != rate) {

                std::wcerr
                    << L"[PATH " << index
                    << L"] WARNING: output "
                    << o.ep.name
                    << L" format differs ("
                    << o.fmt->nChannels
                    << L"ch/"
                    << o.fmt->nSamplesPerSec
                    << L"Hz). It will be skipped.\n";
            }
        }

        DfxDsp dsp;

        int dspRc = dsp.setSignalFormat(
            16,
            static_cast<int>(inCh),
            static_cast<int>(rate),
            16
        );

        if (dspRc != 0) {
            std::wcerr
                << L"[PATH " << index
                << L"] DfxDsp setSignalFormat returned "
                << dspRc
                << L"\n";
        }

        dsp.powerOn(true);
        dsp.eqOn(true);

        ComPtr<IAudioCaptureClient> cap;

        if (FAILED(ic->GetService(IID_PPV_ARGS(&cap)))) {
            std::wcerr
                << L"[PATH " << index
                << L"] Capture service failed\n";

            CoTaskMemFree(ifmt);
            CoUninitialize();
            return;
        }

        ic->Start();

        for (auto& o : outs)
            o.client->Start();

        DWORD task = 0;

        HANDLE avrt =
            AvSetMmThreadCharacteristicsW(
                L"Pro Audio",
                &task
            );

        std::vector<short> in16;
        std::vector<short> out16;

        while (!stop) {
            UINT32 packet = 0;

            if (FAILED(cap->GetNextPacketSize(&packet)))
                break;

            if (!packet) {
                Sleep(2);
                continue;
            }

            while (packet && !stop) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;

                // Windows 11 SDK / current WASAPI signature:
                // GetBuffer(
                //     BYTE**,
                //     UINT32*,
                //     DWORD*,
                //     UINT64*,
                //     UINT64*
                // )
                UINT64 devicePosition = 0;
                UINT64 qpcPosition = 0;

                if (FAILED(cap->GetBuffer(
                    &data,
                    &frames,
                    &flags,
                    &devicePosition,
                    &qpcPosition))) {

                    stop = true;
                    break;
                }

                size_t samples =
                    static_cast<size_t>(frames) * inCh;

                in16.resize(samples);
                out16.resize(samples);

                bool inFloat =
                    (ifmt->wFormatTag ==
                        WAVE_FORMAT_IEEE_FLOAT) ||
                    (ifmt->wFormatTag ==
                        WAVE_FORMAT_EXTENSIBLE &&
                     ((WAVEFORMATEXTENSIBLE*)ifmt)->SubFormat ==
                        KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);

                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    std::fill(
                        in16.begin(),
                        in16.end(),
                        0
                    );
                }
                else if (inFloat) {
                    const float* p =
                        reinterpret_cast<const float*>(data);

                    for (size_t i = 0; i < samples; i++)
                        in16[i] = f32_to_i16(p[i]);
                }
                else if (ifmt->wBitsPerSample == 16) {
                    memcpy(
                        in16.data(),
                        data,
                        samples * sizeof(short)
                    );
                }
                else if (ifmt->wBitsPerSample == 32) {
                    const int32_t* p =
                        reinterpret_cast<const int32_t*>(data);

                    for (size_t i = 0; i < samples; i++)
                        in16[i] =
                            static_cast<short>(p[i] >> 16);
                }
                else {
                    std::fill(
                        in16.begin(),
                        in16.end(),
                        0
                    );
                }

                dsp.processAudio(
                    in16.data(),
                    out16.data(),
                    static_cast<int>(frames),
                    0
                );

                for (auto& o : outs) {
                    if (o.fmt->nChannels != inCh ||
                        o.fmt->nSamplesPerSec != rate)
                        continue;

                    UINT32 pad = 0;
                    UINT32 total = 0;

                    if (FAILED(
                        o.client->GetCurrentPadding(&pad)))
                        continue;

                    if (FAILED(
                        o.client->GetBufferSize(&total)))
                        continue;

                    UINT32 avail =
                        (total > pad)
                        ? total - pad
                        : 0;

                    if (avail < frames)
                        continue;

                    BYTE* od = nullptr;

                    if (FAILED(
                        o.render->GetBuffer(
                            frames,
                            &od)))
                        continue;

                    bool outFloat =
                        (o.fmt->wFormatTag ==
                            WAVE_FORMAT_IEEE_FLOAT) ||
                        (o.fmt->wFormatTag ==
                            WAVE_FORMAT_EXTENSIBLE &&
                         ((WAVEFORMATEXTENSIBLE*)o.fmt)->SubFormat ==
                            KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);

                    size_t os =
                        static_cast<size_t>(frames) *
                        o.fmt->nChannels;

                    if (outFloat) {
                        float* p =
                            reinterpret_cast<float*>(od);

                        for (size_t i = 0; i < os; i++)
                            p[i] =
                                i16_to_f32(out16[i]);
                    }
                    else if (o.fmt->wBitsPerSample == 16) {
                        memcpy(
                            od,
                            out16.data(),
                            os * sizeof(short)
                        );
                    }
                    else if (o.fmt->wBitsPerSample == 32) {
                        int32_t* p =
                            reinterpret_cast<int32_t*>(od);

                        for (size_t i = 0; i < os; i++)
                            p[i] =
                                static_cast<int32_t>(out16[i])
                                << 16;
                    }
                    else {
                        memset(
                            od,
                            0,
                            frames * o.fmt->nBlockAlign
                        );
                    }

                    o.render->ReleaseBuffer(
                        frames,
                        0
                    );
                }

                cap->ReleaseBuffer(frames);

                if (FAILED(
                    cap->GetNextPacketSize(&packet))) {
                    stop = true;
                    break;
                }
            }
        }

        ic->Stop();

        for (auto& o : outs)
            o.client->Stop();

        if (avrt)
            AvRevertMmThreadCharacteristics(avrt);

        for (auto& o : outs) {
            if (o.fmt)
                CoTaskMemFree(o.fmt);
        }

        CoTaskMemFree(ifmt);

        CoUninitialize();

        std::wcout
            << L"[PATH " << index
            << L"] stopped\n";
    }
};

struct MapEntry {
    std::wstring input;
    std::vector<std::wstring> outputs;
};

static std::vector<MapEntry> defaultMap = {
    {L"VAIO1", {L"Philips"}},
    {L"VAIO2", {L"HP"}},
    {L"VAIO3", {L"Azona"}},
    {L"VAIO4", {L"Philips", L"HP"}},
    {L"VAIO5", {L"Philips", L"Azona"}},
    {L"VAIO6", {L"HP", L"Azona"}},
    {L"VAIO7", {L"Philips", L"HP", L"Azona"}}
};

int wmain() {
    std::wcout
        << L"FxMultiHost 7-Path (experimental)\n";

    std::wcout
        << L"FxSound DfxDsp -> WASAPI loopback -> physical outputs\n\n";

    auto ds = playback_devices();

    std::wcout
        << L"Active playback devices:\n";

    for (auto& d : ds)
        std::wcout
            << L"  "
            << d.name
            << L"\n";

    std::wcout << L"\n";

    std::wcout
        << L"IMPORTANT: disable VB-Matrix direct VAIO->speaker routes "
           L"for paths handled here.\n";

    std::wcout
        << L"Path 4/5/6/7 use the same processed stream to multiple "
           L"physical speakers.\n";

    std::wcout
        << L"This first 7-path build maps combinations by opening "
           L"each physical output separately.\n\n";

    std::wcout
        << L"Configuration is compiled in this first build:\n";

    for (int i = 0; i < 7; i++) {
        std::wcout
            << L"  VAIO"
            << i + 1
            << L" -> ";

        for (size_t j = 0;
             j < defaultMap[i].outputs.size();
             j++) {

            std::wcout
                << defaultMap[i].outputs[j]
                << (j + 1 <
                        defaultMap[i].outputs.size()
                    ? L" + "
                    : L"\n");
        }
    }

    std::vector<std::unique_ptr<Path>> paths;

    for (int i = 0; i < 7; i++) {
        paths.emplace_back(
            std::make_unique<Path>(
                i + 1,
                defaultMap[i].input,
                defaultMap[i].outputs
            )
        );

        paths.back()->start();
    }

    std::wcout
        << L"\nRunning. Press ENTER to stop.\n";

    std::wstring line;
    std::getline(std::wcin, line);

    for (auto& p : paths)
        p->stop = true;

    for (auto& p : paths)
        p->join();

    return 0;
}
