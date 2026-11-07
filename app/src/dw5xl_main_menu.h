// Issue 058: a way back to the main menu, through the game's own Select + Start reset.
#pragma once

class PS2Runtime;

namespace dw5xl::mainmenu {
// True while the game asks for its reset: where it does not, Select + Start does nothing and neither does this.
bool gameListens();
// The game is told to reset until its main menu starts, as if the buttons were still held. A state load cancels this.
void requestReturn(PS2Runtime &runtime);
}
