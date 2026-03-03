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
 * @Copyright 2017-2023 Elk Audio AB, Stockholm
 */

#ifndef SUSHI_VST3X_HOST_CONTEXT_H
#define SUSHI_VST3X_HOST_CONTEXT_H

#include "sushi/constants.h"

#include "library/id_generator.h"

#include "elk-warning-suppressor/warning_suppressor.hpp"

ELK_PUSH_WARNING
ELK_DISABLE_EXTRA
ELK_DISABLE_DEPRECATED_DECLARATIONS
ELK_DISABLE_SHORTEN_64_TO_32

#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstunits.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "base/source/fobject.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "elk_vst3_extensions/elk_vst3_extensions.h"

ELK_POP_WARNING

namespace sushi::internal {

class HostControl;

namespace vst3 {

class Vst3xWrapper;
class ConnectionProxy;

class SushiHostApplication : public Steinberg::Vst::HostApplication, public elk::IElkHostExtension
{
public:
    SUSHI_DECLARE_NON_COPYABLE(SushiHostApplication);

    SushiHostApplication(Vst3xWrapper* wrapper_instance) : Steinberg::Vst::HostApplication(),
                                                           _wrapper_instance(wrapper_instance) {}

    Steinberg::tresult PLUGIN_API queryInterface (const Steinberg::TUID iid, void** obj) override;

    Steinberg::tresult PLUGIN_API getName (Steinberg::Vst::String128 name) override;

    Steinberg::tresult PLUGIN_API requestAsyncWork(elk::AsyncWorkCallback callback, void* data, Steinberg::int32& requestId) override;

    // Refer addRef/release functions to the base class implementation
    REFCOUNT_METHODS(Steinberg::Vst::HostApplication);

private:
    Vst3xWrapper* _wrapper_instance;
};

class ComponentHandler : public Steinberg::Vst::IComponentHandler,  public elk::IElkComponentHandlerExtension, public Steinberg::FObject
{
public:
    SUSHI_DECLARE_NON_COPYABLE(ComponentHandler);

    explicit ComponentHandler(Vst3xWrapper* wrapper_instance, HostControl* host_control);

    Steinberg::tresult PLUGIN_API queryInterface (const Steinberg::TUID iid, void** obj) override;

    Steinberg::tresult PLUGIN_API beginEdit (Steinberg::Vst::ParamID /*id*/) override {return Steinberg::kResultOk;}
    Steinberg::tresult PLUGIN_API performEdit (Steinberg::Vst::ParamID parameter_id, Steinberg::Vst::ParamValue normalized_value) override;
    Steinberg::tresult PLUGIN_API endEdit (Steinberg::Vst::ParamID /*parameter_id*/) override {return Steinberg::kResultOk;}
    Steinberg::tresult PLUGIN_API restartComponent (Steinberg::int32 flags) override;

    // From elk::IComponentHandlerExtension
    Steinberg::tresult PLUGIN_API notifyPropertyValueChange(Steinberg::int32 propertyId, const elk::PropertyValue& value) override;

    // Refer addRef/release functions to the base class implementation
    REFCOUNT_METHODS(Steinberg::FObject);

private:
    Vst3xWrapper* _wrapper_instance;
    HostControl*  _host_control;
};

/**
 * @brief Container to hold plugin modules and manage their lifetimes
 */
class PluginInstance
{
public:
    SUSHI_DECLARE_NON_COPYABLE(PluginInstance);

    explicit PluginInstance(SushiHostApplication* host_app);
    ~PluginInstance();

    /**
     * @brief Load a plugin from file
     * @param plugin_path Path to the .vst file or folder
     * @param plugin_name Name of the plugin to load from the file, vst3 allows you to pack multiple plugins in one file/bundle
     * @return True if successful, false otherwise
     */
    bool load_plugin(const std::string& plugin_path, const std::string& plugin_name, Steinberg::Vst::IComponentHandler* component_handler);

    /**
     * @brief Load a plugin from an already instantiated component. This assumes the plugin is built from SingleComponentEffect
     * @param component The component to load from
     * @param plugin_name Name of the plugin to load
     * @return True if successful, false otherwise
     */
    bool load_plugin_from_component(Steinberg::Vst::IComponent* component, const std::string& plugin_name, Steinberg::Vst::IComponentHandler* component_handler);

    [[nodiscard]] const std::string& name() const {return _name;}
    [[nodiscard]] const std::string& vendor() const {return _vendor;}
    [[nodiscard]] Steinberg::Vst::IComponent* component() const {return _component.get();}
    [[nodiscard]] Steinberg::Vst::IAudioProcessor* processor() const {return _processor.get();}
    [[nodiscard]] Steinberg::Vst::IEditController* controller() const {return _controller.get();}
    [[nodiscard]] Steinberg::Vst::IUnitInfo* unit_info() {return _unit_info;}
    [[nodiscard]] Steinberg::Vst::IMidiMapping* midi_mapper() {return _midi_mapper;}
    [[nodiscard]] elk::IElkControllerExtension* controller_extension() {return _elk_controller_extension.get();}
    [[nodiscard]] elk::IElkProcessorExtension* processor_extension() {return _elk_processor_extension.get();}

    bool notify_controller(Steinberg::Vst::IMessage* message);
    bool notify_processor(Steinberg::Vst::IMessage* message);

private:
    void _query_extension_interfaces();
    bool _connect_components();

    std::string _name;
    std::string _vendor;

    SushiHostApplication* _host_app;

    std::shared_ptr<VST3::Hosting::Module> _module;

    // Reference counted pointers to plugin objects
    Steinberg::OPtr<Steinberg::Vst::IComponent>      _component{nullptr};
    Steinberg::OPtr<Steinberg::Vst::IAudioProcessor> _processor{nullptr};
    Steinberg::OPtr<Steinberg::Vst::IEditController> _controller{nullptr};

    // Abstract optional interfaces implemented by one of the objects above:
    Steinberg::OPtr<Steinberg::Vst::IMidiMapping>    _midi_mapper{nullptr};
    Steinberg::OPtr<Steinberg::Vst::IUnitInfo>       _unit_info{nullptr};

    // Optional Elk specific extension interfaces implemented by the plugin

    Steinberg::OPtr<elk::IElkProcessorExtension>     _elk_processor_extension{nullptr};
    Steinberg::OPtr<elk::IElkControllerExtension>    _elk_controller_extension{nullptr};

    Steinberg::OPtr<ConnectionProxy> _controller_connection;
    Steinberg::OPtr<ConnectionProxy> _component_connection;
};

Steinberg::Vst::IComponent* load_component(Steinberg::IPluginFactory* factory, const std::string& plugin_name);
Steinberg::Vst::IEditController* load_controller(Steinberg::IPluginFactory* factory, Steinberg::Vst::IComponent*);

} // end namespace vst
} // end namespace sushi::internal

#endif // SUSHI_VST3X_HOST_CONTEXT_H
