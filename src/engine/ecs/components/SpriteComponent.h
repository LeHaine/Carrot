//
// Created by jglrxavpok on 28/07/2021.
//

#pragma once
#include "engine/ecs/components/Component.h"
#include "engine/render/Sprite.h"

namespace Carrot::ECS {
    struct SpriteComponent: public IdentifiableComponent<SpriteComponent> {
        std::shared_ptr<Carrot::Render::Sprite> sprite;
        bool isTransparent = false;

        explicit SpriteComponent(Entity entity): IdentifiableComponent<SpriteComponent>(std::move(entity)) {}

        const char *const getName() const override {
            return "SpriteComponent";
        }
    };
}