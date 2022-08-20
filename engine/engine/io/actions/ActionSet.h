//
// Created by jglrxavpok on 25/07/2021.
//

#pragma once

#include <string>
#include <string_view>
#include <vector>
#include "Action.hpp"
#include "core/utils/UUID.h"

#include "engine/vr/includes.h"

namespace Carrot {
    class Engine;
}

namespace Carrot::IO {
    /// Represents a group of Action which can be toggled on or off at once.
    class ActionSet {
    public:
        explicit ActionSet(std::string_view name, bool isXRSet = false);
        ~ActionSet();

        bool isActive() const { return active; }
        const std::string& getName() const { return name; }

        void activate();
        void deactivate();

        void add(BoolInputAction& input) {
            boolInputs.push_back(&input);
        }

        void add(FloatInputAction& input) {
            floatInputs.push_back(&input);
        }

        void add(Vec2InputAction& input) {
            vec2Inputs.push_back(&input);
        }

    public:
        const std::vector<BoolInputAction*>& getBoolInputs() const { return boolInputs; }
        const std::vector<FloatInputAction*>& getFloatInputs() const { return floatInputs; }
        const std::vector<Vec2InputAction*>& getVec2Inputs() const { return vec2Inputs; }

    public:
        static void updatePrePollAllSets(Carrot::Engine& engine);
        static void resetAllDeltas();
        static std::vector<ActionSet*>& getSetList();

    private:
        // TODO: actually allow remapping, for the moment only use suggested binding
        const std::vector<std::string>& getMappedBindings(auto& action) {
            return action->getSuggestedBindings();
        };

        void updatePrePoll();
        void resetDeltas();
        void prepareForUse(Carrot::Engine& engine);

    private:

        bool readyForUse = false;
        bool isXRSet = false;
        bool active = false;
        std::string name;
        std::vector<FloatInputAction*> floatInputs;
        std::vector<BoolInputAction*> boolInputs;
        std::vector<Vec2InputAction*> vec2Inputs;

        Carrot::UUID keyCallback;
        Carrot::UUID gamepadButtonCallback;
        Carrot::UUID gamepadAxisCallback;
        Carrot::UUID gamepadVec2Callback;
        Carrot::UUID keysVec2Callback;
        Carrot::UUID mouseButtonCallback;
        Carrot::UUID mousePositionCallback;
        Carrot::UUID mouseDeltaCallback;
        Carrot::UUID mouseDeltaGrabbedCallback;

    private: // OpenXR compatibility
        xr::UniqueActionSet xrActionSet;
    };
}