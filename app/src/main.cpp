// App host entry: CLI parse -> pak-viewer run. The pak loading (preload,
// world source, sponza check-scene extras, demo-content seeding) lives in
// this target — PakApp.cpp/PakPreload.cpp/Host.h — NOT in the engine; the
// engine's WorldRenderer renders whatever world the host hands it.
#include "Host.h"

int main(int argc, const char* argv[]) {
	app::PakAppConfig cfg;
	int exitCode = 0;
	if (!app::ParsePakAppArgs(argc, argv, cfg, exitCode))
		return exitCode;
	return app::RunPakApp(cfg);
}
