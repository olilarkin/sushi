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
 * @brief Factory for CLAP processors.
 */

#ifndef SUSHI_CLAP_PROCESSOR_FACTORY_H
#define SUSHI_CLAP_PROCESSOR_FACTORY_H

#include <memory>

#include "library/base_processor_factory.h"

namespace sushi::internal::clap_wrapper {

class ClapHostContext;

class ClapProcessorFactory : public BaseProcessorFactory
{
public:
    ClapProcessorFactory();
    ~ClapProcessorFactory() override;

    std::pair<ProcessorReturnCode, std::shared_ptr<Processor>> new_instance(const PluginInfo& plugin_info,
                                                                            HostControl& host_control,
                                                                            float sample_rate) override;
private:
    std::unique_ptr<ClapHostContext> _host_context;
};

} // end namespace sushi::internal::clap_wrapper

#endif // SUSHI_CLAP_PROCESSOR_FACTORY_H
