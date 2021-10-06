//
// Created by jglrxavpok on 06/03/2021.
//

#pragma once

#include "Component.h"
#include <engine/render/lighting/Lights.h>

namespace Carrot::ECS {
    struct RaycastedShadowsLight: public IdentifiableComponent<RaycastedShadowsLight> {
        Light& lightRef;

        explicit RaycastedShadowsLight(Entity entity, Light& light): IdentifiableComponent<RaycastedShadowsLight>(std::move(entity)), lightRef(light) {
            light.enabled = true;
        };

        /* not serialisable for the moment
        explicit RaycastedShadowsLight(const rapidjson::Value& json, Entity entity);

        rapidjson::Value toJSON(rapidjson::Document& doc) const override;
         */

        const char *const getName() const override {
            return "RaycastedShadowsLight";
        }

        std::unique_ptr<Component> duplicate(const Entity& newOwner) const override {
            auto result = std::make_unique<RaycastedShadowsLight>(newOwner, lightRef);
            return result;
        }
    };
}

template<>
inline const char* Carrot::Identifiable<Carrot::ECS::RaycastedShadowsLight>::getStringRepresentation() {
    return "RaycastedShadowsLight";
}