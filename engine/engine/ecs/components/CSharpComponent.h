#pragma once

#include <engine/ecs/components/Component.h>
#include <core/scripting/csharp/forward.h>
#include <core/scripting/csharp/CSObject.h>
#include <engine/scripting/CSharpBindings.h>

namespace Carrot::ECS {

    class CSharpComponent : public Carrot::ECS::Component {
    public:
        explicit CSharpComponent(Carrot::ECS::Entity entity, const std::string& namespaceName, const std::string& className);

        explicit CSharpComponent(const rapidjson::Value& json, Carrot::ECS::Entity entity, const std::string& namespaceName, const std::string& className);

        ~CSharpComponent();

        rapidjson::Value toJSON(rapidjson::Document& doc) const override;

        const char *const getName() const override;

        std::unique_ptr <Carrot::ECS::Component> duplicate(const Carrot::ECS::Entity& newOwner) const override;

        void drawInspectorInternals(const Carrot::Render::Context& renderContext, bool& modified) override;

        ComponentID getComponentTypeID() const override;

    public:
        /**
         * Returns the C# sharp of this component
         */
        Scripting::CSObject& getCSComponentObject();

    private:
        void init();

        void refresh();
        void onAssemblyLoad();
        void onAssemblyUnload();

    private:
        bool drawIntProperty(Scripting::ComponentProperty& property);
        bool drawFloatProperty(Scripting::ComponentProperty& property);
        bool drawBooleanProperty(Scripting::ComponentProperty& property);
        bool drawEntityProperty(Scripting::ComponentProperty& property);
        bool drawUserDefinedProperty(Scripting::ComponentProperty& property);

    private:
        bool foundInAssemblies = false;
        std::string namespaceName;
        std::string className;
        std::string fullName;

        Carrot::ComponentID componentID = -1;
        std::shared_ptr<Scripting::CSObject> csComponent;
        std::vector<Scripting::ComponentProperty> componentProperties;
        Scripting::CSharpBindings::Callbacks::Handle loadCallbackHandle;
        Scripting::CSharpBindings::Callbacks::Handle unloadCallbackHandle;

        mutable rapidjson::Document serializedDoc{};
        mutable rapidjson::Value serializedVersion{}; // always keep the serialized version in case we can't load the component from C#
    };
}