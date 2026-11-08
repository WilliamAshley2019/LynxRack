#pragma once

//  for future dev use not explained yet as it is a future project element

#include <array>

#include "RackTypes.h"

namespace lynx
{
struct ModuleStatus
{
    bool playing = false;
    double bpm = 120.0;
    double positionTicks = 0.0;                  
    int activePattern = 0;
    std::array<float, maxChannels> peaks {};
};

struct ModuleCommand
{
    enum class Kind { none, play, stop, rewind, setPattern, setTempo, selectChannel };
    Kind kind = Kind::none;
    double value = 0.0;
};

class IModuleLink
{
public:
    virtual ~IModuleLink() = default;
    virtual void publishStatus (const ModuleStatus&) = 0;
    virtual bool takeCommand (ModuleCommand&) = 0;     
};

class NullModuleLink final : public IModuleLink
{
public:
    void publishStatus (const ModuleStatus&) override {}
    bool takeCommand (ModuleCommand&) override { return false; }
};
} // namespace lynx
