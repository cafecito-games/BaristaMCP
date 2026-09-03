/**************************************************************************/
/*  mcp_contracts.h                                                       */
/*                                                                        */
/*  Copyright (c) 2026-present Cafecito Games LLC.                        */
/*  This file is part of BaristaMCP, a Godot GDExtension.                 */
/*  SPDX-License-Identifier: MIT                                          */
/**************************************************************************/

#ifndef BARISTA_MCP_CONTRACTS_H
#define BARISTA_MCP_CONTRACTS_H

#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

namespace godot {

// Single source of truth for the BaristaMCP tool and resource vocabulary. Advertised schemas and the
// schemas enforced at the request boundary are the same documents.
class MCPContracts {
public:
	static constexpr const char *PROTOCOL_VERSION = "2025-11-25";
	static constexpr const char *SERVER_NAME = "BaristaMCP";
	static constexpr const char *SERVER_VERSION = "0.1.0";
	static constexpr const char *PROJECT_INFO_RESOURCE_URI = "barista://project/info";
	static constexpr const char *UI_TREE_RESOURCE_URI = "barista://ui/tree";
	static constexpr const char *EDITOR_STATE_RESOURCE_URI = "barista://editor/state";
	static constexpr const char *ACTIVE_SCENE_RESOURCE_URI = "barista://scene/active";
	static constexpr const char *SCENE_TREE_RESOURCE_URI = "barista://scene/tree";
	static constexpr const char *UI_ELEMENT_TEMPLATE_URI = "barista://ui/element/{handle}";
	static constexpr const char *UI_SUBTREE_TEMPLATE_URI = "barista://ui/subtree/{handle}";
	// Longest accepted resource-template segment. A longer segment cannot name an issued handle.
	static constexpr int MAX_TEMPLATE_SEGMENT_LENGTH = 64;
	static constexpr const char *JSON_MIME_TYPE = "application/json";
	// Serialized resource payloads larger than this are refused instead of overrunning the transport
	// response cap enforced by MCPServer.
	static constexpr int MAX_RESOURCE_PAYLOAD_BYTES = 512 * 1024;

	// What "ok" asserts for one action route. A "delivery" route asserts only that the requested input
	// reached the exact requested target, and never that the editor did anything in response; the target
	// must still be one that accepts that input, so delivering to something that would demonstrably
	// ignore or reject it is a failed delivery. An "effect" route asserts the requested state now holds
	// and verifies that postcondition before reporting success.
	static constexpr const char *CLAIM_DELIVERY = "delivery";
	static constexpr const char *CLAIM_EFFECT = "effect";

	// The two claims an action route may declare. This is the whole vocabulary.
	static PackedStringArray claim_vocabulary();
	// The claim one advertised action declares, or an empty string when the action is not advertised.
	// An advertised action with no declared claim is a build error a contract test catches, never a
	// silently defaulted claim.
	static String action_claim(const String &p_action);
	// One entry per advertised action, each publishing that route's claim, so a client can tell before
	// it acts whether it must observe the editor afterwards.
	static Array build_action_claims();

	// Name of the only mutating tool. It is advertised solely when mutation was enabled before startup,
	// and it is the one tool whose absence must still be answered with a stable status.
	static constexpr const char *ACT_TOOL_NAME = "act_on_editor_ui";

	// The advertised tool list. The mutating tool is present only when this session enabled mutation
	// before startup, so a disabled session never advertises what it will refuse to do.
	static Array build_tools_list(bool p_mutation_enabled);
	// Looks up one advertised tool. p_mutation_enabled must be true only where the mutating tool's own
	// schemas are needed, so a disabled session can still publish its refusal in the advertised shape.
	static bool find_tool(const String &p_name, Dictionary &r_tool, bool p_mutation_enabled);

	static Array build_resources_list();
	static Array build_resource_templates_list();
	static bool find_resource(const String &p_uri, Dictionary &r_resource);
	// Resolves one advertised resource URI, exact or templated. A templated URI yields its
	// percent-decoded handle segment, validated against the handle grammar; a segment that is not a
	// well-formed handle is not a resource, so a URI segment never becomes an unchecked lookup.
	static bool resolve_resource(const String &p_uri, Dictionary &r_resource, String &r_handle);

	static Dictionary list_params_schema();
	static Dictionary resource_read_params_schema();
};

} // namespace godot

#endif // BARISTA_MCP_CONTRACTS_H
