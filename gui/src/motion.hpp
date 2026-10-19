#pragma once
#include <ui_core.h>

namespace spg::motion {
void enter(UiWindow window, const char* id, float x = 0, float y = 16);
void finish(UiWindow window, const char* id);
void select(UiWindow window, const char* indicator, const char* target);
}
