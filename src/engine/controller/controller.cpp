/*
 * Copyright 2017-2023 Elk Audio AB
 *
 * Modified/Added by Oliver Larkin @Copyright 2026
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
 * @Brief Controller object for external control of sushi
 * @Copyright 2017-2023 Elk Audio AB, Stockholm
 */

#include "elklog/static_logger.h"

#include "controller.h"

#include "sushi/control_notifications.h"

#include "engine/base_engine.h"
#include "controller_common.h"

ELKLOG_GET_LOGGER_WITH_MODULE_NAME("controller");

namespace sushi::internal::engine {

control::ControlStatus to_external_status(int status)
{
    switch (status)
    {
        case EventStatus::HANDLED_OK:                   return sushi::control::ControlStatus::OK;
        case EventStatus::ERROR:                        return sushi::control::ControlStatus::ERROR;
        case ControlEventStatus::UNSUPPORTED_OPERATION: return sushi::control::ControlStatus::UNSUPPORTED_OPERATION;
        case ControlEventStatus::NOT_FOUND:             return sushi::control::ControlStatus::NOT_FOUND;
        case ControlEventStatus::OUT_OF_RANGE:          return sushi::control::ControlStatus::OUT_OF_RANGE;
        case ControlEventStatus::INVALID_ARGUMENTS:     return sushi::control::ControlStatus::INVALID_ARGUMENTS;
        default:                                        return sushi::control::ControlStatus::ERROR;
    }
}

using namespace controller_impl;

Controller::Controller(engine::BaseEngine* engine,
                       midi_dispatcher::MidiDispatcher* midi_dispatcher,
                       audio_frontend::BaseAudioFrontend* audio_frontend) : control::SushiControl(&_system_controller_impl,
                                                                                                  &_transport_controller_impl,
                                                                                                  &_timing_controller_impl,
                                                                                                  &_keyboard_controller_impl,
                                                                                                  &_audio_graph_controller_impl,
                                                                                                  &_program_controller_impl,
                                                                                                  &_parameter_controller_impl,
                                                                                                  &_midi_controller_impl,
                                                                                                  &_audio_routing_controller_impl,
                                                                                                  &_cv_gate_controller_impl,
                                                                                                  &_osc_controller_impl,
                                                                                                  &_session_controller_impl,
                                                                                                  &_editor_controller_impl),
                                                                             _system_controller_impl(engine->audio_input_channels(),
                                                                                                     engine->audio_output_channels()),
                                                                             _transport_controller_impl(engine),
                                                                             _timing_controller_impl(engine),
                                                                             _keyboard_controller_impl(engine),
                                                                             _audio_graph_controller_impl(engine, this),
                                                                             _program_controller_impl(engine, this),
                                                                             _parameter_controller_impl(engine),
                                                                             _midi_controller_impl(engine, midi_dispatcher, this),
                                                                             _audio_routing_controller_impl(engine, this),
                                                                             _cv_gate_controller_impl(engine, this),
                                                                             _osc_controller_impl(engine, this),
                                                                             _session_controller_impl(engine,
                                                                                                      midi_dispatcher,
                                                                                                      audio_frontend,
                                                                                                      this),
                                                                             _editor_controller_impl(engine->processor_container())
{
    _event_dispatcher = engine->event_dispatcher();
    _processors = engine->processor_container();

    _event_dispatcher->subscribe_to_parameter_change_notifications(this);
    _event_dispatcher->subscribe_to_engine_notifications(this);
}

Controller::~Controller()
{
    _event_dispatcher->unsubscribe_from_parameter_change_notifications(this);
    _event_dispatcher->unsubscribe_from_engine_notifications(this);
}

control::ControlStatus Controller::subscribe_to_notifications(control::NotificationType type,
                                                              control::ControlListener* listener)
{
    switch (type)
    {
        case control::NotificationType::PARAMETER_CHANGE:
            _parameter_change_listeners.push_back(listener);
            break;
        case control::NotificationType::PROPERTY_CHANGE:
            _property_change_listeners.push_back(listener);
            break;
        case control::NotificationType::PROCESSOR_UPDATE:
            _processor_update_listeners.push_back(listener);
            break;
        case control::NotificationType::TRACK_UPDATE:
            _track_update_listeners.push_back(listener);
            break;
        case control::NotificationType::TRANSPORT_UPDATE:
            _transport_update_listeners.push_back(listener);
            break;
        case control::NotificationType::CPU_TIMING_UPDATE:
            _cpu_timing_update_listeners.push_back(listener);
            break;
        case control::NotificationType::ASYNC_COMMAND_COMPLETION:
            _command_completion_listeners.push_back(listener);
            break;
        default:
            break;
    }

    return control::ControlStatus::OK;
}

void Controller::completion_callback(void*arg, Event*event, int status)
{
    reinterpret_cast<Controller*>(arg)->_completion_callback(event, status);
}

int Controller::process(Event* event)
{
    if (event->is_parameter_change_notification())
    {
        _notify_parameter_listeners(event);
    }
    else if (event->is_property_change_notification())
    {
        _notify_property_listeners(event);
    }
    else if (event->is_engine_notification())
    {
        _handle_engine_notifications(static_cast<const EngineNotificationEvent*>(event));
    }

    return EventStatus::NOT_HANDLED;
}

void Controller::_handle_engine_notifications(const EngineNotificationEvent* event)
{
    if (event->is_audio_graph_notification())
    {
        _handle_audio_graph_notifications(static_cast<const AudioGraphNotificationEvent*>(event));

    }
    else if (event->is_tempo_notification())
    {
        auto typed_event = static_cast<const TempoNotificationEvent*>(event);
        _notify_transport_listeners(control::TransportNotification(
            control::TransportAction::TEMPO_CHANGED,
                                                               typed_event->tempo(),
                                                               typed_event->time()));
    }
    else if (event->is_time_sign_notification())
    {
        auto typed_event = static_cast<const TimeSignatureNotificationEvent*>(event);
        _notify_transport_listeners(
            control::TransportNotification(control::TransportAction::TIME_SIGNATURE_CHANGED,
                                                               to_external(typed_event->time_signature()),
                                                               typed_event->time()));
    }
    else if (event->is_playing_mode_notification())
    {
        auto typed_event = static_cast<const PlayingModeNotificationEvent*>(event);
        _notify_transport_listeners(
            control::TransportNotification(control::TransportAction::PLAYING_MODE_CHANGED,
                                                               to_external(typed_event->mode()),
                                                               typed_event->time()));
    }
    else if (event->is_sync_mode_notification())
    {
        auto typed_event = static_cast<const SyncModeNotificationEvent*>(event);
        _notify_transport_listeners(
            control::TransportNotification(control::TransportAction::SYNC_MODE_CHANGED,
                                                               to_external(typed_event->mode()),
                                                               typed_event->time()));
    }
    else if (event->is_timing_notification())
    {
        auto typed_event = static_cast<const EngineTimingNotificationEvent*>(event);
        _notify_timing_listeners(typed_event);
    }
}

void Controller::_handle_audio_graph_notifications(const AudioGraphNotificationEvent* event)
{
    switch (event->action())
    {
        case AudioGraphNotificationEvent::Action::PROCESSOR_ADDED_TO_TRACK:
        {
            _notify_processor_listeners(event, control::ProcessorAction::ADDED);
            break;
        }
        case AudioGraphNotificationEvent::Action::PROCESSOR_REMOVED_FROM_TRACK:
        {
            _editor_controller_impl.close_editor(static_cast<int>(event->processor()));
            _notify_processor_listeners(event, control::ProcessorAction::DELETED);
            break;
        }
        case AudioGraphNotificationEvent::Action::PROCESSOR_LAYOUT_CHANGED:
        {
            _editor_controller_impl.close_editor(static_cast<int>(event->processor()));
            _notify_processor_listeners(event, control::ProcessorAction::UPDATED);
            break;
        }
        case AudioGraphNotificationEvent::Action::TRACK_CREATED:
        {
            _notify_track_listeners(event, control::TrackAction::ADDED);
            break;
        }
        case AudioGraphNotificationEvent::Action::TRACK_DELETED:
        {
            _notify_track_listeners(event, control::TrackAction::DELETED);
            break;
        }
        default:
            // External listeners are only notified once processors are added to a track
            break;
    }
}

void Controller::_notify_parameter_listeners(const Event* event) const
{
    auto typed_event = static_cast<const ParameterChangeNotificationEvent*>(event);
    control::ParameterChangeNotification notification(static_cast<int>(typed_event->processor_id()),
                                                      static_cast<int>(typed_event->parameter_id()),
                                                      typed_event->normalized_value(),
                                                      typed_event->domain_value(),
                                                      typed_event->formatted_value(),
                                                      typed_event->time());
    for (auto& listener : _parameter_change_listeners)
    {
        listener->notification(&notification);
    }
}

void Controller::_notify_property_listeners(const Event* event) const
{
    auto typed_event = static_cast<const PropertyChangeNotificationEvent*>(event);
    control::PropertyChangeNotification notification(static_cast<int>(typed_event->processor_id()),
                                                     static_cast<int>(typed_event->property_id()),
                                                     typed_event->value(),
                                                     typed_event->time());
    for (auto& listener : _property_change_listeners)
    {
        listener->notification(&notification);
    }
}

void Controller::_notify_track_listeners(const AudioGraphNotificationEvent* typed_event,
                                          control::TrackAction action) const
{
    control::TrackNotification notification(action,
                                        typed_event->track(),
                                        typed_event->time());
    for (auto& listener : _track_update_listeners)
    {
        listener->notification(&notification);
    }
}

void Controller::_notify_transport_listeners(const control::TransportNotification& notification) const
{
    for (auto& listener : _transport_update_listeners)
    {
        listener->notification(&notification);
    }
}

void Controller::_notify_processor_listeners(const AudioGraphNotificationEvent* typed_event,
                                             control::ProcessorAction action) const
{
    control::ProcessorNotification notification(action,
                                                static_cast<int>(typed_event->processor()),
                                                static_cast<int>(typed_event->track()),
                                                typed_event->time());
    for (auto& listener : _processor_update_listeners)
    {
        listener->notification(&notification);
    }
}

void Controller::_completion_callback([[maybe_unused]] Event* event, int status)
{
    if (status == EventStatus::HANDLED_OK)
    {
        ELKLOG_LOG_DEBUG("Event {}, handled OK", event->id());
    }
    else
    {
        ELKLOG_LOG_WARNING("Event {} returned with error code: ", event->id(), status);
    }
    _notify_command_completion_listeners(event, to_external_status(status));
}

void Controller::set_osc_frontend(control_frontend::OSCFrontend* osc_frontend)
{
    _osc_controller_impl.set_osc_frontend(osc_frontend);
    _session_controller_impl.set_osc_frontend(osc_frontend);
}

void Controller::_notify_timing_listeners(const EngineTimingNotificationEvent* event) const
{
    control::CpuTimings ext;
    ext.main = to_external(event->main_timings());
    ext.threads.reserve(event->thread_timings().size());

    for (const auto& thread : event->thread_timings())
    {
        ext.threads.push_back(to_external(thread));
    }

    control::CpuTimingNotification notification(std::move(ext), event->time());

    for (auto& listener : _cpu_timing_update_listeners)
    {
        listener->notification(&notification);
    }
}
void Controller::_notify_command_completion_listeners(const Event* event, control::ControlStatus status) const
{
    control::CommandCompletionNotification notification(status, event->id(), event->time());
    for (auto& listener : _command_completion_listeners)
    {
        listener->notification(&notification);
    }
}
int Controller::send_with_completion_notification(std::unique_ptr<Event> event)
{
    event->set_completion_cb(completion_callback, this);
    int event_id = event->id();
    _event_dispatcher->post_event(std::move(event));
    return event_id;
}

} // end namespace sushi::internal::engine
