#pragma once
// ============================================================================
// dialog_guard.h — R20 loop guard for blocking file dialogs (portable, header-only).
//
// Bug (specs/DOCUMENTS.md "R20 open-dialog loop"): a shortcut that opens a Win32 modal dialog
// (Ctrl+O -> File > Open) left its keys "down" in Dear ImGui, because the key-up messages went
// to the dialog and the focus-lost + focus-regained pair was coalesced into one frame (ImGui
// then never clears its key state). The editor's IsKeyPressed(O, repeat) kept firing, so every
// Cancel re-posted the command and the dialog came straight back.
//
// Rule enforced here, independent of where a request comes from: a dialog that was cancelled is
// not shown again with the same title until the user has produced fresh input (a key-down or a
// mouse-button-down message reached the app window after the dialog closed). A dialog is also
// never re-entered while it is showing. Stale / synthetic repeats therefore cannot loop.
// ============================================================================
#include <cstdint>
#include <map>
#include <set>
#include <string>

namespace pcbp {

class DialogLoopGuard {
public:
    // Call for every real key-down / mouse-button-down message the app window receives.
    void note_input() { ++input_serial_; }
    uint64_t input_serial() const { return input_serial_; }

    // Before showing dialog `key` (its title). false = do not show (re-entry or cancelled loop).
    bool begin(const std::string& key) {
        if (open_.count(key)) { ++blocked_; return false; }
        auto it = cancelled_at_.find(key);
        if (it != cancelled_at_.end() && it->second == input_serial_) { ++blocked_; return false; }
        open_.insert(key);
        return true;
    }
    // After the dialog closed. cancelled = the user dismissed it without choosing anything.
    void end(const std::string& key, bool cancelled) {
        open_.erase(key);
        if (cancelled) cancelled_at_[key] = input_serial_;
        else cancelled_at_.erase(key);
    }
    int blocked() const { return blocked_; }   // number of suppressed re-triggers (diagnostics / tests)

private:
    uint64_t input_serial_ = 1;
    std::set<std::string> open_;
    std::map<std::string, uint64_t> cancelled_at_;   // dialog -> input serial at its last Cancel
    int blocked_ = 0;
};

} // namespace pcbp
