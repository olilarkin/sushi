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
 * @brief VST 3.x  plugin loader
 * @Copyright 2017-2025 Elk Audio AB, Stockholm
 */
#include "elk-warning-suppressor/warning_suppressor.hpp"

ELK_PUSH_WARNING
ELK_DISABLE_EXTRA
ELK_DISABLE_DEPRECATED_DECLARATIONS
ELK_DISABLE_SHORTEN_64_TO_32

#include "pluginterfaces/base/ustring.h"
#include "public.sdk/source/vst/utility/stringconvert.h"
#include "base/source/fobject.h"

ELK_POP_WARNING

#include "elklog/static_logger.h"

#include "vst3x_host_app.h"
#include "vst3x_wrapper.h"
#include "vst3x_utils.h"


// Define the UUIDs for extensions:
DEF_CLASS_IID (elk::IElkHostExtension)
DEF_CLASS_IID (elk::IElkComponentHandlerExtension)
DEF_CLASS_IID (elk::IElkControllerExtension)
DEF_CLASS_IID (elk::IElkProcessorExtension)

namespace sushi::internal::vst3 {

ELKLOG_GET_LOGGER_WITH_MODULE_NAME("vst3");

constexpr char HOST_NAME[] = "Sushi";

Steinberg::tresult SushiHostApplication::queryInterface(const char* iid, void** obj)
{
    DEF_INTERFACE (elk::IElkHostExtension);
    return HostApplication::queryInterface(iid, obj);
}

Steinberg::tresult SushiHostApplication::getName(Steinberg::Vst::String128 name)

{
    Steinberg::UString128 str(HOST_NAME);
    str.copyTo (name, 128);
    return Steinberg::kResultOk;
}

Steinberg::tresult SushiHostApplication::requestAsyncWork(elk::AsyncWorkCallback callback, void* data, Steinberg::int32& requestId)
{
    if (!callback || !twine::is_current_thread_realtime())
    {
        ELKLOG_LOG_WARNING_IF(!twine::is_current_thread_realtime(), "requestAsyncWork called from outside the audio thread");
        return Steinberg::kInvalidArgument;
    }

    auto id = _wrapper_instance->request_async_work(callback, data);
    requestId = id;
    return Steinberg::kResultOk;
}

ComponentHandler::ComponentHandler(Vst3xWrapper* wrapper_instance,
                                   HostControl* host_control) : _wrapper_instance(wrapper_instance),
                                                                _host_control(host_control)
{}

Steinberg::tresult ComponentHandler::queryInterface(const char* iid, void** obj)
{
    DEF_INTERFACE (elk::IElkComponentHandlerExtension);
    DEF_INTERFACE (Steinberg::Vst::IComponentHandler);
    return Steinberg::kResultFalse;
}

Steinberg::tresult ComponentHandler::performEdit(Steinberg::Vst::ParamID parameter_id,
                                                 Steinberg::Vst::ParamValue normalized_value)
{
    ELKLOG_LOG_DEBUG("performEdit called, param: {} value: {}", parameter_id, normalized_value);
    _wrapper_instance->set_parameter_change(ObjectId(parameter_id), static_cast<float>(normalized_value));
    return Steinberg::kResultOk;
}

Steinberg::tresult ComponentHandler::restartComponent(Steinberg::int32 flags)
{
    ELKLOG_LOG_DEBUG("restartComponent called");
    if (flags | (Steinberg::Vst::kParamValuesChanged & Steinberg::Vst::kReloadComponent))
    {
        auto event = std::make_unique<AudioGraphNotificationEvent>(AudioGraphNotificationEvent::Action::PROCESSOR_UPDATED,
                                                                   _wrapper_instance->id(), 0, IMMEDIATE_PROCESS);
        _host_control->post_event(std::move(event));
        return Steinberg::kResultOk;
    }
    return Steinberg::kResultFalse;
}

Steinberg::tresult ComponentHandler::notifyPropertyValueChange(Steinberg::int32 propertyId, const elk::PropertyValue& value)
{
    if (twine::is_current_thread_realtime())
    {
        ELKLOG_LOG_WARNING("notifyPropertyValueChange() called from a realtime thread");
        return Steinberg::kResultFalse;
    }
    _host_control->post_event(std::make_unique<PropertyChangeNotificationEvent>(_wrapper_instance->id(),
                                                                                propertyId,
                                                                                std::string(value.value, value.length),
                                                                                IMMEDIATE_PROCESS));
    return Steinberg::kResultOk;
}

/* ConnectionProxy is more or less ripped straight out of Steinberg example code.
 * But edited to follow Elk coding style. See plugprovider.cpp. */
class ConnectionProxy : public Steinberg::FObject, public Steinberg::Vst::IConnectionPoint
{
public:
    SUSHI_DECLARE_NON_COPYABLE(ConnectionProxy);

    explicit ConnectionProxy(Steinberg::Vst::IConnectionPoint* src_connection) : _source_connection(src_connection) {}
    ~ConnectionProxy() override = default;

    Steinberg::tresult connect(Steinberg::Vst::IConnectionPoint* other) override;
    Steinberg::tresult disconnect(Steinberg::Vst::IConnectionPoint* other) override;
    Steinberg::tresult notify(Steinberg::Vst::IMessage* message) override;

    bool disconnect();

    OBJ_METHODS(ConnectionProxy, FObject)
    REFCOUNT_METHODS(FObject)
    DEF_INTERFACES_1(IConnectionPoint, FObject)

protected:
    Steinberg::IPtr<IConnectionPoint> _source_connection;
    Steinberg::IPtr<IConnectionPoint> _dest_connection;
};

Steinberg::tresult PLUGIN_API ConnectionProxy::connect(IConnectionPoint* other)
{
    if (other == nullptr)
    {
        return Steinberg::kInvalidArgument;
    }

    if (_dest_connection)
    {
        return Steinberg::kResultFalse;
    }

    _dest_connection = other;
    Steinberg::tresult res = _source_connection->connect(this);
    if (res != Steinberg::kResultTrue)
    {
        _dest_connection = nullptr;
    }
    return res;
}

Steinberg::tresult PLUGIN_API ConnectionProxy::disconnect(IConnectionPoint* other)
{
    if (other == nullptr)
    {
        return Steinberg::kInvalidArgument;
    }

    if (other == _dest_connection)
    {
        if (_source_connection)
        {
            _source_connection->disconnect(this);
        }
        _dest_connection = nullptr;
        return Steinberg::kResultTrue;
    }
    return Steinberg::kInvalidArgument;
}

Steinberg::tresult PLUGIN_API ConnectionProxy::notify(Steinberg::Vst::IMessage* message)
{
    if (_dest_connection)
    {
        return _dest_connection->notify(message);
    }
    return Steinberg::kResultFalse;
}

bool ConnectionProxy::disconnect()
{
    auto status =  disconnect(_dest_connection);
    return  status == Steinberg::kResultTrue;
}

PluginInstance::PluginInstance(SushiHostApplication* host_app): _host_app(host_app)
{}

PluginInstance::~PluginInstance()
{
    if (_component_connection)
    {
        _component_connection->disconnect();
    }
    if (_controller_connection)
    {
        _controller_connection->disconnect();
    }
}

bool PluginInstance::load_plugin(const std::string& plugin_path, const std::string& plugin_name, Steinberg::Vst::IComponentHandler* component_handler)
{
    std::string error_msg;
    _module = VST3::Hosting::Module::create(plugin_path, error_msg);
    if (!_module)
    {
        ELKLOG_LOG_ERROR("Failed to load VST3 Module: {}", error_msg);
        return false;
    }
    auto factory = _module->getFactory().get();
    if (!factory)
    {
        ELKLOG_LOG_ERROR("Failed to get PluginFactory, plugin is probably broken");
        return false;
    }
    Steinberg::PFactoryInfo info;
    if (factory->getFactoryInfo(&info) != Steinberg::kResultOk)
    {
        return false;
    }
    // In the future we might want to check for more things than just vendor name here
    _vendor = info.vendor;

    auto component = load_component(factory, plugin_name);
    if (!component)
    {
        return false;
    }

    auto res = component->initialize(static_cast<Steinberg::Vst::IHostApplication*>(_host_app));
    if (res != Steinberg::kResultOk)
    {
        ELKLOG_LOG_ERROR("Failed to initialize component with error code: {}", res);
        return false;
    }

    auto processor = query_vst_interface<Steinberg::Vst::IAudioProcessor>(component);
    if (!processor)
    {
        ELKLOG_LOG_ERROR("Failed to get processor from component");
        return false;
    }

    auto controller = load_controller(factory, component);
    if (!controller)
    {
        ELKLOG_LOG_ERROR("Failed to load controller");
        return false;
    }

    // Only initialize the controller if it's a separate object from the component.
    // Single-component plugins (IPlug2, JUCE) implement both IComponent and
    // IEditController on the same object, which is already initialized above.
    Steinberg::Vst::IEditController* component_as_controller = nullptr;
    bool separate_controller = true;
    if (component->queryInterface(Steinberg::Vst::IEditController::iid,
                                  reinterpret_cast<void**>(&component_as_controller)) == Steinberg::kResultOk)
    {
        component_as_controller->release();
        separate_controller = false;
    }

    if (separate_controller)
    {
        res = controller->initialize(static_cast<Steinberg::Vst::IHostApplication*>(_host_app));
        if (res != Steinberg::kResultOk)
        {
            ELKLOG_LOG_ERROR("Failed to initialize controller with error code: {}", res);
            return false;
        }
    }

    res = controller->setComponentHandler(component_handler);
    if (res != Steinberg::kResultOk)
    {
        ELKLOG_LOG_ERROR("Failed to set component handler with error code: {}", res);
        return false;
    }

    _component = component;
    _processor = processor;
    _controller = controller;
    _name = plugin_name;

    if (!_connect_components())
    {
        ELKLOG_LOG_ERROR("Failed to connect component to editor");
    }
    _query_extension_interfaces();

    return true;
}

bool PluginInstance::load_plugin_from_component(Steinberg::Vst::IComponent* component,
                                                const std::string& plugin_name,
                                                Steinberg::Vst::IComponentHandler* component_handler)
{
    assert(component);
    auto res = component->initialize(static_cast<Steinberg::Vst::IHostApplication*>(_host_app));
    if (res != Steinberg::kResultOk)
    {
        ELKLOG_LOG_ERROR("Failed to initialize component with error code: {}", res);
        return false;
    }
    auto processor = query_vst_interface<Steinberg::Vst::IAudioProcessor>(component);
    if (!processor)
    {
        ELKLOG_LOG_ERROR("Failed to get processor from component");
        return false;
    }
    auto controller = query_vst_interface<Steinberg::Vst::IEditController>(component);
    if (!controller)
    {
        ELKLOG_LOG_ERROR("Failed to get controller from component");
        return false;
    }
    res = controller->setComponentHandler(component_handler);
    if (res != Steinberg::kResultOk)
    {
        ELKLOG_LOG_ERROR("Failed to set component handler with error code: {}", res);
        return false;
    }

    _component = component;
    _processor = processor;
    _controller = controller;
    _name = plugin_name;

    _query_extension_interfaces();

    return true;
}

void PluginInstance::_query_extension_interfaces()
{
    auto midi_mapper = query_vst_interface<Steinberg::Vst::IMidiMapping>(_controller.get());
    if (midi_mapper)
    {
        _midi_mapper = midi_mapper;
        ELKLOG_LOG_INFO("Plugin supports Midi Mapping interface");
    }

    auto unit_info = query_vst_interface<Steinberg::Vst::IUnitInfo>(_controller.get());
    if (unit_info)
    {
        _unit_info = unit_info;
        ELKLOG_LOG_INFO("Plugin supports Unit Info interface for programs");
    }

    auto processor_extension = query_vst_interface<::elk::IElkProcessorExtension>(_processor.get());
    if (processor_extension)
    {
        _elk_processor_extension = processor_extension;
        ELKLOG_LOG_INFO("Plugin supports Elk Processor Extension");
    }

    auto controller_extension = query_vst_interface<elk::IElkControllerExtension>(_controller.get());
    if (controller_extension)
    {
        _elk_controller_extension = controller_extension;
        ELKLOG_LOG_INFO("Plugin supports Elk Controller Extension");
    }
}

bool PluginInstance::_connect_components()
{
    Steinberg::FUnknownPtr<Steinberg::Vst::IConnectionPoint> component_connection(_component);
    Steinberg::FUnknownPtr<Steinberg::Vst::IConnectionPoint> controller_connection(_controller);

    if (!component_connection || !controller_connection)
    {
        ELKLOG_LOG_ERROR("Failed to create connection points");
        return false;
    }

    _component_connection = NEW ConnectionProxy(component_connection);
    _controller_connection = NEW ConnectionProxy(controller_connection);

    if (_component_connection->connect(controller_connection) != Steinberg::kResultTrue)
    {
        ELKLOG_LOG_ERROR("Failed to connect component");
    }
    else
    {
        if (_controller_connection->connect(component_connection) != Steinberg::kResultTrue)
        {
            ELKLOG_LOG_ERROR("Failed to connect controller");
        }
        else
        {
            return true;
        }
    }
    return false;
}

bool PluginInstance::notify_controller(Steinberg::Vst::IMessage*message)
{
    // This calls notify() on the component connection proxy, which is has the controller
    // connected as its destination. So it is the controller being notified
    auto res = _component_connection->notify(message);
    if (res == Steinberg::kResultOk || res == Steinberg::kResultFalse)
    {
        return true;
    }
    return false;
}

bool PluginInstance::notify_processor(Steinberg::Vst::IMessage*message)
{
    auto res = _controller_connection->notify(message);
    if (res == Steinberg::kResultOk || res == Steinberg::kResultFalse)
    {
        return true;
    }
    return false;
}

Steinberg::Vst::IComponent* load_component(Steinberg::IPluginFactory* factory,
                                           const std::string& plugin_name)
{
    for (int i = 0; i < factory->countClasses(); ++i)
    {
        Steinberg::PClassInfo info;
        factory->getClassInfo(i, &info);
        ELKLOG_LOG_INFO("Querying plugin {} of type {}", info.name, info.category);
        if (info.name == plugin_name)
        {
            Steinberg::Vst::IComponent* component;
            auto res = factory->createInstance(info.cid, Steinberg::Vst::IComponent::iid,
                                               reinterpret_cast<void**>(&component));
            if (res == Steinberg::kResultOk)
            {
                ELKLOG_LOG_INFO("Creating plugin {}", info.name);
                return component;
            }
            ELKLOG_LOG_ERROR("Failed to create component with error code: {}", res);
            return nullptr;
        }
    }
    ELKLOG_LOG_ERROR("No match for plugin {} in factory", plugin_name);
    return nullptr;
}

Steinberg::Vst::IEditController* load_controller(Steinberg::IPluginFactory* factory,
                                                  Steinberg::Vst::IComponent* component)
{
    /* The controller can be implemented both as a part of the component (if inhering from
     * SingleComponentEffect) or as a separate object, Steinberg recommends the latter,
     * JUCE does the former in their plugin adaptor. */

    auto controller = query_vst_interface<Steinberg::Vst::IEditController>(component);
    if (controller)
    {
        return controller;
    }
    // Else try to instantiate the controller as a separate object.
    Steinberg::TUID controllerTUID;
    if (component->getControllerClassId(controllerTUID) == Steinberg::kResultTrue)
    {
        Steinberg::FUID controllerID(controllerTUID);
        if (controllerID.isValid())
        {
            auto res = factory->createInstance(controllerID, Steinberg::Vst::IEditController::iid,
                                               reinterpret_cast<void**>(&controller));
            if (res == Steinberg::kResultOk)
            {
                return controller;
            }
            ELKLOG_LOG_ERROR("Failed to create controller with error code: {}", res);
        }
    }
    return nullptr;
}

} // end namespace sushi::internal::vst3
