/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

/*! \mainpage %ScummVM Source Reference
 *
 * These pages contain a cross referenced documentation for the %ScummVM source code,
 * generated with Doxygen (https://www.doxygen.nl) directly from the source.
 * Currently not much is actually properly documented, but at least you can get an overview
 * of almost all the classes, methods and variables, and how they interact.
 */

// FIXME: Avoid using printf
#define FORBIDDEN_SYMBOL_EXCEPTION_printf

#include "base/commandLine.h"
#include "base/plugins.h"
#include "base/version.h"
#include "engines/engine.h"
#include "engines/metaengine.h"

#include "common/archive.h"
#include "common/config-manager.h"
#include "common/debug.h"
#include "common/events.h"
#include "common/fs.h"
#include "common/system.h"
#include "common/text-to-speech.h"
#include "common/textconsole.h"
#include "common/tokenizer.h"
#include "common/translation.h"

static Common::Error identifyGame(const Plugin **detectionPlugin, DetectedGame &game, const void **descriptor) {
	assert(detectionPlugin);

	// Figure out the engine ID and game ID
	Common::String engineId = ConfMan.get("engineid");
	Common::String gameId = ConfMan.get("gameid");

	// Print text saying what's going on
	debug("User picked target '%s' (engine ID '%s', game ID '%s')...\n", ConfMan.getActiveDomainName().c_str(), engineId.c_str(), gameId.c_str());

	// At this point the engine ID and game ID must be known
	if (engineId.empty()) {
		warning("The engine ID is not set for target '%s'", ConfMan.getActiveDomainName().c_str());
		return Common::kUnknownError;
	}

	if (gameId.empty()) {
		warning("The game ID is not set for target '%s'", ConfMan.getActiveDomainName().c_str());
		return Common::kUnknownError;
	}

	*detectionPlugin = EngineMan.findDetectionPlugin(engineId);
	if (!*detectionPlugin) {
		warning("'%s' is an invalid engine ID. Use the --list-engines command to list supported engine IDs", engineId.c_str());
		return Common::kMetaEnginePluginNotFound;
	}

	// Query the plugin for the game descriptor
	MetaEngineDetection &metaEngine = (*detectionPlugin)->get<MetaEngineDetection>();

	Common::Error result = metaEngine.identifyGame(game, descriptor);
	if (result.getCode() != Common::kNoError) {
		warning("Couldn't identify game '%s' for the engine '%s'.", gameId.c_str(), engineId.c_str());
	}

	return result;
}

static Common::Error randomizeGame(const Plugin *enginePlugin, const DetectedGame &game, const void *meDescriptor) {
	assert(enginePlugin);

	// Determine the game data path, for validation and error messages
	Common::FSNode dir(ConfMan.getPath("path"));
	Common::String target = ConfMan.getActiveDomainName();
	Common::Error err = Common::kNoError;
	Engine *engine = nullptr;

	// Verify that the game path refers to an actual directory
	if (!dir.exists()) {
		err = Common::kPathDoesNotExist;
	} else if (!dir.isDirectory()) {
		err = Common::kPathNotDirectory;
	}

	// Create the game's MetaEngine.
	MetaEngine &metaEngine = enginePlugin->get<MetaEngine>();
	if (err.getCode() == Common::kNoError) {
		// Set default values for all of the custom engine options
		// Apparently some engines query them in their constructor, thus we
		// need to set this up before instance creation.
		metaEngine.registerDefaultSettings(target);
		err = metaEngine.createInstance(g_system, &engine, game, meDescriptor);
	}

	// Check for errors
	if (!engine || err.getCode() != Common::kNoError) {
		warning("%s failed to instantiate engine: %s (target '%s', path '%s')",
				game.engineId.c_str(),
				err.getDesc().c_str(),
				target.c_str(),
				dir.getPath().toString(Common::Path::kNativeSeparator).c_str());

		metaEngine.deleteInstance(engine, game, meDescriptor);

		return err;
	}

	// Set up the metaengine
	engine->setMetaEngine(&metaEngine);

	// Add the game path to the directory search list
	engine->initializePath(dir);

	// Randomize the game
	Common::Error result = engine->randomizeGameFiles();

	// Free up memory
	metaEngine.deleteInstance(engine, game, meDescriptor);

	// Return result (== 0 means no error)
	return result;
}

static int exitWithResult(const Common::Error &res) {
	if (res.getCode() != Common::kNoError) {
		warning("%s", res.getDesc().c_str());
	} else {
		debug("Normal exit");
	}

	PluginManager::destroy();
	Common::ConfigManager::destroy();
	EngineManager::destroy();

	return res.getCode();
}

extern "C" int scummvm_main(int argc, const char *const argv[]) {
	// Register config manager defaults
	Base::registerDefaults();

	// Parse the command line
	Common::StringMap settings;
	Common::String gameId = Base::parseCommandLine(settings, argc, argv);

	// Load the config file (possibly overridden via command line):
	Common::Path initConfigFilename;
	if (settings.contains("initial-cfg")) {
		initConfigFilename = Common::Path(settings["initial-cfg"], Common::Path::kNativeSeparator);
	}

	bool configLoadStatus;
	if (settings.contains("config")) {
		configLoadStatus = ConfMan.loadConfigFile(Common::Path(settings["config"], Common::Path::kNativeSeparator), initConfigFilename);
	} else {
		configLoadStatus = ConfMan.loadDefaultConfigFile(initConfigFilename);
	}

	if (!configLoadStatus) {
		warning("Failed to load configuration file.");
	}

	// Update the config file
	ConfMan.set("versioninfo", gScummVMVersion, Common::ConfigManager::kApplicationDomain);

	ConfMan.registerDefault("always_run_fallback_detection_extern", true);
	PluginManager::instance().init();
	PluginManager::instance().loadAllPlugins();      // load plugins for cached plugin manager
	PluginManager::instance().loadDetectionPlugin(); // load detection plugin for uncached plugin manager

	// Process the remaining command line settings. Must be done after the
	// config file and the plugins have been loaded.
	Common::Error res;
	if (Base::processSettings(gameId, settings, res)) {
		return exitWithResult(res);
	}

	EngineMan.upgradeTargetIfNecessary(ConfMan.getActiveDomainName());

	// Try to find a MetaEnginePlugin which feels responsible for the specified game.
	const Plugin *enginePlugin = nullptr;
	const Plugin *plugin = nullptr;
	DetectedGame game;
	const void *meDescriptor = nullptr;
	Common::Error result = identifyGame(&plugin, game, &meDescriptor);
	if (result.getCode() != Common::kNoError) {
		return exitWithResult(result);
	}

	Common::String engineId = plugin->getName();

	// Then, get the relevant Engine plugin from MetaEngine.
	enginePlugin = PluginMan.findEnginePlugin(engineId);
	if (enginePlugin == nullptr) {
		result = Common::kEnginePluginNotFound;
		return exitWithResult(result);
	}

	// Unload all plugins not needed for this game, to save memory
	// Right now, we have a MetaEngine plugin, and we want to unload all except Engine.

	// Pass in the pointer to enginePlugin, with the matching type, so our function behaves as-is.
	PluginManager::instance().unloadPluginsExcept(PLUGIN_TYPE_ENGINE, enginePlugin);

	// Try to randomize the game
	result = randomizeGame(enginePlugin, game, meDescriptor);
	return exitWithResult(result);
}
