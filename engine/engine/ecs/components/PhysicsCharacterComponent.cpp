#include "PhysicsCharacterComponent.h"

namespace Carrot::ECS {
    PhysicsCharacterComponent::PhysicsCharacterComponent(Carrot::ECS::Entity entity)
            : Carrot::ECS::IdentifiableComponent<PhysicsCharacterComponent>(std::move(entity)) {

    }

    PhysicsCharacterComponent::PhysicsCharacterComponent(const rapidjson::Value& json, Carrot::ECS::Entity entity)
            : PhysicsCharacterComponent(std::move(entity)) {
        if(json.HasMember("mass")) {
            character.setMass(json["mass"].GetFloat());
        }
        if(json.HasMember("collider")) {
            auto pCollider = Physics::Collider::loadFromJSON(json["collider"]);
            character.setCollider(std::move(*pCollider));
        }
    }

    rapidjson::Value PhysicsCharacterComponent::toJSON(rapidjson::Document& doc) const {
        rapidjson::Value obj(rapidjson::kObjectType);
        obj.AddMember("mass", character.getMass(), doc.GetAllocator());
        obj.AddMember("collider", character.getCollider().toJSON(doc.GetAllocator()), doc.GetAllocator());
        return obj;
    }

    std::unique_ptr <Carrot::ECS::Component> PhysicsCharacterComponent::duplicate(const Carrot::ECS::Entity& newOwner) const {
        auto result = std::make_unique<PhysicsCharacterComponent>(newOwner);
        result->character = character;
        return result;
    }

} // Carrot::ECS