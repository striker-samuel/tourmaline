// Thin entry point: all host logic (CLI parse, pak preload, world source,
// renderer wiring, main loop) lives in the engine — see
// Engine/App/Host.h (engine::RunPakApp).
#include <Engine/App/Host.h>

int main(int argc, const char* argv[]) {
	return engine::RunPakApp(argc, argv);
}
