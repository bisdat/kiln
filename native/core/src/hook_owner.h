// SPDX-License-Identifier: MIT
// A hook link's owner as kiln.log names it next to the hook, in the lines about another module's hook on the same
// function (hooks_chain.cpp, compat_overlaps.cpp): "content/ItemRegistryFind (kiln-content)". The sigdb name of the
// address is not used there: several symbols can name one function, and the alphabetically first one is often another
// service's (build/ItemFind for kiln-content's hook on ItemRegistryFind, GitHub issue 39).
#pragma once
#include <string>
#include <string_view>

namespace kiln {

// "service:kiln-content.dll" -> "kiln-content", "plugin:kiln-emf.dll" -> "kiln-emf"; other owners as they are
// ("emf:coolmod", "native:fps-unlock", "kiln").
inline std::string hook_owner_text(std::string_view owner) {
    for (std::string_view prefix : {std::string_view("service:"), std::string_view("plugin:")})
        if (owner.substr(0, prefix.size()) == prefix) {
            owner.remove_prefix(prefix.size());
            break;
        }
    if (owner.size() > 4) {
        std::string tail(owner.substr(owner.size() - 4));
        for (char& c : tail) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        if (tail == ".dll") owner.remove_suffix(4);
    }
    return std::string(owner);
}

// One link as those lines name it: "<label> (<owner>)", or the label alone without an owner.
inline std::string hook_link_text(const std::string& label, std::string_view owner) {
    const std::string who = hook_owner_text(owner);
    return who.empty() ? label : label + " (" + who + ")";
}

}  // namespace kiln
