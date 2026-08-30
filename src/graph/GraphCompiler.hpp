#pragma once

#include "graph/CompiledGraph.hpp"
#include "graph/GraphSpec.hpp"
#include "audio/IoBinder.hpp"
#include "types/Audio.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace avc::graph {

struct CompileEnv {
    std::uint32_t sample_rate = types::kDefaultSampleRate;

    std::uint32_t max_quantum = types::kMaxQuantum;

    std::map<std::string, std::vector<std::uint32_t>> io_slots;

    std::uint64_t generation = 1;
};

class GraphCompiler {
public:
    static bool ioRequests(const GraphSpec &spec, std::vector<audio::IoRequest> &out,
                           std::string &error);

    static void virtualDevices(const GraphSpec &spec, std::vector<audio::IoRequest> &out);

    static std::unique_ptr<CompiledGraph> compile(const GraphSpec &spec, const CompileEnv &env,
                                                  const CompiledGraph *previous,
                                                  std::string &error);

private:
    class Builder;
};

}
