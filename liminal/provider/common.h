#pragma once

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <glaze/json.hpp>

namespace liminal::provider {

// Provider-neutral agent vocabulary only. Concrete clients intentionally keep
// their own history and wire types so provider-specific APIs remain available.

struct ToolCall {
    std::string id;
    std::string name;
    glz::generic input;
};

struct ToolResult {
    std::string call_id;
    std::string content;
    bool is_error = false;
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

struct ToolDefinition {
    std::string name;
    std::string description;
    InputSchema input_schema;
};

struct StreamCallbacks {
    /// Streamed text as it arrives; may be empty.
    std::function<void(std::string_view)> on_text_delta;
    /// A tool call is about to run (argument: tool name). UI feedback only.
    std::function<void(std::string_view)> on_tool_start;
};

} // namespace liminal::provider

template <>
struct glz::meta<liminal::provider::InputSchema> {
    using T = liminal::provider::InputSchema;
    static constexpr auto value =
        object("type", &T::type, "properties", &T::properties, "required", &T::required, "additionalProperties", &T::additional_properties);
};
