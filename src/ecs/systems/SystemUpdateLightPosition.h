//
// Created by jglrxavpok on 06/03/2021.
//

#pragma once
#include "ecs/components/Component.h"
#include "ecs/components/Transform.h"
#include "ecs/components/RaycastedShadowsLight.h"
#include "System.h"

namespace Carrot {
    class SystemUpdateLightPosition: public LogicSystem<Transform, RaycastedShadowsLight> {
    public:
        explicit SystemUpdateLightPosition(Carrot::World& world): LogicSystem<Transform, RaycastedShadowsLight>(world) {}

        void tick(double dt) override;
    };
}


