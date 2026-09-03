/**************************************************************************/
/*  mcp_schema.cpp                                                        */
/*                                                                        */
/*  Copyright (c) 2026-present Cafecito Games LLC.                        */
/*  This file is part of BaristaMCP, a Godot GDExtension.                 */
/*  SPDX-License-Identifier: MIT                                          */
/**************************************************************************/

#include "mcp_schema.h"

#include <godot_cpp/core/math.hpp>
#include <godot_cpp/variant/array.hpp>

namespace godot {

namespace {

Dictionary make_schema(const String &p_type, const String &p_description) {
	Dictionary schema;
	schema["type"] = p_type;
	if (!p_description.is_empty()) {
		schema["description"] = p_description;
	}
	return schema;
}

bool matches_type(const String &p_type, const Variant &p_value, bool &r_known_type) {
	r_known_type = true;
	const Variant::Type variant_type = p_value.get_type();
	if (p_type == "object") {
		return variant_type == Variant::DICTIONARY;
	}
	if (p_type == "array") {
		return variant_type == Variant::ARRAY;
	}
	if (p_type == "string") {
		return variant_type == Variant::STRING || variant_type == Variant::STRING_NAME;
	}
	if (p_type == "boolean") {
		return variant_type == Variant::BOOL;
	}
	if (p_type == "integer") {
		if (variant_type == Variant::INT) {
			return true;
		}
		if (variant_type != Variant::FLOAT) {
			return false;
		}
		// JSON has one number type, so an integral float is a valid JSON Schema integer.
		const double value = p_value;
		return Math::is_finite(value) && value == Math::floor(value);
	}
	if (p_type == "number") {
		if (variant_type == Variant::INT) {
			return true;
		}
		return variant_type == Variant::FLOAT && Math::is_finite((double)p_value);
	}
	r_known_type = false;
	return false;
}

bool validate_against(const Dictionary &p_schema, const Variant &p_value, const String &p_path, String &r_error) {
	const String type = p_schema.get("type", String());
	if (!type.is_empty()) {
		bool known_type = false;
		if (!matches_type(type, p_value, known_type)) {
			if (!known_type) {
				r_error = "Schema for " + p_path + String(" declares unsupported type '") + type + "'.";
			} else {
				r_error = p_path + String(" must be of type '") + type + "'.";
			}
			return false;
		}
	}

	if (type == "object") {
		const Dictionary value = p_value;
		const Dictionary properties = p_schema.get("properties", Dictionary());
		const Array required = p_schema.get("required", Array());
		for (int i = 0; i < required.size(); i++) {
			const String name = required[i];
			if (!value.has(name)) {
				r_error = p_path + String(" is missing required property '") + name + "'.";
				return false;
			}
		}
		const bool allows_additional = p_schema.get("additionalProperties", true);
		const Array keys = value.keys();
		for (int i = 0; i < keys.size(); i++) {
			const Variant key = keys[i];
			if (key.get_type() != Variant::STRING && key.get_type() != Variant::STRING_NAME) {
				r_error = p_path + String(" must use string property names.");
				return false;
			}
			const String name = key;
			if (!properties.has(name)) {
				if (!allows_additional) {
					r_error = p_path + String(" has unknown property '") + name + "'.";
					return false;
				}
				continue;
			}
			const Dictionary property_schema = properties.get(name, Dictionary());
			if (!validate_against(property_schema, value.get(name, Variant()), p_path + String(".") + name, r_error)) {
				return false;
			}
		}
		return true;
	}

	if (p_schema.has("enum")) {
		const Array allowed = p_schema.get("enum", Array());
		bool found = false;
		for (int i = 0; i < allowed.size(); i++) {
			if (allowed[i] == p_value) {
				found = true;
				break;
			}
		}
		if (!found) {
			r_error = p_path + String(" must be one of the advertised enum values.");
			return false;
		}
	}

	if (type == "array" && p_schema.has("items")) {
		const Array value = p_value;
		const Dictionary items = p_schema.get("items", Dictionary());
		for (int i = 0; i < value.size(); i++) {
			if (!validate_against(items, value[i], p_path + String("[") + String::num_int64(i) + "]", r_error)) {
				return false;
			}
		}
	}
	return true;
}

} // namespace

Dictionary MCPSchema::object(const String &p_description) {
	Dictionary schema = make_schema("object", p_description);
	schema["properties"] = Dictionary();
	schema["required"] = Array();
	schema["additionalProperties"] = false;
	return schema;
}

Dictionary MCPSchema::open_object(const String &p_description) {
	Dictionary schema = make_schema("object", p_description);
	schema["properties"] = Dictionary();
	schema["required"] = Array();
	schema["additionalProperties"] = true;
	return schema;
}

Dictionary MCPSchema::array(const Dictionary &p_items, const String &p_description) {
	Dictionary schema = make_schema("array", p_description);
	schema["items"] = p_items;
	return schema;
}

Dictionary MCPSchema::string(const String &p_description) {
	return make_schema("string", p_description);
}

Dictionary MCPSchema::integer(const String &p_description) {
	return make_schema("integer", p_description);
}

Dictionary MCPSchema::number(const String &p_description) {
	return make_schema("number", p_description);
}

Dictionary MCPSchema::boolean(const String &p_description) {
	return make_schema("boolean", p_description);
}

Dictionary MCPSchema::enum_string(const PackedStringArray &p_values, const String &p_description) {
	Dictionary schema = make_schema("string", p_description);
	Array values;
	for (int i = 0; i < p_values.size(); i++) {
		values.push_back(p_values[i]);
	}
	schema["enum"] = values;
	return schema;
}

void MCPSchema::add_property(
		Dictionary &r_schema, const String &p_name, const Dictionary &p_property, bool p_required) {
	Dictionary properties = r_schema.get("properties", Dictionary());
	properties[p_name] = p_property;
	r_schema["properties"] = properties;
	if (p_required) {
		Array required = r_schema.get("required", Array());
		required.push_back(p_name);
		r_schema["required"] = required;
	}
}

bool MCPSchema::validate(const Dictionary &p_schema, const Variant &p_value, String &r_error) {
	return validate_against(p_schema, p_value, "value", r_error);
}

} // namespace godot
