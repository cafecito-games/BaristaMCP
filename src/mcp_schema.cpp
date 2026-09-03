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

#include <cstdint>

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

// The range an "integer" schema promises. JSON carries every number as a double, so a well-formed
// integral number can still sit outside int64_t; converting such a value is undefined and diverges
// between architectures (arm64 saturates, x86-64 yields INT64_MIN). The boundary rejects it instead.
// -2^63 is exactly representable as a double; the largest double strictly below 2^63 is 2^63 - 1024.
constexpr int64_t INTEGER_MINIMUM = -9223372036854775807LL - 1LL;
constexpr int64_t INTEGER_MAXIMUM = 9223372036854774784LL;

// Compares a validated numeric value against a schema bound without narrowing either side: integers
// are compared as integers and floats as doubles.
bool violates_bound(const Dictionary &p_schema, const String &p_key, const Variant &p_value, bool p_is_maximum) {
	if (!p_schema.has(p_key)) {
		return false;
	}
	const Variant bound = p_schema.get(p_key, Variant());
	if (p_value.get_type() == Variant::INT && bound.get_type() == Variant::INT) {
		const int64_t value = p_value;
		const int64_t limit = bound;
		return p_is_maximum ? value > limit : value < limit;
	}
	const double value = p_value;
	const double limit = bound;
	return p_is_maximum ? value > limit : value < limit;
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

// Guards against a definition chain that never reaches a concrete schema.
constexpr int MAX_REFERENCE_HOPS = 8;
constexpr const char *DEFINITIONS_KEY = "$defs";
constexpr const char *REFERENCE_PREFIX = "#/$defs/";

bool validate_against(const Dictionary &p_root, const Dictionary &p_schema, const Variant &p_value,
		const String &p_path, String &r_error) {
	Dictionary schema = p_schema;
	for (int hops = 0; schema.has("$ref"); hops++) {
		if (hops >= MAX_REFERENCE_HOPS) {
			r_error = "Schema for " + p_path + String(" has an unresolvable definition chain.");
			return false;
		}
		const Variant reference = schema.get("$ref", Variant());
		if (reference.get_type() != Variant::STRING) {
			r_error = "Schema for " + p_path + String(" has a non-string '$ref'.");
			return false;
		}
		const String pointer = reference;
		if (!pointer.begins_with(REFERENCE_PREFIX)) {
			r_error = "Schema for " + p_path + String(" references an unsupported pointer '") + pointer + "'.";
			return false;
		}
		const String name = pointer.substr(String(REFERENCE_PREFIX).length());
		const Dictionary definitions = p_root.get(DEFINITIONS_KEY, Dictionary());
		if (!definitions.has(name)) {
			r_error = "Schema for " + p_path + String(" references undefined '") + name + "'.";
			return false;
		}
		schema = definitions.get(name, Dictionary());
	}

	const String type = schema.get("type", String());
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

	if (violates_bound(schema, "minimum", p_value, false) || violates_bound(schema, "maximum", p_value, true)) {
		r_error = p_path + String(" is outside the advertised numeric range.");
		return false;
	}

	if (type == "object") {
		const Dictionary value = p_value;
		const Dictionary properties = schema.get("properties", Dictionary());
		const Array required = schema.get("required", Array());
		for (int i = 0; i < required.size(); i++) {
			const String name = required[i];
			if (!value.has(name)) {
				r_error = p_path + String(" is missing required property '") + name + "'.";
				return false;
			}
		}
		const bool allows_additional = schema.get("additionalProperties", true);
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
			if (!validate_against(
						p_root, property_schema, value.get(name, Variant()), p_path + String(".") + name, r_error)) {
				return false;
			}
		}
		return true;
	}

	if (schema.has("enum")) {
		const Array allowed = schema.get("enum", Array());
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

	if (type == "array" && schema.has("items")) {
		const Array value = p_value;
		const Dictionary items = schema.get("items", Dictionary());
		for (int i = 0; i < value.size(); i++) {
			if (!validate_against(
						p_root, items, value[i], p_path + String("[") + String::num_int64(i) + "]", r_error)) {
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
	Dictionary schema = make_schema("integer", p_description);
	// Advertised, not implicit: a client can read the exact range this boundary accepts.
	schema["minimum"] = INTEGER_MINIMUM;
	schema["maximum"] = INTEGER_MAXIMUM;
	return schema;
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

Dictionary MCPSchema::reference(const String &p_definition_name) {
	Dictionary schema;
	schema["$ref"] = String(REFERENCE_PREFIX) + p_definition_name;
	return schema;
}

void MCPSchema::add_definition(Dictionary &r_schema, const String &p_name, const Dictionary &p_definition) {
	Dictionary definitions = r_schema.get(DEFINITIONS_KEY, Dictionary());
	definitions[p_name] = p_definition;
	r_schema[DEFINITIONS_KEY] = definitions;
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
	return validate_against(p_schema, p_schema, p_value, "value", r_error);
}

} // namespace godot
