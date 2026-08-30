#pragma once

#include "audio/AudioBackend.hpp"
#include "audio/IoBinder.hpp"
#include "audio/PwSession.hpp"
#include "rt/RcuSlot.hpp"

#include <pipewire/pipewire.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace avc::audio {

class PipeWireBackend final : public AudioBackend, public IoBinder {
public:
    PipeWireBackend() = default;
    ~PipeWireBackend() override;

    void setForceQuantum(bool force) noexcept { force_quantum_ = force; }
    void setNodeName(std::string name) { engine_name_ = std::move(name); }

    bool open() override;
    void close() noexcept override;

    std::vector<DeviceInfo> enumerateDevices() override;
    std::string defaultDeviceName(Direction direction) override;
    bool start(const types::AudioFormat &format, Processor *processor) override;
    void stop() noexcept override;
    BackendStats stats() const noexcept override;
    void resetPeaks() noexcept override;

    bool bindIo(const std::vector<IoRequest> &requests,
                std::map<std::string, std::vector<std::uint32_t>> &slots,
                std::string &error) override;
    void releaseIo(const std::set<std::string> &keep) override;

    std::uint32_t engineNodeId() const noexcept;

private:
    static constexpr std::size_t kMaxSlots = 64;
    static constexpr std::uint32_t kNoSlot = 0xFFFFFFFFU;

    struct PortEntry {
        void *data = nullptr;
        std::string owner;
        std::string target;
        std::string name;
        std::uint32_t channel = 0;
    };

    struct PortTable {
        std::vector<void *> inputs;
        std::vector<void *> outputs;
    };

    struct FilterState {
        PipeWireBackend *self = nullptr;
        pw_filter *filter = nullptr;
        spa_hook listener{};
        bool listening = false;
        std::uint32_t node_id = 0;
    };

    struct OwnedLink {
        pw_proxy *proxy = nullptr;
        std::string owner;
    };

    static void onFilterState(void *data, pw_filter_state old_state, pw_filter_state state,
                              const char *error);
    static void onFilterProcess(void *data, spa_io_position *position);

    // ZFW: HOT PATH
    void processEngine(spa_io_position *position) noexcept;
    void firstCycle() noexcept;

    bool createEngine(const types::AudioFormat &format);

    std::vector<PortEntry> &tableFor(IoKind kind) noexcept;
    static Direction directionFor(IoKind kind) noexcept;
    std::uint32_t findSlot(const std::vector<PortEntry> &table, const std::string &owner,
                           std::uint32_t channel) const noexcept;
    std::uint32_t openPort(std::vector<PortEntry> &table, const IoRequest &request,
                           std::uint32_t channel, Direction direction);
    bool portsPresent(const std::vector<IoRequest> &requests) const;
    bool wireRequest(const IoRequest &request, const std::vector<std::uint32_t> &slots);

    void onLinkAppeared(const PwLink &link);
    void enforceExclusive();
    void publishTable();

    bool createLink(std::uint32_t out_node, std::uint32_t out_port, std::uint32_t in_node,
                    std::uint32_t in_port, std::string owner);
    void destroyLinks(const std::string &owner) noexcept;
    void destroyLinks() noexcept;

    PwSession session_;
    std::vector<OwnedLink> links_;

    std::unique_ptr<FilterState> engine_;

    std::set<std::string> exclusive_targets_;

    mutable std::mutex control_mutex_;
    std::vector<PortEntry> input_slots_;
    std::vector<PortEntry> output_slots_;
    rt::RcuSlot<PortTable> table_;

    std::array<types::Sample *, kMaxSlots> in_bufs_{};
    std::array<types::Sample *, kMaxSlots> out_bufs_{};

    Processor *processor_ = nullptr;
    types::AudioFormat format_{};
    std::string engine_name_ = "avc";
    bool force_quantum_ = false;
    bool started_ = false;

    std::atomic<std::uint64_t> cycles_{0};
    std::atomic<std::uint64_t> xruns_{0};
    std::atomic<std::uint64_t> process_ns_last_{0};
    std::atomic<std::uint64_t> process_ns_max_{0};
    std::atomic<std::uint64_t> process_ns_avg_{0};
    std::atomic<std::uint64_t> wakeup_jitter_max_{0};

    std::uint64_t last_wakeup_ns_ = 0;
    std::atomic<std::uint32_t> actual_quantum_{0};
    std::atomic<std::uint32_t> actual_rate_{0};
    std::atomic<int> sched_policy_{-1};
    std::atomic<int> sched_priority_{0};
    std::atomic<std::int32_t> audio_tid_{0};
    bool in_xrun_ = false;
};

}
