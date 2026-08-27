#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <glaze/json.hpp>

namespace liminal::provider {

// Provider-neutral agent vocabulary only. Concrete clients intentionally keep
// their own history and wire types so provider-specific APIs remain available.

struct DiscoveredModel {
    std::string id;
    std::string name;
};

struct ToolCall {
    std::string id;
    std::string name;
    glz::generic input;
};

struct SchemaProperty {
    std::string type;
    std::string description;
};

struct InputSchema {
    std::string type = "object";
    std::map<std::string, SchemaProperty> properties;
    std::vector<std::string> required;
    bool additional_properties = false;
};

enum struct ToolKind {
    FUNCTION,
    WEB_SEARCH,
    WEB_FETCH,
};

struct ToolDefinition {
    ToolKind kind = ToolKind::FUNCTION;
    std::string name;
    std::string description;
    InputSchema input_schema;
};

} // namespace liminal::provider

template <>
struct glz::meta<liminal::provider::InputSchema> {
    using T = liminal::provider::InputSchema;
    static constexpr auto value =
        object("type", &T::type, "properties", &T::properties, "required", &T::required, "additionalProperties", &T::additional_properties);
};
