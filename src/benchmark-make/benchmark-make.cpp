#include "precompiled.hpp"
#include "state.hpp"
#include "board.hpp"
#include "board-defs.inc"

int main(int argc, char* argv[]) { //genbuild {'entrypoint': True, 'ldflags': ''}
	pushfight::enumerate_anchored_states(pushfight::traditional);
	return 0;
}
