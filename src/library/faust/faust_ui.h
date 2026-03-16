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

#ifndef SUSHI_FAUST_UI_H
#define SUSHI_FAUST_UI_H

#include <string>
#include <vector>
#include <map>

#include "faust/gui/UI.h"
#include "faust/gui/PathBuilder.h"

namespace sushi::internal::faust_wrapper {

struct FaustParameterInfo
{
    std::string label;
    std::string full_path;
    FAUSTFLOAT* zone {nullptr};
    FAUSTFLOAT init {0};
    FAUSTFLOAT min {0};
    FAUSTFLOAT max {1};
    FAUSTFLOAT step {0.01f};
    bool is_button {false};
    bool is_bargraph {false};
    std::map<std::string, std::string> metadata;
};

class FaustUICollector : public UI, public PathBuilder
{
public:
    const std::vector<FaustParameterInfo>& parameters() const { return _parameters; }

    // -- widget's layouts
    void openTabBox(const char* label) override
    {
        pushLabel(label);
    }

    void openHorizontalBox(const char* label) override
    {
        pushLabel(label);
    }

    void openVerticalBox(const char* label) override
    {
        pushLabel(label);
    }

    void closeBox() override
    {
        popLabel();
    }

    // -- active widgets
    void addButton(const char* label, FAUSTFLOAT* zone) override
    {
        FaustParameterInfo info;
        info.label = label;
        info.full_path = buildPath(label);
        info.zone = zone;
        info.init = 0;
        info.min = 0;
        info.max = 1;
        info.step = 1;
        info.is_button = true;
        info.metadata = _pending_metadata;
        _pending_metadata.clear();
        _zone_index[zone] = _parameters.size();
        _parameters.push_back(std::move(info));
    }

    void addCheckButton(const char* label, FAUSTFLOAT* zone) override
    {
        addButton(label, zone);
    }

    void addVerticalSlider(const char* label, FAUSTFLOAT* zone,
                           FAUSTFLOAT init, FAUSTFLOAT min, FAUSTFLOAT max, FAUSTFLOAT step) override
    {
        _add_slider(label, zone, init, min, max, step);
    }

    void addHorizontalSlider(const char* label, FAUSTFLOAT* zone,
                             FAUSTFLOAT init, FAUSTFLOAT min, FAUSTFLOAT max, FAUSTFLOAT step) override
    {
        _add_slider(label, zone, init, min, max, step);
    }

    void addNumEntry(const char* label, FAUSTFLOAT* zone,
                     FAUSTFLOAT init, FAUSTFLOAT min, FAUSTFLOAT max, FAUSTFLOAT step) override
    {
        _add_slider(label, zone, init, min, max, step);
    }

    // -- passive widgets
    void addHorizontalBargraph(const char* label, FAUSTFLOAT* zone, FAUSTFLOAT min, FAUSTFLOAT max) override
    {
        FaustParameterInfo info;
        info.label = label;
        info.full_path = buildPath(label);
        info.zone = zone;
        info.min = min;
        info.max = max;
        info.is_bargraph = true;
        info.metadata = _pending_metadata;
        _pending_metadata.clear();
        _zone_index[zone] = _parameters.size();
        _parameters.push_back(std::move(info));
    }

    void addVerticalBargraph(const char* label, FAUSTFLOAT* zone, FAUSTFLOAT min, FAUSTFLOAT max) override
    {
        addHorizontalBargraph(label, zone, min, max);
    }

    // -- soundfiles
    void addSoundfile(const char* /*label*/, const char* /*filename*/, Soundfile** /*sf_zone*/) override {}

    // -- metadata
    void declare(FAUSTFLOAT* zone, const char* key, const char* val) override
    {
        if (zone == nullptr)
        {
            return;
        }
        _pending_metadata[key] = val;
    }

private:
    void _add_slider(const char* label, FAUSTFLOAT* zone,
                     FAUSTFLOAT init, FAUSTFLOAT min, FAUSTFLOAT max, FAUSTFLOAT step)
    {
        FaustParameterInfo info;
        info.label = label;
        info.full_path = buildPath(label);
        info.zone = zone;
        info.init = init;
        info.min = min;
        info.max = max;
        info.step = step;
        info.metadata = _pending_metadata;
        _pending_metadata.clear();
        _zone_index[zone] = _parameters.size();
        _parameters.push_back(std::move(info));
    }

    std::vector<FaustParameterInfo> _parameters;
    std::map<FAUSTFLOAT*, size_t> _zone_index;
    std::map<std::string, std::string> _pending_metadata;
};

} // namespace sushi::internal::faust_wrapper

#endif // SUSHI_FAUST_UI_H
