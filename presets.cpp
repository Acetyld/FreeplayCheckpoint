/*
 * Copyright (c) 2021
 * All rights reserved.
 *
 * This source code is licensed under the MIT-style license found in
 * the LICENSE file in the root directory of this source tree.
 */

#include "pch.h"
#include "CheckpointPlugin.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

namespace {
	constexpr const char* PRESET_FOLDER_NAME = "FreeplayCheckpoint";
	constexpr const char* PRESET_SUBFOLDER_NAME = "presets";
	constexpr const char* DEFAULT_PRESET_FILE_NAME = "freeplaycheckpoint.data";

	bool isInvalidFilenameCharacter(char c) {
		switch (c) {
		case '<':
		case '>':
		case ':':
		case '"':
		case '/':
		case '\\':
		case '|':
		case '?':
		case '*':
		case '&':
		case '@':
			return true;
		default:
			return static_cast<unsigned char>(c) < 32;
		}
	}

	std::string trimWhitespace(std::string value) {
		auto notSpace = [](unsigned char c) {
			return !std::isspace(c);
		};

		value.erase(
			value.begin(),
			std::find_if(value.begin(), value.end(), notSpace)
		);

		value.erase(
			std::find_if(value.rbegin(), value.rend(), notSpace).base(),
			value.end()
		);

		return value;
	}

	void migrateFileIfNeeded(
		const std::filesystem::path& oldPath,
		const std::filesystem::path& newPath
	) {
		std::error_code ec;

		if (!std::filesystem::exists(oldPath, ec) || ec) {
			return;
		}

		ec.clear();
		if (std::filesystem::exists(newPath, ec) || ec) {
			return;
		}

		ec.clear();
		std::filesystem::rename(oldPath, newPath, ec);

		if (!ec) {
			return;
		}

		// rename() can fail if the source and destination are on different
		// filesystems. Fall back to copy + remove.
		ec.clear();
		std::filesystem::copy_file(
			oldPath,
			newPath,
			std::filesystem::copy_options::none,
			ec
		);

		if (ec) {
			return;
		}

		ec.clear();
		std::filesystem::remove(oldPath, ec);
	}
}

std::filesystem::path CheckpointPlugin::getPresetDirectory() {
	auto presetDirectory =
		gameWrapper->GetDataFolder()
		/ PRESET_FOLDER_NAME
		/ PRESET_SUBFOLDER_NAME;

	std::error_code ec;
	std::filesystem::create_directories(presetDirectory, ec);

	if (ec) {
		cvarManager->log(
			"Freeplay Checkpoint: could not create preset directory: "
			+ presetDirectory.string()
		);
	}

	return presetDirectory;
}

std::filesystem::path CheckpointPlugin::getPresetPath(
	const std::string& filename
) {
	// filename() strips any supplied parent path. cpt_filename should identify
	// a preset inside our preset directory, never an arbitrary filesystem path.
	auto safeFilename = std::filesystem::path(filename).filename();

	return getPresetDirectory() / safeFilename;
}

std::filesystem::path CheckpointPlugin::getCurrentPresetPath() {
	auto filename = cvarManager->getCvar("cpt_filename").getStringValue();
	return getPresetPath(filename);
}

std::vector<std::string> CheckpointPlugin::getPresetFiles() {
	std::vector<std::string> presets;

	auto presetDirectory = getPresetDirectory();

	std::error_code ec;
	std::filesystem::directory_iterator iterator(presetDirectory, ec);

	if (ec) {
		cvarManager->log(
			"Freeplay Checkpoint: could not enumerate preset directory: "
			+ presetDirectory.string()
		);
		return presets;
	}

	for (const auto& entry : iterator) {
		std::error_code entryError;

		if (!entry.is_regular_file(entryError) || entryError) {
			continue;
		}

		const auto& path = entry.path();

		if (path.extension() != ".data") {
			continue;
		}

		presets.push_back(path.filename().string());
	}

	std::sort(
		presets.begin(),
		presets.end(),
		[](const std::string& left, const std::string& right) {
			std::string lowerLeft = left;
			std::string lowerRight = right;

			std::transform(
				lowerLeft.begin(),
				lowerLeft.end(),
				lowerLeft.begin(),
				[](unsigned char c) {
					return static_cast<char>(std::tolower(c));
				}
			);

			std::transform(
				lowerRight.begin(),
				lowerRight.end(),
				lowerRight.begin(),
				[](unsigned char c) {
					return static_cast<char>(std::tolower(c));
				}
			);

			return lowerLeft < lowerRight;
		}
	);

	return presets;
}

std::string CheckpointPlugin::sanitizePresetName(
	const std::string& rawName
) {
	std::string name = trimWhitespace(rawName);

	if (name.empty() || name == "." || name == "..") {
		return "";
	}

	// Do not allow a caller to supply a directory.
	name = std::filesystem::path(name).filename().string();

	for (char& c : name) {
		if (isInvalidFilenameCharacter(c)) {
			c = '_';
		}
	}

	name = trimWhitespace(name);

	// Windows filenames cannot end in a period or space.
	while (!name.empty() && (name.back() == '.' || name.back() == ' ')) {
		name.pop_back();
	}

	if (name.empty() || name == "." || name == "..") {
		return "";
	}

	std::filesystem::path path(name);

	// Preset files always use exactly one .data extension.
	if (path.extension() == ".data") {
		name = path.stem().string();
	}
	else if (path.has_extension()) {
		// Treat any other extension as part of the user-visible preset name.
		// Example: "my.shots" -> "my.shots.data"
		name = path.filename().string();
	}

	if (name.empty()) {
		return "";
	}

	return name + ".data";
}

void CheckpointPlugin::createPreset(
	std::vector<std::string> command
) {
	auto presetNameCvar = cvarManager->getCvar("cpt_new_preset_name");

	if (presetNameCvar.IsNull()) {
		cvarManager->log(
			"Freeplay Checkpoint: cpt_new_preset_name is not registered"
		);
		return;
	}

	std::string filename = sanitizePresetName(presetNameCvar.getStringValue());

	if (filename.empty()) {
		cvarManager->log("Freeplay Checkpoint: preset name cannot be empty");
		return;
	}

	auto presetPath = getPresetPath(filename);

	std::error_code ec;
	if (std::filesystem::exists(presetPath, ec)) {
		cvarManager->log("Freeplay Checkpoint: preset already exists: " + filename);
		return;
	}

	if (ec) {
		cvarManager->log("Freeplay Checkpoint: could not check preset path: " + presetPath.string());
		return;
	}

	// A new preset starts as an empty checkpoint collection.
	setFrozen(false, false);
	checkpoints.clear();
	locks.clear();
	curCheckpoint = 0;

	hasQuickCheckpoint = false;
	rewindState.atCheckpoint = false;
	rewindState.justDeletedCheckpoint = false;
	rewindState.justLoadedQuickCheckpoint = false;

	// The existing cpt_filename value-changed callback will call
	// loadCheckpointFile(). The refactored loader should safely return if the
	// new file does not exist yet.
	cvarManager->getCvar("cpt_filename").setValue(filename);

	// Creates the new file using the plugin's existing checkpoint format.
	saveCheckpointFile();

	// Clear the text box after successful creation.
	presetNameCvar.setValue("");

	// Regenerate the dropdown so the new preset becomes available immediately.
	writeSettingsFile();

	cvarManager->log("Freeplay Checkpoint: created preset " + filename);
}

void CheckpointPlugin::migrateLegacyPresets() {
	auto dataDirectory = gameWrapper->GetDataFolder();
	auto presetDirectory = getPresetDirectory();

	// Always migrate the plugin's original default file if it exists.
	migrateFileIfNeeded(
		dataDirectory / DEFAULT_PRESET_FILE_NAME,
		presetDirectory / DEFAULT_PRESET_FILE_NAME
	);

	// The original plugin also allowed the user to manually set cpt_filename.
	// If that configured file exists directly in BakkesMod/data, migrate it too.
	auto filenameCvar = cvarManager->getCvar("cpt_filename");

	if (!filenameCvar.IsNull()) {
		auto configuredFilename =
			std::filesystem::path(filenameCvar.getStringValue()).filename();

		if (
			!configuredFilename.empty()
			&& configuredFilename != DEFAULT_PRESET_FILE_NAME
		) {
			migrateFileIfNeeded(
				dataDirectory / configuredFilename,
				presetDirectory / configuredFilename
			);
		}
	}
}

void CheckpointPlugin::ensureDefaultPreset() {
	auto presets = getPresetFiles();

	if (!presets.empty()) {
		// If the configured preset no longer exists, select the first available
		// preset rather than leaving the plugin pointed at a missing file.
		auto currentFilename =
			std::filesystem::path(
				cvarManager->getCvar("cpt_filename").getStringValue()
			).filename().string();

		auto currentPath = getPresetPath(currentFilename);

		std::error_code ec;
		if (
			currentFilename.empty()
			|| !std::filesystem::exists(currentPath, ec)
			|| ec
		) {
			cvarManager->getCvar("cpt_filename").setValue(presets.front());
		}

		return;
	}

	// First run: make the original default filename the active preset and
	// create an empty checkpoint file with the existing serialization logic.
	checkpoints.clear();
	locks.clear();
	curCheckpoint = 0;

	cvarManager->getCvar("cpt_filename").setValue(DEFAULT_PRESET_FILE_NAME);

	saveCheckpointFile();
}
