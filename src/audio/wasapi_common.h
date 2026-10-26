#pragma once

// Shared pieces of the two WASAPI halves: mix-format validation and HRESULT
// mapping. Both capture and render open the same kind of endpoint and accept
// the same two mix formats (16-bit PCM and 32-bit float, plain or extensible),
// so the parsing lives here instead of being duplicated.

#include <windows.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <propsys.h>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
}

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "chast/com_ptr.h"
#include "chast/error.h"

namespace chast::wasapi {

inline int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// The endpoint's persistent id, used to notice that the user moved the default
// output. Empty when Windows cannot produce one.
inline std::wstring device_id(IMMDevice* device) {
    if (!device) {
        return {};
    }
    LPWSTR id = nullptr;
    if (FAILED(device->GetId(&id)) || !id) {
        return {};
    }
    std::wstring result(id);
    CoTaskMemFree(id);
    return result;
}

inline std::wstring default_render_device_id(IMMDeviceEnumerator* enumerator) {
    if (!enumerator) {
        return {};
    }
    ComPtr<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.put()))) {
        return {};
    }
    return device_id(device.get());
}

inline bool device_active(IMMDevice* device) {
    if (!device) {
        return false;
    }
    DWORD state = 0;
    return SUCCEEDED(device->GetState(&state)) && state == DEVICE_STATE_ACTIVE;
}

inline Error error_from_hresult(HRESULT hr, const char* what) {
    char text[256] = {};
    const DWORD length =
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, static_cast<DWORD>(hr), 0, text, sizeof(text), nullptr);
    while (length > 0 && (text[length - 1] == '\r' || text[length - 1] == '\n' ||
                          text[length - 1] == ' ')) {
        text[length - 1] = '\0';
    }
    Status status = Status::CaptureFailed;
    if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
        status = Status::Unavailable;
    } else if (hr == AUDCLNT_E_UNSUPPORTED_FORMAT) {
        status = Status::Unsupported;
    } else if (hr == AUDCLNT_E_DEVICE_IN_USE) {
        status = Status::NotCurrentlyAvailable;
    }
    std::string message = std::string(what) + " failed (0x";
    char code[16] = {};
    std::snprintf(code, sizeof(code), "%08lx", static_cast<unsigned long>(hr));
    message += code;
    message += ")";
    if (length > 0) {
        message += ": ";
        message += text;
    }
    return Error(status, std::move(message));
}

// The endpoint's mix format, validated and copied. `raw` keeps the original
// WAVEFORMATEX (including cbSize) because IAudioClient::Initialize wants it
// back verbatim.
struct MixFormat {
    uint32_t sample_rate = 0;
    uint16_t channels = 0;
    AVSampleFormat format = AV_SAMPLE_FMT_NONE;  // S16 or FLT
    DWORD channel_mask = 0;
    std::vector<uint8_t> raw;

    const WAVEFORMATEX* wave() const {
        return reinterpret_cast<const WAVEFORMATEX*>(raw.data());
    }
};

inline bool parse_mix_format(const WAVEFORMATEX* mix, MixFormat& out, std::string* error) {
    if (!mix) {
        if (error) {
            *error = "the audio endpoint reported no mix format";
        }
        return false;
    }
    bool pcm = false;
    bool floating = false;
    DWORD channel_mask = 0;
    if (mix->wFormatTag == WAVE_FORMAT_PCM) {
        pcm = true;
    } else if (mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        floating = true;
    } else if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->cbSize >= 22) {
        const auto* extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix);
        channel_mask = extended->dwChannelMask;
        if (IsEqualGUID(extended->SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) {
            pcm = true;
        } else if (IsEqualGUID(extended->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
            floating = true;
        }
    }

    if (!pcm && !floating) {
        if (error) {
            *error = "the audio endpoint mixes in a format that is not PCM or float";
        }
        return false;
    }
    if (pcm && mix->wBitsPerSample != 16) {
        if (error) {
            *error = "the audio endpoint mixes " + std::to_string(mix->wBitsPerSample) +
                     "-bit PCM; 16-bit is required";
        }
        return false;
    }
    if (floating && mix->wBitsPerSample != 32) {
        if (error) {
            *error = "the audio endpoint mixes " + std::to_string(mix->wBitsPerSample) +
                     "-bit float; 32-bit is required";
        }
        return false;
    }
    if (mix->nChannels < 1 || mix->nChannels > 8) {
        if (error) {
            *error = "the audio endpoint mixes " + std::to_string(mix->nChannels) +
                     " channels";
        }
        return false;
    }
    if (mix->nSamplesPerSec < 8000 || mix->nSamplesPerSec > 192000) {
        if (error) {
            *error = "the audio endpoint mixes at " +
                     std::to_string(mix->nSamplesPerSec) + " Hz";
        }
        return false;
    }

    out.sample_rate = mix->nSamplesPerSec;
    out.channels = static_cast<uint16_t>(mix->nChannels);
    out.format = floating ? AV_SAMPLE_FMT_FLT : AV_SAMPLE_FMT_S16;
    out.channel_mask = channel_mask;
    const size_t bytes = sizeof(WAVEFORMATEX) + mix->cbSize;
    out.raw.assign(reinterpret_cast<const uint8_t*>(mix),
                   reinterpret_cast<const uint8_t*>(mix) + bytes);
    return true;
}

// Input channel layout for swresample. Windows speaker masks use the same bit
// values as FFmpeg's AV_CH_* constants; when the mask is missing or disagrees
// with the channel count, FFmpeg's default layout for that count is used.
inline AVChannelLayout input_layout(const MixFormat& format) {
    AVChannelLayout layout{};
    if (format.channel_mask != 0) {
        if (av_channel_layout_from_mask(&layout, format.channel_mask) < 0 ||
            layout.nb_channels != format.channels) {
            av_channel_layout_uninit(&layout);
        }
    }
    if (layout.nb_channels == 0) {
        av_channel_layout_default(&layout, format.channels);
    }
    return layout;
}

inline std::wstring widen_utf8(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring wide(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                        static_cast<int>(text.size()), wide.data(), size);
    return wide;
}

inline std::string narrow_utf8(const wchar_t* text) {
    if (!text || !*text) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }
    std::string narrow(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, narrow.data(), size, nullptr, nullptr);
    return narrow;
}

// PKEY_Device_FriendlyName, spelled out so callers do not need the
// function-discovery header's full property-key list.
inline constexpr PROPERTYKEY kFriendlyNameKey = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

inline std::string friendly_name(IMMDevice* device) {
    if (!device) {
        return {};
    }
    ComPtr<IPropertyStore> store;
    if (FAILED(device->OpenPropertyStore(STGM_READ, store.put()))) {
        return {};
    }
    PROPVARIANT value;
    PropVariantInit(&value);
    std::string name;
    if (SUCCEEDED(store->GetValue(kFriendlyNameKey, &value)) &&
        value.vt == VT_LPWSTR) {
        name = narrow_utf8(value.pwszVal);
    }
    PropVariantClear(&value);
    return name;
}

}  // namespace chast::wasapi
