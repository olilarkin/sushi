/*
 * Copyright 2026 Oliver Larkin
 *
 * SUSHI is free software: you can redistribute it and/or modify it under the terms of
 * the GNU Affero General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * SUSHI is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE. See the GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License along with
 * SUSHI. If not, see http://www.gnu.org/licenses/
 */

#include "supersonic_bridge.h"

// Supersonic / scsynth core headers. These are intentionally confined to this
// single translation unit (compiled with the Supersonic include dirs + the
// emscripten NativeShim) so the rest of Sushi never sees them.
#include "audio_processor.h"   // process_audio / init_memory / get_audio_*_bus / ring_buffer_storage
#include "shared_memory.h"     // ControlPointers, *_BUFFER_* offsets, WORLD_OPTIONS_START
#include "audio_config.h"      // sonicpi::WorldOpts
#include "workers/RingBufferWriter.h"

#include "osc/OscOutboundPacketStream.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <sstream>
#include <vector>

namespace sushi::internal::supercollider {

namespace {

// The scsynth core is a single-World, process-global singleton. This guards
// against a second SuperCollider processor being instantiated in the same
// Sushi process (which would corrupt the shared ring buffer / g_world).
std::atomic<bool> g_engine_in_use {false};

// Reasonable defaults for an embedded, single-track synthesis World. These
// mirror SupersonicEngine::Config; generous enough for typical live use.
constexpr int kNumBuffers          = 1024;
constexpr int kMaxNodes            = 1024;
constexpr int kMaxGraphDefs        = 512;
constexpr int kMaxWireBufs         = 64;
constexpr int kNumAudioBusChannels = 1024;
constexpr int kNumControlBuses     = 16384;
constexpr int kRealTimeMemoryKB    = 8192;
constexpr int kNumRGens            = 64;

ControlPointers* control_pointers()
{
    return reinterpret_cast<ControlPointers*>(ring_buffer_storage + CONTROL_START);
}

// Append one whitespace-delimited token to the OSC message, choosing an
// argument type heuristically: all-digit (with optional sign) -> int32,
// numeric with '.'/'e' -> float, otherwise a string.
void append_osc_arg(osc::OutboundPacketStream& p, const std::string& tok)
{
    if (tok.empty())
    {
        p << "";
        return;
    }

    bool looks_numeric = true;
    bool is_float = false;
    for (size_t i = 0; i < tok.size(); ++i)
    {
        char c = tok[i];
        if (c == '.' || c == 'e' || c == 'E')
        {
            is_float = true;
        }
        else if (std::isdigit(static_cast<unsigned char>(c)))
        {
            // ok
        }
        else if ((c == '+' || c == '-') && i == 0)
        {
            // leading sign ok
        }
        else
        {
            looks_numeric = false;
            break;
        }
    }
    // A bare sign or empty-after-sign is not a number.
    if (looks_numeric && (tok == "+" || tok == "-" || tok == "."))
    {
        looks_numeric = false;
    }

    if (!looks_numeric)
    {
        p << tok.c_str();
    }
    else if (is_float)
    {
        p << static_cast<float>(std::strtod(tok.c_str(), nullptr));
    }
    else
    {
        p << static_cast<osc::int32>(std::strtol(tok.c_str(), nullptr, 10));
    }
}

} // anonymous namespace

SupersonicBridge::~SupersonicBridge()
{
    if (_owns_engine)
    {
#ifndef __EMSCRIPTEN__
        destroy_world();
#endif
        g_engine_in_use.store(false, std::memory_order_release);
    }
}

bool SupersonicBridge::init(double sample_rate, int block_size, int num_inputs, int num_outputs)
{
    if (_initialised)
    {
        return true;
    }

    bool expected = false;
    if (!g_engine_in_use.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
    {
        // Another SuperCollider processor already owns the process-global World.
        return false;
    }
    _owns_engine = true;

    _block_size = block_size;
    _num_inputs = num_inputs;
    _num_outputs = num_outputs;
    _block_duration = (sample_rate > 0.0) ? static_cast<double>(block_size) / sample_rate : 0.0;

    // Write WorldOptions into ring_buffer_storage at WORLD_OPTIONS_START, exactly
    // as JuceAudioCallback::initialiseWorld would, then build the World via init_memory().
    uint32_t* opts = reinterpret_cast<uint32_t*>(ring_buffer_storage + WORLD_OPTIONS_START);
    using namespace sonicpi::WorldOpts;
    opts[kNumBuffers]            = static_cast<uint32_t>(::sushi::internal::supercollider::kNumBuffers);
    opts[kMaxNodes]              = static_cast<uint32_t>(::sushi::internal::supercollider::kMaxNodes);
    opts[kMaxGraphDefs]          = static_cast<uint32_t>(::sushi::internal::supercollider::kMaxGraphDefs);
    opts[kMaxWireBufs]           = static_cast<uint32_t>(::sushi::internal::supercollider::kMaxWireBufs);
    opts[kNumAudioBusChannels]   = static_cast<uint32_t>(::sushi::internal::supercollider::kNumAudioBusChannels);
    opts[kNumInputBusChannels]   = static_cast<uint32_t>(num_inputs);
    opts[kNumOutputBusChannels]  = static_cast<uint32_t>(num_outputs);
    opts[kNumControlBusChannels] = static_cast<uint32_t>(kNumControlBuses);
    opts[kBufLength]             = static_cast<uint32_t>(block_size);
    opts[kRealTimeMemorySize]    = static_cast<uint32_t>(kRealTimeMemoryKB);
    opts[kNumRGens]              = static_cast<uint32_t>(kNumRGens);
    opts[kRealTime]              = 0;   // NRT/SAB mode — commands processed inline in process_audio
    opts[kMemoryLocking]         = 0;
    opts[kLoadGraphDefs]         = 1;
    opts[kSampleRate]            = static_cast<uint32_t>(sample_rate);
    opts[kVerbosity]             = 0;
    opts[kMode]                  = 0;   // direct memory access
    opts[17]                     = 0;   // sharedMemoryID — none (no external POSIX shm)

    init_memory(sample_rate);

    if (get_audio_buffer_samples() != block_size)
    {
        // World failed to build at the requested block size.
#ifndef __EMSCRIPTEN__
        destroy_world();
#endif
        g_engine_in_use.store(false, std::memory_order_release);
        _owns_engine = false;
        return false;
    }

    _time = 0.0;
    _initialised = true;
    return true;
}

int SupersonicBridge::block_size() const
{
    return _block_size;
}

void SupersonicBridge::process(const float* const* inputs, int num_inputs,
                               float* const* outputs, int num_outputs)
{
    if (!_initialised)
    {
        for (int ch = 0; ch < num_outputs; ++ch)
        {
            if (outputs[ch])
            {
                std::memset(outputs[ch], 0, static_cast<size_t>(_block_size) * sizeof(float));
            }
        }
        return;
    }

    const int n = _block_size;

    // Feed the input bus (channel-major: channel c occupies [c*n, c*n+n)).
    auto* input_bus = reinterpret_cast<float*>(get_audio_input_bus());
    if (input_bus && _num_inputs > 0)
    {
        int in_ch = std::min(num_inputs, _num_inputs);
        for (int ch = 0; ch < in_ch; ++ch)
        {
            if (inputs && inputs[ch])
            {
                std::memcpy(input_bus + ch * n, inputs[ch], static_cast<size_t>(n) * sizeof(float));
            }
            else
            {
                std::memset(input_bus + ch * n, 0, static_cast<size_t>(n) * sizeof(float));
            }
        }
        for (int ch = in_ch; ch < _num_inputs; ++ch)
        {
            std::memset(input_bus + ch * n, 0, static_cast<size_t>(n) * sizeof(float));
        }
    }

    process_audio(_time,
                  static_cast<uint32_t>(_num_outputs),
                  static_cast<uint32_t>(_num_inputs));
    _time += _block_duration;

    // Pull the master output mix (also channel-major).
    auto* output_bus = reinterpret_cast<float*>(get_audio_output_bus());
    int out_ch = std::min(num_outputs, _num_outputs);
    for (int ch = 0; ch < out_ch; ++ch)
    {
        if (outputs[ch])
        {
            if (output_bus)
            {
                std::memcpy(outputs[ch], output_bus + ch * n, static_cast<size_t>(n) * sizeof(float));
            }
            else
            {
                std::memset(outputs[ch], 0, static_cast<size_t>(n) * sizeof(float));
            }
        }
    }
    for (int ch = out_ch; ch < num_outputs; ++ch)
    {
        if (outputs[ch])
        {
            std::memset(outputs[ch], 0, static_cast<size_t>(n) * sizeof(float));
        }
    }
}

bool SupersonicBridge::send_osc_packet(const void* data, uint32_t size)
{
    if (!_initialised || data == nullptr || size == 0)
    {
        return false;
    }

    auto* ctrl = control_pointers();
    return RingBufferWriter::write(ring_buffer_storage + IN_BUFFER_START,
                                   IN_BUFFER_SIZE,
                                   &ctrl->in_head,
                                   &ctrl->in_tail,
                                   &ctrl->in_sequence,
                                   &ctrl->in_write_lock,
                                   data,
                                   size);
}

bool SupersonicBridge::send_osc_text(const std::string& line)
{
    std::istringstream stream(line);
    std::string address;
    stream >> address;
    if (address.empty() || address[0] != '/')
    {
        return false;
    }

    std::vector<char> buffer(8192);
    osc::OutboundPacketStream p(buffer.data(), buffer.size());
    try
    {
        p << osc::BeginMessage(address.c_str());
        std::string tok;
        while (stream >> tok)
        {
            append_osc_arg(p, tok);
        }
        p << osc::EndMessage;
    }
    catch (const std::exception&)
    {
        return false;
    }

    return send_osc_packet(p.Data(), static_cast<uint32_t>(p.Size()));
}

bool SupersonicBridge::load_synthdef_blob(const void* data, uint32_t size)
{
    if (data == nullptr || size == 0)
    {
        return false;
    }

    std::vector<char> buffer(size + 1024u);
    osc::OutboundPacketStream p(buffer.data(), buffer.size());
    try
    {
        p << osc::BeginMessage("/d_recv")
          << osc::Blob(data, static_cast<osc::osc_bundle_element_size_t>(size))
          << osc::EndMessage;
    }
    catch (const std::exception&)
    {
        return false;
    }

    return send_osc_packet(p.Data(), static_cast<uint32_t>(p.Size()));
}

} // namespace sushi::internal::supercollider
