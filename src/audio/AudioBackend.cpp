#include "audio/AudioBackend.hpp"

#include <string>

namespace avc::audio {

std::string channelName(std::uint32_t index, std::uint32_t total)
{
    if (total == 1) {
        return "MONO";
    }
    if (total == 2) {
        return index == 0 ? "FL" : "FR";
    }
    return "AUX" + std::to_string(index);
}

}
