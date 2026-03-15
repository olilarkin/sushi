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

#ifndef SUSHI_FAUST_PROCESSOR_FACTORY_H
#define SUSHI_FAUST_PROCESSOR_FACTORY_H

#include "library/base_processor_factory.h"

namespace sushi::internal::faust_wrapper {

class FaustProcessorFactory : public BaseProcessorFactory
{
public:
    ~FaustProcessorFactory() override = default;

    std::pair<ProcessorReturnCode, std::shared_ptr<Processor>> new_instance(const PluginInfo& plugin_info,
                                                                            HostControl& host_control,
                                                                            float sample_rate) override;
};

} // namespace sushi::internal::faust_wrapper

#endif // SUSHI_FAUST_PROCESSOR_FACTORY_H
