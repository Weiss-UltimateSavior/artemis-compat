// dialog_request.h — [dialog] host-modal box request (name entry / confirm).
//
// Extracted from lua_engine.h so hosts (jni / mac) can consume the struct
// without dragging in the Lua engine header.
#pragma once
#include <string>

namespace artc {

// [dialog ... textfield=… textfieldsize=… varname=…] — a host-modal box.
// Three framework variants share this tag:
//   message only (no textfield/varname)  -> OK alert
//   varname present                      -> yes/no confirm (result -> varname)
//   textfield present                    -> text input (text -> textfield var)
struct DialogRequest {
    std::string title;
    std::string message;
    std::string textfield;   // variable that receives the entered text
    int textfieldsize = 0;   // max characters (0 = host default)
    bool has_input = false;  // textfield present
    bool has_result = false; // varname present (yes/no confirm)
    std::string text;        // filled by the host
    bool accepted = false;   // filled by the host (OK / confirm-yes / dismissed)
};

} // namespace artc
