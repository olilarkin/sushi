/*
 * Copyright 2017-2025 Elk Audio AB, Stockholm
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

/**
 * @brief Simple monophonic synthesizer from Brickworks library
 * @copyright 2017-2025 Elk Audio AB, Stockholm
 */

#include <bw_buf.h>

#include "elklog/static_logger.h"

#include "simple_synth_plugin.h"

#include <iostream>

namespace sushi::internal::simple_synth_plugin {

ELKLOG_GET_LOGGER_WITH_MODULE_NAME("simplesynth");

constexpr auto PLUGIN_UID = "sushi.brickworks.simple_synth";
constexpr auto DEFAULT_LABEL = "Simple synthesizer";

constexpr float A4_FREQUENCY = 440.0f;
constexpr int A4_NOTENUM = 69;
constexpr float NOTE2FREQ_SCALE = 5.0f / 60.0f;


SimpleSynthPlugin::SimpleSynthPlugin(HostControl host_control) : InternalPlugin(host_control)
{
    Processor::set_name(PLUGIN_UID);
    Processor::set_label(DEFAULT_LABEL);

    _volume = register_float_parameter("volume", "Volume", "dB",
                                       0.0f, -60.0f, 12.0f,
                                       Direction::AUTOMATABLE,
                                       new dBToLinPreProcessor(-60.0f, 12.0f));

    // scaling taken from newer Brickworks synth examples

    constexpr float MAX_PORTAMENTO = 0.43429448190325173f;
    _portamento = register_float_parameter("portamento", "Portamento time", "sec",
                                           0.0f, 0.0f, MAX_PORTAMENTO,
                                           Direction::AUTOMATABLE,
                                           new FloatParameterPreProcessor(0.0f, MAX_PORTAMENTO));
    _pulse_width = register_float_parameter("pulse_width", "Pulse width", "",
                                            0.5f, 0.0f, 1.0f,
                                            Direction::AUTOMATABLE,
                                            new FloatParameterPreProcessor(0.0f, 1.0f));
    _filter_cutoff = register_float_parameter("filter_cutoff", "Filter cutoff", "Hz",
                                              4'000.0f, 20.0f, 20'000.0f,
                                              Direction::AUTOMATABLE,
                                              new CubicWarpPreProcessor(20.0f, 20'000.0f));
    _filter_Q = register_float_parameter("filter_Q", "Filter Q", "",
                                         1.0f, 0.5f, 10.0f,
                                         Direction::AUTOMATABLE,
                                         new FloatParameterPreProcessor(0.5f, 10.0f));
    _attack = register_float_parameter("attack", "Attack time", "sec",
                                       0.002f, 0.002f, 1.0f,
                                       Direction::AUTOMATABLE,
                                       new FloatParameterPreProcessor(0.002f, 1.0f));
    _decay = register_float_parameter("decay", "Decay time", "sec",
                                      0.002f, 0.002f, 1.0f,
                                      Direction::AUTOMATABLE,
                                      new FloatParameterPreProcessor(0.002f, 1.0f));
    _sustain = register_float_parameter("sustain", "Sustain level", "",
                                        1.0f, 0.0f, 1.0f,
                                        Direction::AUTOMATABLE,
                                        new FloatParameterPreProcessor(0.0f, 1.0f));
    _release = register_float_parameter("release", "Release time", "sec",
                                        0.002f, 0.002f, 1.0f,
                                        Direction::AUTOMATABLE,
                                        new FloatParameterPreProcessor(0.002f, 1.0f));

    _max_input_channels = 0;
    _supports_midi_input = true;
};

ProcessorReturnCode SimpleSynthPlugin::init(float sample_rate)
{
    bw_phase_gen_init(&_phase_gen_coeffs);
    bw_osc_pulse_init(&_osc_pulse_coeffs);
    bw_svf_init(&_svf_coeffs);
    bw_env_gen_init(&_env_gen_coeffs);

    bw_osc_pulse_set_antialiasing(&_osc_pulse_coeffs, 1);

    configure(sample_rate);

    return ProcessorReturnCode::OK;
}

void SimpleSynthPlugin::configure(float sample_rate)
{
    bw_phase_gen_set_sample_rate(&_phase_gen_coeffs, sample_rate);
    bw_osc_pulse_set_sample_rate(&_osc_pulse_coeffs, sample_rate);
    bw_svf_set_sample_rate(&_svf_coeffs, sample_rate);
    bw_env_gen_set_sample_rate(&_env_gen_coeffs, sample_rate);
}

void SimpleSynthPlugin::set_enabled(bool enabled)
{
    Processor::set_enabled(enabled);

    // Initialize note tracking
    for (int i = 0; i < MAX_MIDI_NOTE; i++)
    {
        _held_notes[i] = false;
    }
    _note = A4_NOTENUM;
    _freq = A4_FREQUENCY;
    _gate = 0;

    // Reset all brickworks modules
    bw_phase_gen_reset_coeffs(&_phase_gen_coeffs);
    float phase_out, phase_inc_out;
    bw_phase_gen_reset_state(&_phase_gen_coeffs, &_phase_gen_state, 0.0f, &phase_out, &phase_inc_out);

    bw_osc_pulse_reset_coeffs(&_osc_pulse_coeffs);
    bw_osc_filt_reset_state(&_osc_filt_state, 0.0f);

    bw_svf_reset_coeffs(&_svf_coeffs);
    float svf_lp, svf_bp, svf_hp;
    bw_svf_reset_state(&_svf_coeffs, &_svf_state, 0.0f, &svf_lp, &svf_bp, &svf_hp);

    bw_env_gen_reset_coeffs(&_env_gen_coeffs);
    bw_env_gen_reset_state(&_env_gen_coeffs, &_env_gen_state, 0);
}


void SimpleSynthPlugin::process_event(const RtEvent& event)
{
    // Just forward everything to the process() callback with a FIFO,
    // to correctly handle sample-accurate events
    switch (event.type())
    {
        case RtEventType::NOTE_ON:
        case RtEventType::NOTE_OFF:
        {
            if (_bypassed)
            {
                break;
            }
            if (!_event_fifo.push(event))
            {
                ELKLOG_LOG_ERROR("Internal queue full while processing event");
            }
            break;
        }

        case RtEventType::NOTE_AFTERTOUCH:
        case RtEventType::PITCH_BEND:
        case RtEventType::AFTERTOUCH:
        case RtEventType::MODULATION:
        case RtEventType::WRAPPED_MIDI_EVENT:
            // Consume these events so they are not propagated
            break;

        default:
            InternalPlugin::process_event(event);
            break;
    }
}


void SimpleSynthPlugin::process_audio(const ChunkSampleBuffer& /* in_buffer */, ChunkSampleBuffer& out_buffer)
{
    out_buffer.clear();
    bw_phase_gen_set_portamento_tau(&_phase_gen_coeffs, _portamento->processed_value());
    bw_osc_pulse_set_pulse_width(&_osc_pulse_coeffs, _pulse_width->processed_value());
    bw_svf_set_cutoff(&_svf_coeffs, _filter_cutoff->processed_value());
    bw_svf_set_Q(&_svf_coeffs, _filter_Q->processed_value());
    bw_env_gen_set_attack(&_env_gen_coeffs, _attack->processed_value());
    bw_env_gen_set_decay(&_env_gen_coeffs, _decay->processed_value());
    bw_env_gen_set_sustain(&_env_gen_coeffs, _sustain->processed_value());
    bw_env_gen_set_release(&_env_gen_coeffs, _release->processed_value());

    int previous_offset = 0;
    RtEvent event;

    while (_event_fifo.pop(event))
    {
        int next_offset = event.sample_offset();
        // we're assuming that events are received in order,
        // if that's not the case simply drop the event
        if (next_offset < previous_offset)
        {
            ELKLOG_LOG_DEBUG("Dropping unordered event of type {} with sample offset {}",
                             static_cast<int>(event.type()), event.sample_offset());
            continue;
        }
        _render_loop(previous_offset, next_offset);

        auto key_event = event.keyboard_event();
        int note = key_event->note();
        switch (key_event->type())
        {
        case RtEventType::NOTE_ON:
        {
            ELKLOG_LOG_DEBUG("Note ON, num. {}, vel. {}",
                             note, key_event->velocity());
            _held_notes[note] = true;
            _update_note_gate();
            break;
        }

        case RtEventType::NOTE_OFF:
        {
            ELKLOG_LOG_DEBUG("Note OFF, num. {}, vel. {}",
                            note, key_event->velocity());
            if (_held_notes[note])
            {
                _held_notes[note] = false;
                _update_note_gate();
            }
            break;
        }

        default:
            ELKLOG_LOG_DEBUG("Unexpected event type passed to process(): {}", static_cast<int>(key_event->type()));

        }
        previous_offset = next_offset;
    }

    // Process any remaining samples
    int remaining = AUDIO_CHUNK_SIZE - previous_offset;
    if (remaining > 0)
    {
        _render_loop(previous_offset, remaining);
    }

    if (!_bypassed)
    {
        float gain = _volume->processed_value();
        out_buffer.add_with_gain(_render_buffer, gain);
    }
}

void SimpleSynthPlugin::_update_note_gate()
{
    for (int i = MAX_MIDI_NOTE-1; i >= 0; i--)
    {
        if (_held_notes[i])
        {
            _note = i;
            _gate = 1;
            // the Brickworks example also had other tuning parameters, skipped here and precomputing a fixed freq instead
            _freq = A4_FREQUENCY * bw_pow2f(NOTE2FREQ_SCALE * static_cast<float>(_note - A4_NOTENUM));
            return;
        }
    }
    _gate = 0;

}

void SimpleSynthPlugin::_render_loop(int offset, int n)
{
    float* out = &_render_buffer.channel(0)[offset];
    float* buf = &_aux_buffer.channel(0)[offset];

	const bool vca_open = bw_env_gen_get_phase(&_env_gen_state) != bw_env_gen_phase_off || _gate;

    bw_phase_gen_set_frequency(&_phase_gen_coeffs, _freq);
    bw_phase_gen_process(&_phase_gen_coeffs, &_phase_gen_state, nullptr, out, buf, n);
    bw_osc_pulse_process(&_osc_pulse_coeffs, out, buf, out, n);
    bw_osc_filt_process(&_osc_filt_state, out, out, n);
    bw_svf_process(&_svf_coeffs, &_svf_state, out, out, nullptr, nullptr, n);
    if (vca_open)
    {
        bw_env_gen_process(&_env_gen_coeffs, &_env_gen_state, _gate, buf, n);
        bw_buf_mul(out, out, buf, n);
    }
    else
    {
        bw_buf_fill(0.0f, out, n);
    }
}

std::string_view SimpleSynthPlugin::static_uid()
{
    return PLUGIN_UID;
}


} // namespace sushi::internal::simple_synth_plugin
