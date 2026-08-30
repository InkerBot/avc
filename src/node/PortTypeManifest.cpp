#include "node/PortTypeManifest.hpp"

namespace avc::node {
namespace {

bool sameShape(const PortTypeDescriptor &a, const PortTypeDescriptor &b) noexcept
{
    return a.transport == b.transport && a.bytes == b.bytes;
}

}

PortTypeManifest::PortTypeManifest()
{
    // Audio is index 0 and stays index 0: a descriptor that names no type gets
    // it, which is every port that existed before types did.
    types_.push_back({kAudioPortType, "Audio", PortTransport::Stream,
                      static_cast<std::uint32_t>(sizeof(types::Sample)), {}});
    types_.push_back({kTextPortType, "Text", PortTransport::Value, kTextPortTypeBytes, {}});
}

PortTypeManifest &PortTypeManifest::instance()
{
    static PortTypeManifest manifest;
    return manifest;
}

int PortTypeManifest::indexOf(std::string_view name) const noexcept
{
    if (name.empty()) {
        return static_cast<int>(kAudioPortTypeIndex);
    }
    for (std::size_t i = 0; i < types_.size(); ++i) {
        if (types_[i].name == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

const PortTypeDescriptor *PortTypeManifest::find(std::string_view name) const noexcept
{
    const int index = indexOf(name);
    return index < 0 ? nullptr : &types_[static_cast<std::size_t>(index)];
}

bool PortTypeManifest::add(PortTypeDescriptor descriptor, std::string &error)
{
    if (sealed_) {
        error = "port type '" + descriptor.name + "' arrived after the manifest was sealed";
        return false;
    }
    if (descriptor.name.empty()) {
        error = "a port type has to have a name";
        return false;
    }
    if (descriptor.bytes == 0) {
        error = "port type '" + descriptor.name + "' carries nothing";
        return false;
    }

    const int existing = indexOf(descriptor.name);
    if (existing >= 0) {
        const PortTypeDescriptor &have = types_[static_cast<std::size_t>(existing)];
        if (!sameShape(have, descriptor)) {
            error = "port type '" + descriptor.name + "' is already declared with a different shape"
                    + (have.extension.empty() ? ", as a built-in" : ", by '" + have.extension + "'");
            return false;
        }
        return true;
    }

    if (descriptor.label.empty()) {
        descriptor.label = descriptor.name;
    }
    types_.push_back(std::move(descriptor));
    return true;
}

}
