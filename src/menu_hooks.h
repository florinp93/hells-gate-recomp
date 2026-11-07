#pragma once

#include <functional>

namespace menu_hooks {

void SetQuitToDesktopHandler(std::function<void()> handler);

}  // namespace menu_hooks
