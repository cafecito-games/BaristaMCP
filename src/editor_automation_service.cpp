/**************************************************************************/
/*  editor_automation_service.cpp                                         */
/*                                                                        */
/*  Copyright (c) 2026-present Cafecito Games LLC.                        */
/*  This file is part of BaristaMCP, a Godot GDExtension.                 */
/*  SPDX-License-Identifier: MIT                                          */
/**************************************************************************/

#include "editor_automation_service.h"

#include "editor_snapshot.h"

#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/variant/variant.hpp>

namespace godot {

namespace {

int clamp_int(int64_t p_value, int p_min, int p_max) {
	if (p_value < p_min) {
		return p_min;
	}
	if (p_value > p_max) {
		return p_max;
	}
	return (int)p_value;
}

int64_t serialized_size(const Dictionary &p_payload) {
	return JSON::stringify(p_payload, "", false).to_utf8_buffer().size();
}

} // namespace

void EditorAutomationService::configure(EditorInterface *p_editor_interface) {
	editor_interface = p_editor_interface;
}

EditorSnapshotOptions EditorAutomationService::parse_options(const Dictionary &p_arguments) {
	// Types and the int64_t representable range are both enforced by the boundary schema before this
	// runs, so these conversions cannot narrow. The documented option ranges are clamped here so a
	// client can never request an unbounded capture.
	EditorSnapshotOptions options;
	if (p_arguments.has("max_depth")) {
		options.max_depth = clamp_int((int64_t)p_arguments.get("max_depth", options.max_depth),
				EditorSnapshotLimits::MIN_MAX_DEPTH, EditorSnapshotLimits::MAX_MAX_DEPTH);
	}
	if (p_arguments.has("max_elements")) {
		options.max_elements = clamp_int((int64_t)p_arguments.get("max_elements", options.max_elements),
				EditorSnapshotLimits::MIN_MAX_ELEMENTS, EditorSnapshotLimits::MAX_MAX_ELEMENTS);
	}
	if (p_arguments.has("include_internal")) {
		options.include_internal = (bool)p_arguments.get("include_internal", false);
	}
	return options;
}

Dictionary EditorAutomationService::inspect_ui(const Dictionary &p_arguments, String &r_error, String &r_message) {
	r_error = String();
	r_message = String();
	if (editor_interface == nullptr) {
		r_error = "unsupported_capability";
		r_message = "No editor interface is available for UI inspection.";
		return Dictionary();
	}

	const EditorSnapshotOptions requested = parse_options(p_arguments);
	EditorSnapshotOptions applied = requested;
	Dictionary payload;
	// Deterministic truncation: halve the element budget until the serialized snapshot fits the
	// published payload limit, and report the budget that actually produced the result.
	while (true) {
		EditorSnapshotData data;
		generation++;
		if (!EditorSnapshot::capture(editor_interface, generation, applied, data)) {
			r_error = "unsupported_capability";
			r_message = "The editor base control is unavailable; no UI snapshot was produced.";
			return Dictionary();
		}
		data.requested_options = requested;
		data.applied_options = applied;
		payload = EditorSnapshot::serialize(data);
		if (serialized_size(payload) <= EditorSnapshotLimits::MAX_PAYLOAD_BYTES ||
				applied.max_elements <= EditorSnapshotLimits::MIN_MAX_ELEMENTS) {
			break;
		}
		applied.max_elements = applied.max_elements / 2;
		if (applied.max_elements < EditorSnapshotLimits::MIN_MAX_ELEMENTS) {
			applied.max_elements = EditorSnapshotLimits::MIN_MAX_ELEMENTS;
		}
	}
	return payload;
}

void EditorAutomationService::process(double p_delta) {
	(void)p_delta;
}

void EditorAutomationService::shutdown() {
	editor_interface = nullptr;
}

} // namespace godot
