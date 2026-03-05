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

/**
 * @brief Factory implementation for AUv2 processors.
 */

#include "elklog/static_logger.h"

#include "auv2_processor_factory.h"

#ifdef SUSHI_BUILD_WITH_AUV2
#include "auv2_wrapper.h"
#endif

#include "elk-warning-suppressor/warning_suppressor.hpp"

ELK_PUSH_WARNING
ELK_DISABLE_UNUSED_CONST_VARIABLE
ELKLOG_GET_LOGGER_WITH_MODULE_NAME("AUv2");
ELK_POP_WARNING

namespace sushi::internal::auv2_wrapper {

#ifdef SUSHI_BUILD_WITH_AUV2

std::pair<ProcessorReturnCode, std::shared_ptr<Processor>> AUv2ProcessorFactory::new_instance(const PluginInfo& plugin_info,
                                                                                               HostControl& host_control,
                                                                                               float sample_rate)
{
    auto processor = std::make_shared<AUv2Wrapper>(host_control, plugin_info.uid);
    auto processor_status = processor->init(sample_rate);
    return {processor_status, processor};
}

#else // SUSHI_BUILD_WITH_AUV2

std::pair<ProcessorReturnCode, std::shared_ptr<Processor>> AUv2ProcessorFactory::new_instance([[maybe_unused]] const PluginInfo& plugin_info,
                                                                                               [[maybe_unused]] HostControl& host_control,
                                                                                               [[maybe_unused]] float sample_rate)
{
    ELKLOG_LOG_ERROR("Sushi was not built with support for AUv2 plugins");
    return {ProcessorReturnCode::UNSUPPORTED_OPERATION, nullptr};
}

#endif // SUSHI_BUILD_WITH_AUV2

} // end namespace sushi::internal::auv2_wrapper
