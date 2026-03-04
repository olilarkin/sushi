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

/**
 * @brief Factory implementation for CLAP processors.
 */

#include "elklog/static_logger.h"

#include "clap_processor_factory.h"

#ifdef SUSHI_BUILD_WITH_CLAP
#include "clap_host_context.h"
#include "clap_wrapper.h"
#endif

#include "elk-warning-suppressor/warning_suppressor.hpp"

ELK_PUSH_WARNING
ELK_DISABLE_UNUSED_CONST_VARIABLE
ELKLOG_GET_LOGGER_WITH_MODULE_NAME("Clap");
ELK_POP_WARNING

namespace sushi::internal::clap_wrapper {

ClapProcessorFactory::~ClapProcessorFactory() = default;

#ifdef SUSHI_BUILD_WITH_CLAP

ClapProcessorFactory::ClapProcessorFactory() : _host_context(std::make_unique<ClapHostContext>())
{}

std::pair<ProcessorReturnCode, std::shared_ptr<Processor>> ClapProcessorFactory::new_instance(const PluginInfo& plugin_info,
                                                                                               HostControl& host_control,
                                                                                               float sample_rate)
{
    int plugin_index = 0;
    if (!plugin_info.uid.empty())
    {
        try
        {
            plugin_index = std::stoi(plugin_info.uid);
        }
        catch (...)
        {
            ELKLOG_LOG_WARNING("Could not parse CLAP plugin uid '{}' as index, using 0", plugin_info.uid);
        }
    }

    auto processor = std::make_shared<ClapWrapper>(host_control,
                                                    plugin_info.path,
                                                    plugin_index,
                                                    _host_context.get());
    auto processor_status = processor->init(sample_rate);
    return {processor_status, processor};
}

#else // SUSHI_BUILD_WITH_CLAP

class ClapHostContext {};

ClapProcessorFactory::ClapProcessorFactory() = default;

std::pair<ProcessorReturnCode, std::shared_ptr<Processor>> ClapProcessorFactory::new_instance([[maybe_unused]] const PluginInfo& plugin_info,
                                                                                               [[maybe_unused]] HostControl& host_control,
                                                                                               [[maybe_unused]] float sample_rate)
{
    ELKLOG_LOG_ERROR("Sushi was not built with support for CLAP plugins");
    return {ProcessorReturnCode::UNSUPPORTED_OPERATION, nullptr};
}

#endif // SUSHI_BUILD_WITH_CLAP

} // end namespace sushi::internal::clap_wrapper
