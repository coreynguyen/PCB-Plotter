#pragma once
// ============================================================================
// input_resync.h — R20: drop Dear ImGui's keyboard state after the app window regains focus.
//
// A Win32 modal dialog opened from a shortcut (Ctrl+O) receives the key-up messages, and the
// WM_KILLFOCUS / WM_SETFOCUS pair it causes is queued into ONE ImGui frame. ImGui coalesces the two
// focus events ("lost" is overwritten by "regained") and therefore never clears its key state: Ctrl
// and O stay "down", and IsKeyPressed(O, repeat) keeps firing. The shell calls this before NewFrame()
// whenever WM_SETFOCUS / WM_ACTIVATEAPP arrived (portable: also used by tests/test_files_preview.cpp).
// ============================================================================
#include "imgui.h"

namespace pcbp::gui {

inline void imgui_drop_stale_keys(ImGuiIO& io) { io.ClearInputKeys(); }

} // namespace pcbp::gui
