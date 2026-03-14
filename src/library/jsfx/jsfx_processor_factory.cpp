/*
 * Copyright 2017-2023 Elk Audio AB
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

#include "jsfx_processor_factory.h"

#include "jsfx_wrapper.h"

namespace sushi::internal::jsfx_wrapper {

std::pair<ProcessorReturnCode, std::shared_ptr<Processor>>
JsfxProcessorFactory::new_instance(const PluginInfo& plugin_info,
                                   HostControl& host_control,
                                   float sample_rate)
{
#ifdef SUSHI_BUILD_WITH_JSFX
    auto processor = std::make_shared<JsfxWrapper>(host_control, plugin_info);
    auto status = processor->init(sample_rate);
    return {status, status == ProcessorReturnCode::OK ? std::static_pointer_cast<Processor>(processor) : nullptr};
#else
    (void) plugin_info;
    (void) host_control;
    (void) sample_rate;
    return {ProcessorReturnCode::PLUGIN_LOAD_ERROR, nullptr};
#endif
}

} // namespace sushi::internal::jsfx_wrapper
