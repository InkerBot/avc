#pragma once

#ifdef _WIN32
#include "audio/WasapiBackend.hpp"
#include "audio/WasapiSession.hpp"
#include "audio/WasapiVirtualDevices.hpp"
#else
#include "audio/PipeWireBackend.hpp"
#include "audio/PwSession.hpp"
#include "audio/PwVirtualDevices.hpp"
#endif

namespace avc::audio {

#ifdef _WIN32
using HostBackend = WasapiBackend;
using HostSession = WasapiSession;
using HostVirtualDevices = WasapiVirtualDevices;
#else
using HostBackend = PipeWireBackend;
using HostSession = PwSession;
using HostVirtualDevices = PwVirtualDevices;
#endif

}
