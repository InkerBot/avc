#include "audio/UsbIpAudio.hpp"

// USB/IP and UAC1 protocol behavior is derived from Virtual-Cables by Tarek
// Wasfy and AI (BSD-2-Clause). See third_party/Virtual-Cables-LICENSE.txt.

#include "log/Log.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <winver.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace avc::audio {
namespace {

using Byte = std::uint8_t;
using Clock = std::chrono::steady_clock;

constexpr std::uint16_t kUsbIpVersion = 0x0111;
constexpr std::uint16_t kOpReqImport = 0x8003;
constexpr std::uint16_t kOpRepImport = 0x0003;
constexpr std::uint16_t kOpReqDevlist = 0x8005;
constexpr std::uint16_t kOpRepDevlist = 0x0005;
constexpr std::uint32_t kCmdSubmit = 0x00000001;
constexpr std::uint32_t kCmdUnlink = 0x00000002;
constexpr std::uint32_t kRetSubmit = 0x00000003;
constexpr std::uint32_t kRetUnlink = 0x00000004;
constexpr std::uint32_t kDirectionOut = 0;
constexpr std::uint32_t kDirectionIn = 1;
constexpr std::uint32_t kNoIsoPackets = 0xffffffffU;
constexpr std::int32_t kStatusOk = 0;
constexpr std::int32_t kStatusPipe = -32;
constexpr std::uint32_t kDefaultSampleRate = 48000;
constexpr std::uint32_t kMaxSampleRate = 192000;
constexpr std::size_t kRingFrames = 16384;
constexpr std::uint32_t kMaxDevices = 32;
constexpr std::uint32_t kMaxTransferLength = 16U * 1024U * 1024U;

#ifdef AVC_USBIP_INSTALLER_NAME
constexpr std::string_view kBundledInstallerName = AVC_USBIP_INSTALLER_NAME;
constexpr std::string_view kBundledInstallerSha256 = AVC_USBIP_INSTALLER_SHA256;
#endif

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

struct FileVersion {
    std::uint16_t major = 0;
    std::uint16_t minor = 0;
    std::uint16_t patch = 0;
    std::uint16_t build = 0;
};

// Only 0.9.7.8 carries the known unsafe behavior. Older releases such as
// 0.9.7.7 and future releases must not be rejected by a range comparison.
constexpr FileVersion kBlockedUsbIpVersion{0, 9, 7, 8};

constexpr bool sameVersion(const FileVersion &left, const FileVersion &right) noexcept
{
    return left.major == right.major && left.minor == right.minor &&
           left.patch == right.patch && left.build == right.build;
}

static_assert(sameVersion(FileVersion{0, 9, 7, 8}, kBlockedUsbIpVersion));
static_assert(!sameVersion(FileVersion{0, 9, 7, 7}, kBlockedUsbIpVersion));

std::string versionString(const FileVersion &version)
{
    return std::to_string(version.major) + "." + std::to_string(version.minor) + "." +
           std::to_string(version.patch) + "." + std::to_string(version.build);
}

std::optional<FileVersion> executableVersion(const std::wstring &path)
{
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size == 0) return std::nullopt;

    std::vector<Byte> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return std::nullopt;

    VS_FIXEDFILEINFO *fixed = nullptr;
    UINT fixed_size = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void **>(&fixed), &fixed_size) ||
        fixed == nullptr || fixed_size < sizeof(VS_FIXEDFILEINFO) ||
        fixed->dwSignature != 0xfeef04bdU) {
        return std::nullopt;
    }
    return FileVersion{static_cast<std::uint16_t>(HIWORD(fixed->dwFileVersionMS)),
                       static_cast<std::uint16_t>(LOWORD(fixed->dwFileVersionMS)),
                       static_cast<std::uint16_t>(HIWORD(fixed->dwFileVersionLS)),
                       static_cast<std::uint16_t>(LOWORD(fixed->dwFileVersionLS))};
}

bool usbIpClientIsAllowed(const std::wstring &executable, std::string &error)
{
    const std::optional<FileVersion> version = executableVersion(executable);
    if (version && sameVersion(*version, kBlockedUsbIpVersion)) {
        error = "usbip-win2 " + versionString(*version) +
                " is blocked because this specific release can corrupt kernel memory and crash "
                "Windows; install 0.9.7.7 or another release";
        return false;
    }
    return true;
}

std::uint16_t readBe16(const Byte *p) noexcept
{
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) | p[1]);
}

std::uint32_t readBe32(const Byte *p) noexcept
{
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

std::uint16_t readLe16(const Byte *p) noexcept
{
    return static_cast<std::uint16_t>(p[0] | (static_cast<std::uint16_t>(p[1]) << 8));
}

void putLe16(Byte *p, std::uint16_t value) noexcept
{
    p[0] = static_cast<Byte>(value);
    p[1] = static_cast<Byte>(value >> 8);
}

void appendBe16(std::vector<Byte> &out, std::uint16_t value)
{
    out.push_back(static_cast<Byte>(value >> 8));
    out.push_back(static_cast<Byte>(value));
}

void appendBe32(std::vector<Byte> &out, std::uint32_t value)
{
    out.push_back(static_cast<Byte>(value >> 24));
    out.push_back(static_cast<Byte>(value >> 16));
    out.push_back(static_cast<Byte>(value >> 8));
    out.push_back(static_cast<Byte>(value));
}

void appendBeI32(std::vector<Byte> &out, std::int32_t value)
{
    appendBe32(out, static_cast<std::uint32_t>(value));
}

void appendZeros(std::vector<Byte> &out, std::size_t count)
{
    out.insert(out.end(), count, Byte{0});
}

bool recvAll(SOCKET socket, void *raw, std::size_t size) noexcept
{
    auto *data = static_cast<char *>(raw);
    while (size > 0) {
        const int chunk = static_cast<int>(std::min<std::size_t>(size, INT_MAX));
        const int got = recv(socket, data, chunk, 0);
        if (got <= 0) return false;
        data += got;
        size -= static_cast<std::size_t>(got);
    }
    return true;
}

bool sendAll(SOCKET socket, const void *raw, std::size_t size) noexcept
{
    const auto *data = static_cast<const char *>(raw);
    while (size > 0) {
        const int chunk = static_cast<int>(std::min<std::size_t>(size, INT_MAX));
        const int sent = send(socket, data, chunk, 0);
        if (sent <= 0) return false;
        data += sent;
        size -= static_cast<std::size_t>(sent);
    }
    return true;
}

bool sendAll(SOCKET socket, const std::vector<Byte> &data) noexcept
{
    return sendAll(socket, data.data(), data.size());
}

void appendFixed(std::vector<Byte> &out, std::string_view text, std::size_t width)
{
    const std::size_t used = std::min(width, text.size());
    out.insert(out.end(), text.begin(), text.begin() + static_cast<std::ptrdiff_t>(used));
    appendZeros(out, width - used);
}

std::vector<Byte> stringDescriptor(std::string_view utf8)
{
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                    static_cast<int>(utf8.size()), nullptr, 0);
    if (count <= 0) {
        count =
            MultiByteToWideChar(CP_ACP, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    }
    std::wstring wide(static_cast<std::size_t>(std::max(count, 0)), L'\0');
    if (count > 0) {
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                static_cast<int>(utf8.size()), wide.data(), count) <= 0) {
            (void)MultiByteToWideChar(CP_ACP, 0, utf8.data(), static_cast<int>(utf8.size()),
                                      wide.data(), count);
        }
    }
    if (wide.size() > 126) wide.resize(126);

    std::vector<Byte> out(2 + wide.size() * 2);
    out[0] = static_cast<Byte>(out.size());
    out[1] = 0x03;
    for (std::size_t i = 0; i < wide.size(); ++i) {
        putLe16(out.data() + 2 + i * 2, static_cast<std::uint16_t>(wide[i]));
    }
    return out;
}

struct SharedRing {
    alignas(64) std::atomic<std::uint64_t> read_{0};
    alignas(64) std::atomic<std::uint64_t> write_{0};
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<std::uint64_t> underruns_{0};
    std::array<float, kRingFrames> samples_{};
};

struct SharedAudio {
    std::uint32_t magic = 0x41564355U; // AVCU
    std::uint32_t version = 1;
    std::uint32_t channels = 0;
    std::uint32_t reserved = 0;
    std::atomic<Byte> configuration{0};
    std::atomic<Byte> playback_alt{0};
    std::atomic<Byte> capture_alt{0};
    std::atomic<std::uint32_t> sample_rate{kDefaultSampleRate};
    std::atomic<bool> mute{false};
    std::atomic<std::int16_t> volume{0};
    std::array<SharedRing, 32> playback;
    std::array<SharedRing, 32> capture;
};

bool ringPush(SharedRing &ring, float sample) noexcept
{
    const std::uint64_t write = ring.write_.load(std::memory_order_relaxed);
    const std::uint64_t read = ring.read_.load(std::memory_order_acquire);
    if (write - read >= ring.samples_.size()) {
        ring.dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    ring.samples_[write % ring.samples_.size()] = sample;
    ring.write_.store(write + 1, std::memory_order_release);
    return true;
}

bool ringPop(SharedRing &ring, float &sample) noexcept
{
    const std::uint64_t read = ring.read_.load(std::memory_order_relaxed);
    const std::uint64_t write = ring.write_.load(std::memory_order_acquire);
    if (read == write) {
        ring.underruns_.fetch_add(1, std::memory_order_relaxed);
        sample = 0.0F;
        return false;
    }
    sample = ring.samples_[read % ring.samples_.size()];
    ring.read_.store(read + 1, std::memory_order_release);
    return true;
}

void ringReset(SharedRing &ring) noexcept
{
    ring.read_.store(ring.write_.load(std::memory_order_acquire), std::memory_order_release);
}

std::string deviceIdentity(std::string_view owner, IoKind kind, std::string_view product,
                           std::uint32_t channels, std::uint32_t sample_rate)
{
    std::string value(owner);
    value.push_back('\0');
    value += std::to_string(static_cast<unsigned>(kind));
    value.push_back('\0');
    value.append(product);
    value.push_back('\0');
    value += std::to_string(channels);
    value.push_back('\0');
    value += std::to_string(sample_rate);
    return value;
}

std::uint64_t usbIdentityHash(std::string_view identity) noexcept
{
    // Bump this schema tag whenever the USB descriptors change incompatibly. Windows caches
    // endpoint names and formats by USB identity, so reusing an old identity can leave a valid
    // descriptor paired with a stale, unusable audio-engine format.
    constexpr std::string_view schema = "avc-uac1-v2";
    std::uint64_t hash = 14695981039346656037ULL;
    const auto add = [&](std::string_view value) {
        for (const unsigned char byte : value) {
            hash ^= byte;
            hash *= 1099511628211ULL;
        }
    };
    add(schema);
    hash ^= 0;
    hash *= 1099511628211ULL;
    add(identity);
    return hash;
}

std::uint16_t usbProductId(std::string_view identity) noexcept
{
    std::uint16_t product =
        static_cast<std::uint16_t>(0x8000U | (usbIdentityHash(identity) & 0x7fffU));
    if (product == 0xffffU) product = 0xfffeU;
    // 0xca00..0xcaff was the old bus-number-based range. Never reuse it, because Windows may
    // still have an endpoint with a stale format cached under one of those identities.
    if ((product & 0xff00U) == 0xca00U) product ^= 0x0100U;
    return product;
}

std::string usbSerial(std::string_view identity)
{
    static constexpr char digits[] = "0123456789abcdef";
    const std::uint64_t hash = usbIdentityHash(identity);
    std::string serial = "AVC-UAC1-";
    for (int shift = 60; shift >= 0; shift -= 4)
        serial.push_back(digits[(hash >> shift) & 0x0fU]);
    return serial;
}

std::uint32_t highSpeedPayload(std::uint32_t channels, std::uint32_t sample_rate) noexcept
{
    const std::uint64_t frames_per_millisecond = (sample_rate + 999U) / 1000U;
    const std::uint64_t bytes = frames_per_millisecond * channels * sizeof(std::int16_t);
    return bytes <= 3072 ? static_cast<std::uint32_t>(bytes) : 0;
}

std::uint16_t highSpeedPacketSize(std::uint32_t payload) noexcept
{
    const std::uint32_t transactions = (payload + 1023U) / 1024U;
    const std::uint32_t bytes_per_transaction = (payload + transactions - 1) / transactions;
    return static_cast<std::uint16_t>(bytes_per_transaction | ((transactions - 1) << 11));
}

std::wstring sharedMappingName(std::string_view identity)
{
    const std::uint64_t hash = usbIdentityHash(identity);
    static constexpr wchar_t digits[] = L"0123456789abcdef";
    std::wstring name = L"Local\\avc-usbip-";
    for (int shift = 60; shift >= 0; shift -= 4)
        name.push_back(digits[(hash >> shift) & 0x0fU]);
    return name;
}

struct SetupPacket {
    Byte request_type = 0;
    Byte request = 0;
    std::uint16_t value = 0;
    std::uint16_t index = 0;
    std::uint16_t length = 0;
};

std::vector<Byte> truncate(std::vector<Byte> data, std::uint16_t length)
{
    if (data.size() > length) data.resize(length);
    return data;
}

bool descriptorListIsWellFormed(std::span<const Byte> descriptors) noexcept
{
    std::size_t offset = 0;
    while (offset < descriptors.size()) {
        const std::size_t length = descriptors[offset];
        if (length < 2 || length > descriptors.size() - offset) return false;
        offset += length;
    }
    return offset == descriptors.size();
}

std::vector<Byte> rate24(std::uint32_t rate)
{
    return {static_cast<Byte>(rate), static_cast<Byte>(rate >> 8), static_cast<Byte>(rate >> 16)};
}

std::vector<Byte> int16Le(std::int16_t value)
{
    return {static_cast<Byte>(value), static_cast<Byte>(static_cast<std::uint16_t>(value) >> 8)};
}

}

struct UsbIpAudioDevice::Impl {
    int number = 0;
    std::string bus_id;
    std::string owner;
    std::string product;
    std::string identity;
    IoKind kind = IoKind::VirtualMic;
    std::uint32_t channel_count = 1;
    std::uint32_t sample_rate = kDefaultSampleRate;
    std::vector<Byte> device_descriptor;
    std::vector<Byte> config_descriptor;
    std::map<Byte, std::vector<Byte>> strings;
    HANDLE mapping = nullptr;
    SharedAudio *shared = nullptr;
    bool attached = false;

    ~Impl()
    {
        if (shared != nullptr) UnmapViewOfFile(shared);
        if (mapping != nullptr) CloseHandle(mapping);
    }

    void resetRings() noexcept
    {
        if (shared == nullptr) return;
        for (std::uint32_t channel = 0; channel < channel_count; ++channel) {
            ringReset(shared->playback[channel]);
            ringReset(shared->capture[channel]);
        }
    }

    bool playbackActive() const noexcept
    {
        return shared != nullptr && shared->configuration.load(std::memory_order_acquire) == 1 &&
               shared->playback_alt.load(std::memory_order_acquire) == 1;
    }

    bool captureActive() const noexcept
    {
        return shared != nullptr && shared->configuration.load(std::memory_order_acquire) == 1 &&
               shared->capture_alt.load(std::memory_order_acquire) == 1;
    }

    std::pair<std::vector<Byte>, std::int32_t> control(const SetupPacket &setup,
                                                       std::span<const Byte> out)
    {
        const bool standard = (setup.request_type & 0x60U) == 0;
        if (standard) {
            switch (setup.request) {
            case 0x06: { // GET_DESCRIPTOR
                const Byte type = static_cast<Byte>(setup.value >> 8);
                const Byte index = static_cast<Byte>(setup.value);
                if (type == 0x01 && index == 0)
                    return {truncate(device_descriptor, setup.length), kStatusOk};
                if (type == 0x02 && index == 0)
                    return {truncate(config_descriptor, setup.length), kStatusOk};
                if (type == 0x03) {
                    const auto found = strings.find(index);
                    if (found != strings.end())
                        return {truncate(found->second, setup.length), kStatusOk};
                }
                return {{}, kStatusPipe};
            }
            case 0x05: // SET_ADDRESS
                return {{}, kStatusOk};
            case 0x09: { // SET_CONFIGURATION
                const Byte next = static_cast<Byte>(setup.value);
                if (next > 1) return {{}, kStatusPipe};
                shared->configuration.store(next, std::memory_order_release);
                shared->playback_alt.store(0, std::memory_order_release);
                shared->capture_alt.store(0, std::memory_order_release);
                resetRings();
                return {{}, kStatusOk};
            }
            case 0x08: // GET_CONFIGURATION
                return {truncate({shared->configuration.load(std::memory_order_acquire)}, setup.length),
                        kStatusOk};
            case 0x0b: { // SET_INTERFACE
                const Byte interface_number = static_cast<Byte>(setup.index);
                const Byte alt = static_cast<Byte>(setup.value);
                if (shared->configuration.load(std::memory_order_acquire) != 1 || interface_number > 2 ||
                    alt > 1 || (interface_number == 0 && alt != 0)) {
                    return {{}, kStatusPipe};
                }
                if (interface_number == 1)
                    shared->playback_alt.store(alt, std::memory_order_release);
                if (interface_number == 2)
                    shared->capture_alt.store(alt, std::memory_order_release);
                if (alt == 0 && interface_number != 0) resetRings();
                return {{}, kStatusOk};
            }
            case 0x0a: { // GET_INTERFACE
                const Byte interface_number = static_cast<Byte>(setup.index);
                if (interface_number == 0) return {truncate({0}, setup.length), kStatusOk};
                if (interface_number == 1)
                    return {truncate({shared->playback_alt.load(std::memory_order_acquire)}, setup.length),
                            kStatusOk};
                if (interface_number == 2)
                    return {truncate({shared->capture_alt.load(std::memory_order_acquire)}, setup.length),
                            kStatusOk};
                return {{}, kStatusPipe};
            }
            case 0x00: // GET_STATUS
            case 0x0c: // SYNCH_FRAME
                return {truncate({0, 0}, setup.length), kStatusOk};
            case 0x01: // CLEAR_FEATURE
            case 0x03: // SET_FEATURE
                return {{}, kStatusOk};
            default:
                return {{}, kStatusPipe};
            }
        }

        const Byte recipient = setup.request_type & 0x1fU;
        const Byte selector = static_cast<Byte>(setup.value >> 8);
        const Byte endpoint = static_cast<Byte>(setup.index);
        if (recipient == 0x02 && selector == 0x01 && (endpoint == 0x01 || endpoint == 0x82)) {
            switch (setup.request) {
            case 0x01: // SET_CUR
                if (out.size() >= 3) {
                    const std::uint32_t rate = static_cast<std::uint32_t>(out[0]) |
                                               (static_cast<std::uint32_t>(out[1]) << 8) |
                                               (static_cast<std::uint32_t>(out[2]) << 16);
                    if (rate != sample_rate) return {{}, kStatusPipe};
                    shared->sample_rate.store(rate, std::memory_order_release);
                }
                return {{}, kStatusOk};
            case 0x81: // GET_CUR
                return {truncate(rate24(shared->sample_rate.load(std::memory_order_acquire)), setup.length),
                        kStatusOk};
            case 0x82: // GET_MIN
            case 0x83: // GET_MAX
                return {truncate(rate24(sample_rate), setup.length), kStatusOk};
            case 0x84: // GET_RES
                return {truncate(rate24(1), setup.length), kStatusOk};
            default:
                break;
            }
        }

        const Byte entity = static_cast<Byte>(setup.index >> 8);
        const Byte interface_number = static_cast<Byte>(setup.index);
        if (recipient == 0x01 && interface_number == 0 && (entity == 2 || entity == 5)) {
            const Byte control_selector = static_cast<Byte>(setup.value >> 8);
            if (control_selector == 0x01) { // MUTE_CONTROL
                if (setup.request == 0x01) {
                    if (!out.empty()) shared->mute.store(out[0] != 0, std::memory_order_release);
                    return {{}, kStatusOk};
                }
                if (setup.request == 0x81)
                    return {truncate({static_cast<Byte>(shared->mute.load(std::memory_order_acquire))},
                                     setup.length),
                            kStatusOk};
            }
            if (control_selector == 0x02) { // VOLUME_CONTROL, signed 1/256 dB.
                if (setup.request == 0x01) {
                    if (out.size() >= 2)
                        shared->volume.store(static_cast<std::int16_t>(readLe16(out.data())),
                                             std::memory_order_release);
                    return {{}, kStatusOk};
                }
                if (setup.request == 0x81)
                    return {truncate(int16Le(shared->volume.load(std::memory_order_acquire)), setup.length),
                            kStatusOk};
                if (setup.request == 0x82)
                    return {truncate(int16Le(-60 * 256), setup.length), kStatusOk};
                if (setup.request == 0x83) return {truncate(int16Le(0), setup.length), kStatusOk};
                if (setup.request == 0x84) return {truncate(int16Le(256), setup.length), kStatusOk};
            }
        }
        return {{}, kStatusPipe};
    }

    void writePlayback(std::span<const Byte> data) noexcept
    {
        const std::size_t frame_bytes = static_cast<std::size_t>(channel_count) * 2;
        if (frame_bytes == 0) return;
        const std::size_t frames = data.size() / frame_bytes;
        for (std::size_t frame = 0; frame < frames; ++frame) {
            for (std::uint32_t channel = 0; channel < channel_count; ++channel) {
                const Byte *sample = data.data() + frame * frame_bytes + channel * 2;
                const auto pcm = static_cast<std::int16_t>(readLe16(sample));
                (void)ringPush(shared->playback[channel], static_cast<float>(pcm) / 32768.0F);
            }
        }
    }

    void readCapture(std::span<Byte> data) noexcept
    {
        std::fill(data.begin(), data.end(), Byte{0});
        const std::size_t frame_bytes = static_cast<std::size_t>(channel_count) * 2;
        if (frame_bytes == 0) return;
        const std::size_t frames = data.size() / frame_bytes;
        for (std::size_t frame = 0; frame < frames; ++frame) {
            for (std::uint32_t channel = 0; channel < channel_count; ++channel) {
                float value = 0.0F;
                (void)ringPop(shared->capture[channel], value);
                const float limited = std::clamp(value, -1.0F, 1.0F);
                const auto pcm = static_cast<std::int16_t>(
                    std::lround(limited < 0.0F ? limited * 32768.0F : limited * 32767.0F));
                Byte *sample = data.data() + frame * frame_bytes + channel * 2;
                putLe16(sample, static_cast<std::uint16_t>(pcm));
            }
        }
    }
};

namespace {

std::shared_ptr<UsbIpAudioDevice> makeDevice(int number, std::string_view owner,
                                             std::string_view product, IoKind kind,
                                             std::uint32_t channels, std::uint32_t sample_rate,
                                             bool publisher,
                                             std::string &error)
{
    auto impl = std::make_unique<UsbIpAudioDevice::Impl>();
    impl->number = number;
    impl->bus_id = "1-" + std::to_string(number);
    impl->owner = owner;
    impl->product = product;
    impl->identity = deviceIdentity(owner, kind, product, channels, sample_rate);
    impl->kind = kind;
    impl->channel_count = channels;
    impl->sample_rate = sample_rate;

    const std::wstring mapping_name = sharedMappingName(impl->identity);
    bool initialize = false;
    if (publisher) {
        impl->mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                           static_cast<DWORD>(sizeof(SharedAudio)),
                                           mapping_name.c_str());
        initialize = impl->mapping != nullptr && GetLastError() != ERROR_ALREADY_EXISTS;
    } else {
        impl->mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, mapping_name.c_str());
    }
    if (impl->mapping == nullptr) {
        error = publisher ? "cannot create the USB/IP shared audio mapping"
                          : "the daemon has not published the USB/IP shared audio mapping";
        return {};
    }
    impl->shared = static_cast<SharedAudio *>(
        MapViewOfFile(impl->mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedAudio)));
    if (impl->shared == nullptr) {
        error = "cannot map the USB/IP shared audio buffer";
        return {};
    }
    if (initialize) {
        new (impl->shared) SharedAudio();
        impl->shared->channels = channels;
        impl->shared->sample_rate.store(sample_rate, std::memory_order_relaxed);
    } else if (impl->shared->magic != 0x41564355U || impl->shared->version != 1 ||
               impl->shared->channels != channels
               || impl->shared->sample_rate.load(std::memory_order_relaxed) != sample_rate) {
        error = "the USB/IP shared audio mapping is incompatible";
        return {};
    }

    impl->device_descriptor.assign(18, 0);
    auto &device = impl->device_descriptor;
    device[0] = 18;
    device[1] = 0x01;
    putLe16(device.data() + 2, 0x0200); // USB 2.0 high-speed.
    device[7] = 64;
    putLe16(device.data() + 8, 0xffff); // Experimental local VID.
    putLe16(device.data() + 10, usbProductId(impl->identity));
    putLe16(device.data() + 12, 0x0001);
    device[14] = 1;
    device[15] = 2;
    device[16] = 3;
    device[17] = 1;

    auto &config = impl->config_descriptor;
    const auto add = [&](std::initializer_list<Byte> bytes) {
        config.insert(config.end(), bytes.begin(), bytes.end());
    };
    const Byte usb_channels = static_cast<Byte>(channels);
    const std::uint16_t channel_config = usb_channels == 2 ? 0x0003 : 0x0000;
    const std::uint16_t max_packet = highSpeedPacketSize(
        highSpeedPayload(channels, sample_rate));
    const std::vector<Byte> usb_rate = rate24(sample_rate);
    const auto addFeatureUnit = [&](Byte unit, Byte source) {
        // UAC1 feature units carry one bmaControls byte for the master channel
        // and one for every logical channel, followed by iFeature.
        const Byte length = static_cast<Byte>(7U + usb_channels + 1U);
        add({length, 0x24, 0x06, unit, source, 1, 0x03});
        for (Byte channel = 0; channel < usb_channels; ++channel) config.push_back(0);
        config.push_back(0);
    };

    add({9, 0x02, 0, 0, 3, 1, 0, 0x80, 50});
    add({9, 0x04, 0, 0, 0, 0x01, 0x01, 0x00, 0});
    const std::size_t audio_control_start = config.size();
    add({10, 0x24, 0x01, 0x00, 0x01, 0, 0, 2, 1, 2});
    add({12, 0x24, 0x02, 1, 0x01, 0x01, 0, usb_channels, static_cast<Byte>(channel_config),
         static_cast<Byte>(channel_config >> 8), 0, 0});
    addFeatureUnit(2, 1);
    add({9, 0x24, 0x03, 3, 0x01, 0x03, 0, 2, 0});
    add({12, 0x24, 0x02, 4, 0x01, 0x02, 0, usb_channels, static_cast<Byte>(channel_config),
         static_cast<Byte>(channel_config >> 8), 0, 0});
    addFeatureUnit(5, 4);
    add({9, 0x24, 0x03, 6, 0x01, 0x01, 0, 5, 0});
    putLe16(config.data() + audio_control_start + 5,
            static_cast<std::uint16_t>(config.size() - audio_control_start));
    add({9, 0x04, 1, 0, 0, 0x01, 0x02, 0x00, 0});
    add({9, 0x04, 1, 1, 1, 0x01, 0x02, 0x00, 0});
    add({7, 0x24, 0x01, 1, 1, 0x01, 0x00});
    add({11, 0x24, 0x02, 1, usb_channels, 2, 16, 1, usb_rate[0], usb_rate[1], usb_rate[2]});
    add({9, 0x05, 0x01, 0x09, static_cast<Byte>(max_packet), static_cast<Byte>(max_packet >> 8), 4,
         0, 0});
    add({7, 0x25, 0x01, 0, 0, 0, 0});
    add({9, 0x04, 2, 0, 0, 0x01, 0x02, 0x00, 0});
    add({9, 0x04, 2, 1, 1, 0x01, 0x02, 0x00, 0});
    add({7, 0x24, 0x01, 6, 1, 0x01, 0x00});
    add({11, 0x24, 0x02, 1, usb_channels, 2, 16, 1, usb_rate[0], usb_rate[1], usb_rate[2]});
    add({9, 0x05, 0x82, 0x0d, static_cast<Byte>(max_packet), static_cast<Byte>(max_packet >> 8), 4,
         0, 0});
    add({7, 0x25, 0x01, 0, 0, 0, 0});
    putLe16(config.data() + 2, static_cast<std::uint16_t>(config.size()));
    if (!descriptorListIsWellFormed(config)) {
        error = "internal USB audio descriptor layout is invalid";
        return {};
    }

    impl->strings[0] = {4, 0x03, 0x09, 0x04};
    impl->strings[1] = stringDescriptor("AVC Project");
    impl->strings[2] = stringDescriptor(product);
    impl->strings[3] = stringDescriptor(usbSerial(impl->identity));
    return std::shared_ptr<UsbIpAudioDevice>(new UsbIpAudioDevice(std::move(impl)));
}

}

UsbIpAudioDevice::UsbIpAudioDevice(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
UsbIpAudioDevice::~UsbIpAudioDevice() = default;

void UsbIpAudioDevice::pullPlayback(std::uint32_t channel, float *output,
                                    std::size_t frames) noexcept
{
    if (output == nullptr || channel >= impl_->channel_count) return;
    for (std::size_t i = 0; i < frames; ++i)
        (void)ringPop(impl_->shared->playback[channel], output[i]);
}

void UsbIpAudioDevice::pushCapture(std::uint32_t channel, const float *input,
                                   std::size_t frames) noexcept
{
    if (input == nullptr || channel >= impl_->channel_count || !impl_->captureActive()) return;
    for (std::size_t i = 0; i < frames; ++i)
        (void)ringPush(impl_->shared->capture[channel], input[i]);
}

std::uint32_t UsbIpAudioDevice::channels() const noexcept
{
    return impl_->channel_count;
}

namespace {

struct BasicHeader {
    std::uint32_t command = 0;
    std::uint32_t sequence = 0;
    std::uint32_t device_id = 0;
    std::uint32_t direction = 0;
    std::uint32_t endpoint = 0;
};

struct IsoPacket {
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
    std::uint32_t actual = 0;
    std::int32_t status = 0;
};

struct SubmitRequest {
    BasicHeader basic;
    std::uint32_t transfer_flags = 0;
    std::uint32_t transfer_length = 0;
    std::uint32_t start_frame = 0;
    std::uint32_t packet_count = 0;
    std::uint32_t interval = 0;
    std::array<Byte, 8> setup{};
    std::vector<Byte> out;
    std::vector<IsoPacket> packets;

    bool isIsochronous() const noexcept
    {
        return packet_count != 0 && packet_count != kNoIsoPackets;
    }
};

struct IsoTimeline {
    Clock::time_point origin{};
    std::array<Clock::time_point, 16> next{};
    std::array<bool, 16> described{};
    HANDLE timer = nullptr;

    IsoTimeline()
    {
        timer = CreateWaitableTimerExW(nullptr, nullptr,
                                       CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                       TIMER_MODIFY_STATE | SYNCHRONIZE);
        if (timer == nullptr) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }

    ~IsoTimeline()
    {
        if (timer != nullptr) CloseHandle(timer);
    }

    IsoTimeline(const IsoTimeline &) = delete;
    IsoTimeline &operator=(const IsoTimeline &) = delete;

    void waitUntil(Clock::time_point deadline) const
    {
        for (;;) {
            const auto now = Clock::now();
            if (now >= deadline) return;
            if (timer == nullptr) {
                std::this_thread::sleep_until(deadline);
                return;
            }

            const auto remaining =
                std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now).count();
            LARGE_INTEGER due{};
            due.QuadPart = -std::max<LONGLONG>(1, (remaining + 99) / 100);
            if (!SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0)
                || WaitForSingleObject(timer, INFINITE) != WAIT_OBJECT_0) {
                std::this_thread::sleep_until(deadline);
                return;
            }
        }
    }
};

struct IsoEndpointQueue {
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<SubmitRequest> requests;
    std::thread thread;
    IsoTimeline timeline;
    bool stopping = false;
};

bool readBasic(SOCKET socket, BasicHeader &header)
{
    std::array<Byte, 20> raw{};
    if (!recvAll(socket, raw.data(), raw.size())) return false;
    header.command = readBe32(raw.data());
    header.sequence = readBe32(raw.data() + 4);
    header.device_id = readBe32(raw.data() + 8);
    header.direction = readBe32(raw.data() + 12);
    header.endpoint = readBe32(raw.data() + 16);
    return true;
}

bool readSubmit(SOCKET socket, const BasicHeader &basic, SubmitRequest &request)
{
    request.basic = basic;
    std::array<Byte, 28> body{};
    if (!recvAll(socket, body.data(), body.size())) return false;
    request.transfer_flags = readBe32(body.data());
    request.transfer_length = readBe32(body.data() + 4);
    request.start_frame = readBe32(body.data() + 8);
    request.packet_count = readBe32(body.data() + 12);
    request.interval = readBe32(body.data() + 16);
    std::copy_n(body.data() + 20, 8, request.setup.data());
    if (request.transfer_length > kMaxTransferLength) return false;

    if (basic.direction == kDirectionOut && request.transfer_length > 0) {
        request.out.resize(request.transfer_length);
        if (!recvAll(socket, request.out.data(), request.out.size())) return false;
    }
    if (request.isIsochronous()) {
        if (request.packet_count > 4096) return false;
        request.packets.resize(request.packet_count);
        std::array<Byte, 16> raw{};
        for (IsoPacket &packet : request.packets) {
            if (!recvAll(socket, raw.data(), raw.size())) return false;
            packet.offset = readBe32(raw.data());
            packet.length = readBe32(raw.data() + 4);
            packet.actual = readBe32(raw.data() + 8);
            packet.status = static_cast<std::int32_t>(readBe32(raw.data() + 12));
        }
    }
    return true;
}

bool replySubmit(SOCKET socket, const SubmitRequest &request, std::int32_t status,
                 std::uint32_t actual, std::span<const Byte> data,
                 std::span<const IsoPacket> packets, std::uint32_t error_count)
{
    if (status != kStatusOk) {
        actual = 0;
        data = {};
    }
    std::vector<Byte> frame;
    frame.reserve(48 + data.size() + packets.size() * 16);
    appendBe32(frame, kRetSubmit);
    appendBe32(frame, request.basic.sequence);
    appendBe32(frame, 0);
    appendBe32(frame, 0);
    appendBe32(frame, 0);
    appendBeI32(frame, status);
    appendBe32(frame, actual);
    appendBe32(frame, request.isIsochronous() ? request.start_frame : 0);
    appendBe32(frame, request.isIsochronous() ? static_cast<std::uint32_t>(packets.size())
                                              : kNoIsoPackets);
    appendBe32(frame, error_count);
    appendZeros(frame, 8);
    frame.insert(frame.end(), data.begin(), data.end());
    for (const IsoPacket &packet : packets) {
        appendBe32(frame, packet.offset);
        appendBe32(frame, packet.length);
        appendBe32(frame, packet.actual);
        appendBeI32(frame, packet.status);
    }
    return sendAll(socket, frame);
}

bool replyUnlink(SOCKET socket, const BasicHeader &request, std::int32_t status)
{
    std::vector<Byte> frame;
    frame.reserve(48);
    appendBe32(frame, kRetUnlink);
    appendBe32(frame, request.sequence);
    appendZeros(frame, 12);
    appendBeI32(frame, status);
    appendZeros(frame, 24);
    return sendAll(socket, frame);
}

bool replySubmitLocked(SOCKET socket, std::mutex &send_mutex,
                       const SubmitRequest &request, std::int32_t status,
                       std::uint32_t actual, std::span<const Byte> data,
                       std::span<const IsoPacket> packets, std::uint32_t error_count)
{
    const std::lock_guard<std::mutex> lock(send_mutex);
    return replySubmit(socket, request, status, actual, data, packets, error_count);
}

bool replyUnlinkLocked(SOCKET socket, std::mutex &send_mutex,
                       const BasicHeader &request, std::int32_t status)
{
    const std::lock_guard<std::mutex> lock(send_mutex);
    return replyUnlink(socket, request, status);
}

void markPackets(std::vector<IsoPacket> &packets, std::size_t actual_total,
                 std::int32_t status = kStatusOk)
{
    std::size_t remaining = actual_total;
    for (IsoPacket &packet : packets) {
        packet.status = status;
        if (status != kStatusOk) {
            packet.actual = 0;
            continue;
        }
        const std::size_t used = std::min<std::size_t>(packet.length, remaining);
        packet.actual = static_cast<std::uint32_t>(used);
        remaining -= used;
    }
}

std::size_t responseLength(const SubmitRequest &request)
{
    if (request.packets.empty()) return request.transfer_length;
    std::uint64_t total = 0;
    for (const IsoPacket &packet : request.packets)
        total += packet.length;
    if (request.transfer_length > 0)
        total = std::min<std::uint64_t>(total, request.transfer_length);
    return static_cast<std::size_t>(std::min<std::uint64_t>(total, kMaxTransferLength));
}

SetupPacket parseSetup(const std::array<Byte, 8> &raw) noexcept
{
    return {raw[0], raw[1], readLe16(raw.data() + 2), readLe16(raw.data() + 4),
            readLe16(raw.data() + 6)};
}

std::wstring environmentPath(const wchar_t *name)
{
    const DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
    if (needed == 0) return {};
    std::wstring value(needed, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), needed);
    if (written == 0 || written >= needed) return {};
    value.resize(written);
    return value;
}

std::wstring findUsbIpExecutable()
{
    std::array<wchar_t, 32768> found{};
    const DWORD size = SearchPathW(nullptr, L"usbip.exe", nullptr, static_cast<DWORD>(found.size()),
                                   found.data(), nullptr);
    if (size > 0 && size < found.size()) return std::wstring(found.data(), size);

    std::vector<std::filesystem::path> candidates;
    const std::wstring program_files = environmentPath(L"ProgramFiles");
    const std::wstring program_files_x86 = environmentPath(L"ProgramFiles(x86)");
    for (const std::wstring &root : {program_files, program_files_x86}) {
        if (root.empty()) continue;
        candidates.emplace_back(std::filesystem::path(root) / L"USBip" / L"usbip.exe");
        candidates.emplace_back(std::filesystem::path(root) / L"usbip-win2" / L"usbip.exe");
        candidates.emplace_back(std::filesystem::path(root) / L"usbip" / L"usbip.exe");
    }
    std::error_code ec;
    for (const auto &candidate : candidates) {
        if (std::filesystem::is_regular_file(candidate, ec)) return candidate.wstring();
        ec.clear();
    }
    return {};
}

std::wstring currentExecutable()
{
    std::wstring path(32768, L'\0');
    const DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (size == 0 || size >= path.size()) return {};
    path.resize(size);
    return path;
}

std::filesystem::path bundledUsbIpInstaller()
{
#ifdef AVC_USBIP_INSTALLER_NAME
    const std::wstring executable = currentExecutable();
    if (executable.empty()) return {};
    return std::filesystem::path(executable).parent_path() / L"drivers" /
           std::filesystem::path(kBundledInstallerName);
#else
    return {};
#endif
}

bool sha256File(const std::filesystem::path &path, std::array<Byte, 32> &digest)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    HANDLE file = INVALID_HANDLE_VALUE;
    std::vector<Byte> object;

    const auto close = [&] {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        if (hash != nullptr) BCryptDestroyHash(hash);
        if (algorithm != nullptr) BCryptCloseAlgorithmProvider(algorithm, 0);
    };

    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
        close();
        return false;
    }
    DWORD object_size = 0;
    DWORD copied = 0;
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &copied,
                          0) < 0 ||
        object_size == 0) {
        close();
        return false;
    }
    object.resize(object_size);
    if (BCryptCreateHash(algorithm, &hash, object.data(), static_cast<ULONG>(object.size()),
                         nullptr, 0, 0) < 0) {
        close();
        return false;
    }

    file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        close();
        return false;
    }
    std::array<Byte, 64U * 1024U> buffer{};
    for (;;) {
        DWORD count = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr)) {
            close();
            return false;
        }
        if (count == 0) break;
        if (BCryptHashData(hash, buffer.data(), count, 0) < 0) {
            close();
            return false;
        }
    }
    const bool ok = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
    close();
    return ok;
}

int hexDigit(char ch) noexcept
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return 10 + ch - 'a';
    if (ch >= 'A' && ch <= 'F') return 10 + ch - 'A';
    return -1;
}

bool bundledInstallerMatches(const std::filesystem::path &path)
{
#ifdef AVC_USBIP_INSTALLER_NAME
    if (kBundledInstallerSha256.size() != 64) return false;
    std::array<Byte, 32> digest{};
    if (!sha256File(path, digest)) return false;
    for (std::size_t i = 0; i < digest.size(); ++i) {
        const int high = hexDigit(kBundledInstallerSha256[i * 2]);
        const int low = hexDigit(kBundledInstallerSha256[i * 2 + 1]);
        if (high < 0 || low < 0 || digest[i] != static_cast<Byte>((high << 4) | low)) {
            return false;
        }
    }
    return true;
#else
    (void)path;
    return false;
#endif
}

bool launchElevated(std::string_view operation, std::span<const int> buses, std::string &error)
{
    const std::wstring executable = currentExecutable();
    if (executable.empty()) {
        error = "cannot locate the AVC executable for USB/IP elevation";
        return false;
    }
    std::wstring bus_list;
    for (const int number : buses) {
        if (!bus_list.empty()) bus_list.push_back(L',');
        bus_list += std::to_wstring(number);
    }
    const std::wstring wide_operation(operation.begin(), operation.end());
    const std::wstring parameters = L"--usbip-operation=" + wide_operation +
                                    L" --usbip-buses=" + bus_list;
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"runas";
    info.lpFile = executable.c_str();
    info.lpParameters = parameters.c_str();
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info)) {
        const DWORD code = GetLastError();
        error = code == ERROR_CANCELLED ? "USB/IP device update was cancelled "
                                          "at the administrator prompt"
                                        : "cannot start the elevated USB/IP device "
                                          "helper (Windows error " +
                                              std::to_string(code) + ")";
        return false;
    }
    const DWORD wait = WaitForSingleObject(info.hProcess, 60000);
    DWORD exit_code = 1;
    if (wait != WAIT_OBJECT_0 || !GetExitCodeProcess(info.hProcess, &exit_code)) {
        CloseHandle(info.hProcess);
        error = "the elevated USB/IP device helper did not finish";
        return false;
    }
    CloseHandle(info.hProcess);
    if (exit_code != 0) {
        error = "usbip-win2 could not update the AVC audio device(s); run AVC as "
                "administrator "
                "and check `usbip.exe port` (helper exit " +
                std::to_string(exit_code) + ")";
        return false;
    }
    return true;
}

}

struct UsbIpAudioManager::Impl {
    explicit Impl(bool publish) : publisher(publish) {}

    bool publisher = false;
    bool synchronized = false;
    std::uint32_t sample_rate = kDefaultSampleRate;
    mutable std::mutex control_mutex;
    mutable std::mutex devices_mutex;
    std::vector<std::shared_ptr<UsbIpAudioDevice>> devices;
    std::atomic<bool> stopping{false};
    bool winsock_open = false;
    SOCKET listener = INVALID_SOCKET;
    std::thread accept_thread;
    std::mutex clients_mutex;
    std::vector<SOCKET> clients;
    std::vector<std::thread> connection_threads;

    std::vector<std::shared_ptr<UsbIpAudioDevice>> deviceSnapshot() const
    {
        const std::lock_guard<std::mutex> lock(devices_mutex);
        return devices;
    }

    std::shared_ptr<UsbIpAudioDevice> findBus(std::string_view bus_id) const
    {
        const std::lock_guard<std::mutex> lock(devices_mutex);
        for (const auto &device : devices) {
            if (device->impl_->bus_id == bus_id) return device;
        }
        return {};
    }

    bool startServer(std::string &error)
    {
        if (listener != INVALID_SOCKET) return true;
        WSADATA data{};
        const int startup = WSAStartup(MAKEWORD(2, 2), &data);
        if (startup != 0) {
            error = "cannot initialize Winsock for USB/IP (error " + std::to_string(startup) + ")";
            return false;
        }
        winsock_open = true;

        listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == INVALID_SOCKET) {
            error = "cannot create the USB/IP listener (Winsock error " +
                    std::to_string(WSAGetLastError()) + ")";
            stopServer();
            return false;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(3240);
        if (bind(listener, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) ==
                SOCKET_ERROR ||
            listen(listener, SOMAXCONN) == SOCKET_ERROR) {
            error = "cannot listen on 127.0.0.1:3240 for USB/IP; another USB/IP "
                    "server may "
                    "already be running (Winsock error " +
                    std::to_string(WSAGetLastError()) + ")";
            stopServer();
            return false;
        }
        stopping.store(false, std::memory_order_release);
        accept_thread = std::thread([this] { acceptLoop(); });
        spdlog::info("USB/IP audio server listening on 127.0.0.1:3240");
        return true;
    }

    void stopServer() noexcept
    {
        stopping.store(true, std::memory_order_release);
        const SOCKET old_listener = std::exchange(listener, INVALID_SOCKET);
        if (old_listener != INVALID_SOCKET) {
            shutdown(old_listener, SD_BOTH);
            closesocket(old_listener);
        }
        if (accept_thread.joinable()) accept_thread.join();

        {
            const std::lock_guard<std::mutex> lock(clients_mutex);
            for (SOCKET client : clients)
                shutdown(client, SD_BOTH);
        }
        for (std::thread &thread : connection_threads) {
            if (thread.joinable()) thread.join();
        }
        connection_threads.clear();
        {
            const std::lock_guard<std::mutex> lock(clients_mutex);
            clients.clear();
        }
        if (winsock_open) {
            WSACleanup();
            winsock_open = false;
        }
    }

    void acceptLoop()
    {
        while (!stopping.load(std::memory_order_acquire)) {
            sockaddr_in remote{};
            int remote_size = sizeof(remote);
            const SOCKET client =
                accept(listener, reinterpret_cast<sockaddr *>(&remote), &remote_size);
            if (client == INVALID_SOCKET) {
                if (!stopping.load(std::memory_order_acquire))
                    spdlog::warn("USB/IP accept failed: {}", WSAGetLastError());
                return;
            }
            BOOL enabled = TRUE;
            (void)setsockopt(client, IPPROTO_TCP, TCP_NODELAY,
                             reinterpret_cast<const char *>(&enabled), sizeof(enabled));
            {
                const std::lock_guard<std::mutex> lock(clients_mutex);
                clients.push_back(client);
                connection_threads.emplace_back([this, client] { handleConnection(client); });
            }
        }
    }

    void removeClient(SOCKET client)
    {
        const std::lock_guard<std::mutex> lock(clients_mutex);
        const auto found = std::find(clients.begin(), clients.end(), client);
        if (found != clients.end()) clients.erase(found);
    }

    bool writeOpHeader(SOCKET client, std::uint16_t code, std::uint32_t status)
    {
        std::vector<Byte> out;
        appendBe16(out, kUsbIpVersion);
        appendBe16(out, code);
        appendBe32(out, status);
        return sendAll(client, out);
    }

    bool writeUsbDevice(SOCKET client, const std::shared_ptr<UsbIpAudioDevice> &device)
    {
        const auto &impl = *device->impl_;
        std::vector<Byte> out;
        out.reserve(312);
        appendFixed(out, "/sys/devices/platform/avc/" + impl.bus_id, 256);
        appendFixed(out, impl.bus_id, 32);
        appendBe32(out, 1); // Bus number.
        appendBe32(out, static_cast<std::uint32_t>(impl.number));
        appendBe32(out, 3); // USB_SPEED_HIGH.
        appendBe16(out, readLe16(impl.device_descriptor.data() + 8));
        appendBe16(out, readLe16(impl.device_descriptor.data() + 10));
        appendBe16(out, readLe16(impl.device_descriptor.data() + 12));
        out.push_back(impl.device_descriptor[4]);
        out.push_back(impl.device_descriptor[5]);
        out.push_back(impl.device_descriptor[6]);
        out.push_back(1);
        out.push_back(impl.device_descriptor[17]);
        out.push_back(3);
        return sendAll(client, out);
    }

    bool writeDeviceList(SOCKET client)
    {
        const auto snapshot = deviceSnapshot();
        if (!writeOpHeader(client, kOpRepDevlist, 0)) return false;
        std::vector<Byte> count;
        appendBe32(count, static_cast<std::uint32_t>(snapshot.size()));
        if (!sendAll(client, count)) return false;
        constexpr std::array<Byte, 12> interfaces{0x01, 0x01, 0x00, 0,    0x01, 0x02,
                                                  0x00, 0,    0x01, 0x02, 0x00, 0};
        for (const auto &device : snapshot) {
            if (!writeUsbDevice(client, device) ||
                !sendAll(client, interfaces.data(), interfaces.size()))
                return false;
        }
        return true;
    }

    void handleConnection(SOCKET client)
    {
        const auto cleanup = [this, client] {
            shutdown(client, SD_BOTH);
            closesocket(client);
            removeClient(client);
        };

        std::array<Byte, 8> op{};
        if (!recvAll(client, op.data(), op.size()) || readBe16(op.data()) != kUsbIpVersion) {
            cleanup();
            return;
        }
        const std::uint16_t code = readBe16(op.data() + 2);
        if (code == kOpReqDevlist) {
            (void)writeDeviceList(client);
            cleanup();
            return;
        }
        if (code != kOpReqImport) {
            cleanup();
            return;
        }

        std::array<char, 32> raw_bus{};
        if (!recvAll(client, raw_bus.data(), raw_bus.size())) {
            cleanup();
            return;
        }
        const auto nul = std::find(raw_bus.begin(), raw_bus.end(), '\0');
        const std::string bus_id(raw_bus.begin(), nul);
        const auto device = findBus(bus_id);
        if (!device) {
            (void)writeOpHeader(client, kOpRepImport, 1);
            cleanup();
            return;
        }
        if (!writeOpHeader(client, kOpRepImport, 0) || !writeUsbDevice(client, device)) {
            cleanup();
            return;
        }
        spdlog::info("USB/IP imported {} as '{}'", bus_id, device->impl_->product);
        handleUrbs(client, device);
        spdlog::info("USB/IP session closed for {}", bus_id);
        cleanup();
    }

    void paceIso(const SubmitRequest &request, IsoTimeline &timeline)
    {
        if (!request.isIsochronous()) return;
        const auto duration = std::chrono::milliseconds(request.packet_count);
        const auto now = Clock::now();
        if (timeline.origin == Clock::time_point{}) timeline.origin = now;

        const std::size_t index = request.basic.endpoint % timeline.next.size();
        auto &deadline = timeline.next[index];
        if (deadline == Clock::time_point{}) deadline = timeline.origin;
        deadline += duration;

        // Keep one monotonic deadline per endpoint. A slightly late wake-up shortens the next
        // wait instead of shifting the whole stream; only a genuine long stall resets the
        // timeline so a backlog is not replayed as a burst.
        if (deadline + duration * 2 < now) deadline = now + duration;

        if (!timeline.described[index]) {
            timeline.described[index] = true;
            spdlog::debug(
                "USB/IP isoch endpoint {}: {} packet(s), interval {}, transfer {} bytes, "
                "frame {}",
                request.basic.endpoint, request.packet_count, request.interval,
                request.transfer_length, request.start_frame);
        }
        timeline.waitUntil(deadline);
    }

    bool processSubmit(SOCKET client, const std::shared_ptr<UsbIpAudioDevice> &device,
                       SubmitRequest &request, IsoTimeline &timeline,
                       std::mutex &send_mutex)
    {
        auto &impl = *device->impl_;
        if (request.basic.endpoint == 0) {
            const SetupPacket setup = parseSetup(request.setup);
            auto [data, status] = impl.control(setup, request.out);
            std::uint32_t actual = static_cast<std::uint32_t>(data.size());
            if (request.basic.direction == kDirectionOut && status == kStatusOk) {
                actual = static_cast<std::uint32_t>(request.out.size());
                data.clear();
            }
            return replySubmitLocked(client, send_mutex, request, status, actual, data, {}, 0);
        }

        if (request.basic.endpoint == 1 && request.basic.direction == kDirectionOut) {
            if (impl.playbackActive()) impl.writePlayback(request.out);
            markPackets(request.packets, request.out.size());
            paceIso(request, timeline);
            return replySubmitLocked(client, send_mutex, request, kStatusOk,
                                     static_cast<std::uint32_t>(request.out.size()), {},
                                     request.packets, 0);
        }
        if (request.basic.endpoint == 2 && request.basic.direction == kDirectionIn) {
            paceIso(request, timeline);
            std::vector<Byte> data(responseLength(request), 0);
            if (impl.captureActive()) impl.readCapture(data);
            markPackets(request.packets, data.size());
            return replySubmitLocked(client, send_mutex, request, kStatusOk,
                                     static_cast<std::uint32_t>(data.size()), data,
                                     request.packets, 0);
        }
        markPackets(request.packets, 0, kStatusPipe);
        return replySubmitLocked(client, send_mutex, request, kStatusPipe, 0, {},
                                 request.packets,
                                 static_cast<std::uint32_t>(request.packets.size()));
    }

    void handleUrbs(SOCKET client, const std::shared_ptr<UsbIpAudioDevice> &device)
    {
        constexpr std::size_t max_queued_iso_urbs = 256;
        std::mutex send_mutex;
        std::array<IsoEndpointQueue, 16> iso_queues;
        IsoTimeline control_timeline;
        std::atomic<bool> failed{false};

        const auto runEndpoint = [&](std::size_t index) {
            IsoEndpointQueue &queue = iso_queues[index];
            for (;;) {
                SubmitRequest request;
                {
                    std::unique_lock<std::mutex> lock(queue.mutex);
                    queue.ready.wait(lock, [&] { return queue.stopping || !queue.requests.empty(); });
                    if (queue.stopping) return;
                    request = std::move(queue.requests.front());
                    queue.requests.pop_front();
                }
                if (!processSubmit(client, device, request, queue.timeline, send_mutex)) {
                    failed.store(true, std::memory_order_release);
                    shutdown(client, SD_BOTH);
                    return;
                }
            }
        };

        while (!stopping.load(std::memory_order_acquire)
               && !failed.load(std::memory_order_acquire)) {
            BasicHeader basic;
            if (!readBasic(client, basic)) break;
            if (basic.command == kCmdSubmit) {
                SubmitRequest request;
                if (!readSubmit(client, basic, request)) break;
                if (request.isIsochronous() && request.basic.endpoint != 0) {
                    const std::size_t index = request.basic.endpoint % iso_queues.size();
                    IsoEndpointQueue &queue = iso_queues[index];
                    if (!queue.thread.joinable()) {
                        try {
                            queue.thread = std::thread(runEndpoint, index);
                        } catch (...) {
                            failed.store(true, std::memory_order_release);
                            break;
                        }
                    }
                    {
                        const std::lock_guard<std::mutex> lock(queue.mutex);
                        if (queue.requests.size() >= max_queued_iso_urbs) {
                            failed.store(true, std::memory_order_release);
                            break;
                        }
                        queue.requests.push_back(std::move(request));
                    }
                    queue.ready.notify_one();
                } else if (!processSubmit(client, device, request, control_timeline,
                                          send_mutex)) {
                    failed.store(true, std::memory_order_release);
                    break;
                }
                continue;
            }
            if (basic.command == kCmdUnlink) {
                std::array<Byte, 28> body{};
                if (!recvAll(client, body.data(), body.size()) ||
                    !replyUnlinkLocked(client, send_mutex, basic, kStatusOk))
                    break;
                continue;
            }
            break;
        }

        for (IsoEndpointQueue &queue : iso_queues) {
            {
                const std::lock_guard<std::mutex> lock(queue.mutex);
                queue.stopping = true;
                queue.requests.clear();
            }
            queue.ready.notify_one();
        }
        for (IsoEndpointQueue &queue : iso_queues) {
            if (queue.thread.joinable()) queue.thread.join();
        }
    }
};

UsbIpAudioManager::UsbIpAudioManager(bool publisher) : impl_(std::make_unique<Impl>(publisher)) {}
UsbIpAudioManager::~UsbIpAudioManager()
{
    clear();
}

bool UsbIpAudioManager::ensure(const std::vector<IoRequest> &requests,
                               std::uint32_t sample_rate, std::string &error)
{
    spdlog::debug("USB/IP engine binding: begin ({} request(s))", requests.size());
    const std::lock_guard<std::mutex> control_lock(impl_->control_mutex);
    if (impl_->publisher) {
        error = "the daemon-side USB/IP manager cannot bind engine audio ports";
        return false;
    }
    std::vector<IoRequest> virtual_requests;
    for (const IoRequest &request : requests) {
        if (!isVirtual(request.kind)) continue;
        if (request.channels < 1 || request.channels > 32
            || sample_rate < 8000 || sample_rate > kMaxSampleRate
            || highSpeedPayload(request.channels, sample_rate) == 0) {
            error = "the USB/IP audio format for '" + request.node
                    + "' exceeds the USB 2.0 high-speed isochronous limit";
            return false;
        }
        virtual_requests.push_back(request);
    }
    if (virtual_requests.empty()) return true;
    if (virtual_requests.size() > kMaxDevices) {
        error = "the USB/IP backend supports at most 32 virtual audio devices";
        return false;
    }
    impl_->sample_rate = sample_rate;
    const std::lock_guard<std::mutex> devices_lock(impl_->devices_mutex);
    for (const IoRequest &request : virtual_requests) {
        const std::string identity =
            deviceIdentity(request.node, request.kind, request.target, request.channels,
                           sample_rate);
        const auto found = std::find_if(impl_->devices.begin(), impl_->devices.end(),
                                        [&](const auto &device) {
                                            return device->impl_->identity == identity;
                                        });
        if (found != impl_->devices.end()) continue;
        auto device = makeDevice(0, request.node, request.target, request.kind, request.channels,
                                 sample_rate, false, error);
        if (!device) return false;
        impl_->devices.push_back(std::move(device));
    }
    spdlog::debug("USB/IP engine binding: complete ({} virtual device(s))",
                  virtual_requests.size());
    return true;
}

std::shared_ptr<UsbIpAudioDevice> UsbIpAudioManager::find(const IoRequest &request) const
{
    const std::string identity =
        deviceIdentity(request.node, request.kind, request.target, request.channels,
                       impl_->sample_rate);
    const std::lock_guard<std::mutex> lock(impl_->devices_mutex);
    const auto found =
        std::find_if(impl_->devices.begin(), impl_->devices.end(), [&](const auto &device) {
            return device->impl_->identity == identity;
        });
    return found == impl_->devices.end() ? std::shared_ptr<UsbIpAudioDevice>{} : *found;
}

bool UsbIpAudioManager::publish(const std::vector<VirtualDeviceRequest> &wanted,
                                std::string &error)
{
    spdlog::debug("USB/IP publication: begin ({} device(s))", wanted.size());
    const std::lock_guard<std::mutex> control_lock(impl_->control_mutex);
    if (!impl_->publisher) {
        error = "the engine-side USB/IP manager cannot publish Windows devices";
        return false;
    }
    if (wanted.size() > kMaxDevices) {
        error = "the USB/IP backend supports at most 32 virtual audio devices";
        return false;
    }
    std::set<std::string> names;
    for (const VirtualDeviceRequest &request : wanted) {
        if (request.channels < 1 || request.channels > 32 || request.sample_rate < 8000
            || request.sample_rate > kMaxSampleRate
            || highSpeedPayload(request.channels, request.sample_rate) == 0) {
            error = "the USB/IP audio format for '" + request.name
                    + "' exceeds the USB 2.0 high-speed isochronous limit";
            return false;
        }
        if (!names.insert(request.name).second) {
            error = "two nodes both publish '" + request.name + "'";
            return false;
        }
    }
    if (wanted.empty()) return true;
    const std::wstring usbip_executable = findUsbIpExecutable();
    if (usbip_executable.empty()) {
        error = "usbip.exe was not found; install usbip-win2 0.9.7.7 or another supported "
                "release and ensure its client is on PATH or under Program Files\\USBip";
        return false;
    }
    if (!usbIpClientIsAllowed(usbip_executable, error)) return false;

    std::vector<int> added_buses;
    {
        const std::lock_guard<std::mutex> devices_lock(impl_->devices_mutex);
        for (const VirtualDeviceRequest &request : wanted) {
            const std::string identity =
                deviceIdentity(request.owner, request.kind, request.name, request.channels,
                               request.sample_rate);
            const auto found = std::find_if(impl_->devices.begin(), impl_->devices.end(),
                                            [&](const auto &device) {
                                                return device->impl_->identity == identity;
                                            });
            if (found != impl_->devices.end()) continue;
            std::array<bool, 256> used{};
            for (const auto &device : impl_->devices)
                used[static_cast<std::size_t>(device->impl_->number)] = true;
            int number = 1;
            while (number < 256 && used[static_cast<std::size_t>(number)]) ++number;
            if (number >= 256) {
                error = "the USB/IP bus has no free device number";
                return false;
            }
            auto device = makeDevice(number, request.owner, request.name, request.kind,
                                     request.channels, request.sample_rate, true, error);
            if (!device) return false;
            impl_->devices.push_back(std::move(device));
            added_buses.push_back(number);
        }
    }
    if (!impl_->startServer(error)) return false;
    if (!impl_->synchronized || !added_buses.empty()) {
        std::vector<int> buses = added_buses;
        const std::string_view operation = impl_->synchronized ? "attach" : "sync";
        if (!impl_->synchronized) {
            buses.clear();
            for (const auto &device : impl_->deviceSnapshot())
                buses.push_back(device->impl_->number);
        }
        spdlog::debug("USB/IP publication: launching '{}' helper for {} bus(es)", operation,
                      buses.size());
        if (!launchElevated(operation, buses, error)) return false;
        spdlog::debug("USB/IP publication: '{}' helper completed", operation);
        impl_->synchronized = true;
        const std::lock_guard<std::mutex> devices_lock(impl_->devices_mutex);
        for (const auto &device : impl_->devices) {
            if (std::ranges::find(buses, device->impl_->number) != buses.end())
                device->impl_->attached = true;
        }
    }
    spdlog::debug("USB/IP publication: complete");
    return true;
}

void UsbIpAudioManager::retain(const std::vector<VirtualDeviceRequest> &wanted) noexcept
{
    if (!impl_->publisher) return;
    const std::lock_guard<std::mutex> control_lock(impl_->control_mutex);
    std::vector<int> obsolete_buses;
    {
        const std::lock_guard<std::mutex> devices_lock(impl_->devices_mutex);
        for (const auto &device : impl_->devices) {
            const bool wanted_device =
                std::ranges::any_of(wanted, [&](const VirtualDeviceRequest &request) {
                    return device->impl_->identity == deviceIdentity(
                               request.owner, request.kind, request.name, request.channels,
                               request.sample_rate);
                });
            if (!wanted_device && device->impl_->attached)
                obsolete_buses.push_back(device->impl_->number);
        }
    }
    if (!obsolete_buses.empty()) {
        std::string error;
        if (!launchElevated("detach", obsolete_buses, error)) {
            spdlog::error("cannot remove obsolete Windows virtual audio devices: {}", error);
            return;
        }
    }
    const std::lock_guard<std::mutex> devices_lock(impl_->devices_mutex);
    std::erase_if(impl_->devices, [&](const auto &device) {
        return std::none_of(wanted.begin(), wanted.end(), [&](const VirtualDeviceRequest &request) {
            return device->impl_->identity ==
                   deviceIdentity(request.owner, request.kind, request.name, request.channels,
                                  request.sample_rate);
        });
    });
}

void UsbIpAudioManager::clear() noexcept
{
    if (!impl_) return;
    const std::lock_guard<std::mutex> lock(impl_->control_mutex);
    if (impl_->publisher && impl_->synchronized) {
        std::vector<int> buses;
        for (const auto &device : impl_->deviceSnapshot()) {
            if (device->impl_->attached) buses.push_back(device->impl_->number);
        }
        if (!buses.empty()) {
            std::string error;
            if (!launchElevated("detach", buses, error))
                spdlog::warn("cannot detach Windows virtual audio devices during shutdown: {}",
                             error);
        }
    }
    impl_->stopServer();
    const std::lock_guard<std::mutex> devices_lock(impl_->devices_mutex);
    impl_->devices.clear();
}

namespace {

std::wstring quoteArgument(std::wstring_view argument)
{
    if (argument.find_first_of(L" \t\"") == std::wstring_view::npos) return std::wstring(argument);
    std::wstring out = L"\"";
    std::size_t slashes = 0;
    for (const wchar_t ch : argument) {
        if (ch == L'\\') {
            ++slashes;
            continue;
        }
        if (ch == L'\"') {
            out.append(slashes * 2 + 1, L'\\');
            out.push_back(L'\"');
            slashes = 0;
            continue;
        }
        out.append(slashes, L'\\');
        slashes = 0;
        out.push_back(ch);
    }
    out.append(slashes * 2, L'\\');
    out.push_back(L'\"');
    return out;
}

bool runUsbIp(const std::wstring &executable, const std::vector<std::wstring> &arguments,
              std::string &output)
{
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &security, 0)) return false;
    (void)SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    std::wstring command = quoteArgument(executable);
    for (const std::wstring &argument : arguments) {
        command.push_back(L' ');
        command += quoteArgument(argument);
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = write_pipe;
    startup.hStdError = write_pipe;
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    CloseHandle(write_pipe);
    if (!created) {
        CloseHandle(read_pipe);
        return false;
    }

    output.clear();
    std::array<char, 4096> buffer{};
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(read_pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read,
                      nullptr) ||
            read == 0)
            break;
        output.append(buffer.data(), read);
    }
    CloseHandle(read_pipe);
    (void)WaitForSingleObject(process.hProcess, 30000);
    DWORD exit_code = 1;
    (void)GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return exit_code == 0;
}

std::string trimAscii(std::string value)
{
    const auto space = [](unsigned char c) { return std::isspace(c) != 0; };
    value.erase(value.begin(),
                std::find_if(value.begin(), value.end(), [&](char c) { return !space(c); }));
    value.erase(
        std::find_if(value.rbegin(), value.rend(), [&](char c) { return !space(c); }).base(),
        value.end());
    return value;
}

struct AttachedPort {
    std::wstring port;
    std::wstring bus;
};

std::vector<AttachedPort> ownUsbIpPorts(std::string_view output)
{
    std::vector<AttachedPort> ports;
    std::istringstream lines{std::string(output)};
    std::string line;
    std::string current;
    while (std::getline(lines, line)) {
        const std::string text = trimAscii(line);
        if (text.rfind("Port ", 0) == 0) {
            const std::size_t colon = text.find(':', 5);
            current = colon == std::string::npos ? std::string{} : text.substr(5, colon - 5);
            continue;
        }
        constexpr std::string_view marker = "usbip://127.0.0.1:3240/";
        const std::size_t uri = text.find(marker);
        if (!current.empty() && uri != std::string::npos) {
            const std::size_t begin = uri + marker.size();
            const std::size_t end = text.find_first_of(" \t\r\n", begin);
            const std::string bus = text.substr(begin, end - begin);
            ports.push_back({std::wstring(current.begin(), current.end()),
                             std::wstring(bus.begin(), bus.end())});
            current.clear();
        }
    }
    return ports;
}

std::vector<int> parseBuses(std::string_view text)
{
    std::vector<int> buses;
    while (!text.empty()) {
        const std::size_t comma = text.find(',');
        const std::string_view item = text.substr(0, comma);
        if (item.empty() || !std::ranges::all_of(item, [](char ch) {
                return ch >= '0' && ch <= '9';
            }))
            return {};
        const unsigned long value = std::strtoul(std::string(item).c_str(), nullptr, 10);
        if (value == 0 || value > 255) return {};
        buses.push_back(static_cast<int>(value));
        if (comma == std::string_view::npos) break;
        text.remove_prefix(comma + 1);
    }
    std::ranges::sort(buses);
    buses.erase(std::unique(buses.begin(), buses.end()), buses.end());
    return buses;
}

}

UsbIpDriverStatus queryUsbIpDriverStatus()
{
    UsbIpDriverStatus status;
    const std::filesystem::path installer = bundledUsbIpInstaller();
    std::error_code ec;
    if (!installer.empty() && std::filesystem::is_regular_file(installer, ec)) {
        status.installer_available = bundledInstallerMatches(installer);
        if (!status.installer_available) {
            status.error = "the bundled usbip-win2 installer failed its SHA-256 check";
        }
    }

    const std::wstring executable = findUsbIpExecutable();
    if (executable.empty()) return status;
    status.client_present = true;
    if (const std::optional<FileVersion> version = executableVersion(executable)) {
        status.version = versionString(*version);
    }
    std::string compatibility_error;
    status.compatible = usbIpClientIsAllowed(executable, compatibility_error);
    if (!status.compatible) {
        status.error = std::move(compatibility_error);
        return status;
    }

    std::string output;
    status.driver_ready = runUsbIp(executable, {L"port"}, output);
    if (!status.driver_ready && status.error.empty()) {
        status.error = "usbip.exe is present, but the USB/IP driver is not ready";
    }
    return status;
}

bool installBundledUsbIpDriver(std::string &error)
{
    const std::filesystem::path installer = bundledUsbIpInstaller();
    std::error_code ec;
    if (installer.empty() || !std::filesystem::is_regular_file(installer, ec)) {
        error = "this AVC build does not contain the usbip-win2 driver installer";
        return false;
    }
    if (!bundledInstallerMatches(installer)) {
        error = "the bundled usbip-win2 installer failed its SHA-256 check";
        return false;
    }

    // usbip-win2 uses Inno Setup. Keep its progress window visible, suppress a
    // surprise reboot, and rely on the UI's explicit warning before reaching
    // this UAC prompt because installation briefly resets all USB 3 hubs.
    const std::wstring parameters = L"/SP- /SILENT /SUPPRESSMSGBOXES /NORESTART";
    const std::wstring directory = installer.parent_path().wstring();
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"runas";
    info.lpFile = installer.c_str();
    info.lpParameters = parameters.c_str();
    info.lpDirectory = directory.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info)) {
        const DWORD code = GetLastError();
        error = code == ERROR_CANCELLED
                    ? "USB/IP driver installation was cancelled at the administrator prompt"
                    : "cannot start the USB/IP driver installer (Windows error " +
                          std::to_string(code) + ")";
        return false;
    }

    constexpr DWORD kInstallerTimeoutMs = 5U * 60U * 1000U;
    const DWORD waited = WaitForSingleObject(info.hProcess, kInstallerTimeoutMs);
    DWORD exit_code = 1;
    const bool exited = waited == WAIT_OBJECT_0 && GetExitCodeProcess(info.hProcess, &exit_code);
    CloseHandle(info.hProcess);
    if (!exited) {
        error = "the USB/IP driver installer is still running; finish it before trying again";
        return false;
    }
    if (exit_code != 0) {
        error = "the USB/IP driver installer failed (exit " + std::to_string(exit_code) + ")";
        return false;
    }
    return true;
}

int runUsbIpDeviceHelper(std::string_view operation, std::string_view bus_list)
{
    if (operation != "sync" && operation != "attach" && operation != "detach") return 2;
    const std::vector<int> buses = parseBuses(bus_list);
    if (buses.empty() || buses.size() > kMaxDevices) return 2;
    const std::wstring executable = findUsbIpExecutable();
    if (executable.empty()) return 3;
    if (operation != "detach") {
        std::string ignored;
        if (!usbIpClientIsAllowed(executable, ignored)) return 7;
    }

    std::string output;
    if (operation != "detach") {
        bool listed = false;
        for (int attempt = 0; attempt < 20; ++attempt) {
            if (runUsbIp(executable, {L"list", L"-r", L"127.0.0.1"}, output)) {
                listed = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
        if (!listed) return 4;
    }

    std::vector<AttachedPort> attached;
    if (runUsbIp(executable, {L"port"}, output)) {
        attached = ownUsbIpPorts(output);
        for (const AttachedPort &port : attached) {
            const bool requested = std::ranges::any_of(buses, [&](int number) {
                return port.bus == L"1-" + std::to_wstring(number);
            });
            if (operation == "sync" || (operation == "detach" && requested)) {
                std::string ignored;
                if (!runUsbIp(executable, {L"detach", L"-p", port.port}, ignored)) return 5;
            }
        }
    }

    if (operation == "detach") return 0;
    for (const int number : buses) {
        const std::wstring bus_id = L"1-" + std::to_wstring(number);
        if (operation == "attach" && std::ranges::any_of(attached, [&](const AttachedPort &port) {
                return port.bus == bus_id;
            }))
            continue;
        if (!runUsbIp(executable, {L"attach", L"-r", L"127.0.0.1", L"-b", bus_id}, output)) {
            return 6;
        }
    }
    return 0;
}

}
