#pragma once

#include "types/Audio.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace avc::audio {

enum class Direction : std::uint8_t {
    Input,
    Output,
};

std::string channelName(std::uint32_t index, std::uint32_t total);

struct DeviceInfo {
    std::uint32_t id = 0;
    std::string name;
    std::string description;
    std::string media_class;
    std::string application;
    std::string media_name;
    std::uint32_t input_ports = 0;
    std::uint32_t output_ports = 0;
};

struct ProcessContext {
    types::Sample *const *inputs = nullptr;
    types::Sample *const *outputs = nullptr;
    std::uint32_t n_inputs = 0;
    std::uint32_t n_outputs = 0;
    std::uint32_t nframes = 0;
    std::uint64_t cycle = 0;
};

class Processor {
public:
    virtual ~Processor() = default;

    // ZFW: HOT PATH
    virtual void process(const ProcessContext &ctx) noexcept = 0;
};

struct BackendStats {
    std::uint64_t cycles = 0;
    std::uint64_t xruns = 0;
    std::uint64_t process_ns_last = 0;
    std::uint64_t process_ns_max = 0;

    std::uint64_t process_ns_avg = 0;

    std::uint64_t wakeup_jitter_ns_max = 0;
    std::uint32_t quantum = 0;
    std::uint32_t sample_rate = 0;
    std::uint32_t actual_quantum = 0;
    std::uint32_t actual_rate = 0;
    std::uint32_t input_latency_frames = 0;
    std::uint32_t output_latency_frames = 0;
    bool io_latency_known = false;
    bool device_driven = false;
    int sched_policy = -1;
    int sched_priority = 0;
    std::int32_t audio_tid = 0;
};

class AudioBackend {
public:
    virtual ~AudioBackend() = default;

    AudioBackend() = default;
    AudioBackend(const AudioBackend &) = delete;
    AudioBackend &operator=(const AudioBackend &) = delete;

    virtual bool open() = 0;
    virtual void close() noexcept = 0;

    virtual std::vector<DeviceInfo> enumerateDevices() = 0;

    virtual std::string defaultDeviceName(Direction direction) = 0;

    virtual bool start(const types::AudioFormat &format, Processor *processor) = 0;
    virtual void stop() noexcept = 0;

    virtual BackendStats stats() const noexcept = 0;

    virtual void resetPeaks() noexcept = 0;
};

}
