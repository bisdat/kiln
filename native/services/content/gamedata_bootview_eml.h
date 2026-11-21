// SPDX-License-Identifier: MIT
// kiln.content's base view in a kiln-backend start (kiln.launch KILN). A boot generation's resource patches
// are checked before the start (gamedata_bootview.h, gamedata_bootpatch.cpp check_base) against the archive generation
// kiln-eml's startup pass committed for this launch, read through kiln.eml.archive with kiln.boot's own committed view
// (native/core/src/services/boot_archive_eml.h): the same binding rules, pins and translation as OnBoot's reads, so a patch
// is checked against exactly the data its author could read. The boot snapshot is never read. A hosted or installed
// start, or a kiln-backend start whose generation cannot be bound, gets no view: the bytes decide when the patches are
// written, as before.
//
// Kiln-internal, with kiln.eml.archive and kiln.launch.
//
// The ShroudForge-shaped rules:
//  - a list the startup pass made longer is checked as the generation has it: an OnBoot patch of a new index passes
//    (the boot snapshot, made before the start, would have refused it);
//  - a refusal needs the source to say its view of the resource is final for this start (final_for), and this source
//    gives no such answer yet: Kiln, a boot mod, a data pack or another mod's assets.json may create or override a
//    resource in the same start, and the archive holds none of those, so every patch is checked when it is written, as
//    in a hosted start. With such an answer, a target the pass changed (a list made shorter, another variant, a resource
//    it has not got) refuses the boot mod's patch before the start: kiln.boot leaves an optional mod out whole and stops
//    the start for a required one, before the game's entry point;
//  - only boot mods are refused or made required: a ShroudForge package the pass skipped (skip-and-continue) is no boot
//    owner here, is never named in a refusal and makes nothing required; the boot mod whose patch needed its data is the
//    one refused, by its own "boot" rule.
//
// The view is bound at the first prepare that asks (kiln-eml's startup pass ran in its on_ready, before kiln.boot's
// commit) and released once by apply, when the generation is published and the game's resources are up.
// Header-only, host-tested (tests/bootview_eml_test.cpp, against a fake kiln.eml.archive table).
// Threads: view() on kiln.boot's boot thread, finished() once from apply; the backing's lock guards its state and is
// never held across a call into kiln-eml's reads or the view's release.
#pragma once
#include <mutex>
#include <string>

#include "../../core/src/services/boot_archive_eml.h"
#include "gamedata_bootview.h"

namespace kc::bootview_eml {

/// What a kiln-backend start hands kiln.content (gamedata_bootview_eml.cpp; the host tests their own).
struct Inputs {
    bool (*kiln_mode)(kiln::boot_view::LaunchFacts* out) = nullptr;  // this start runs the kiln backend; *out its launch facts
    const KilnEmlArchiveApiV1* (*table)() = nullptr;                // kiln.eml.archive (kiln-eml's early export), null without it
    kiln::boot_view::IdentitySource identity = nullptr;             // the committed generation's identity
    bool (*row)(const char* name, std::string* text) = nullptr;     // a kiln.caps row is available; else *text says how it is
    KilnOwner (*owner)() = nullptr;                                 // kiln.content's owner, the view's acquirer
};

/// kiln_eml_archive.h: a consumer checks the group and member rows it uses before it uses the table; a held, off or
/// absent row refuses. kiln.content uses acquire, identity, release and resource_json, as kiln.boot does.
inline constexpr const char* kArchiveRows[] = {"eml.archive.metadata",         "eml.archive.metadata.acquire", "eml.archive.metadata.identity",
                                               "eml.archive.metadata.release", "eml.archive.read",             "eml.archive.read.resource_json"};

class Backing {
   public:
    explicit Backing(const Inputs* in) : in_(in) {}

    /// The committed generation's view, bound at the first call; null with *why when this start has none.
    kiln::boot_view::BaseView* view(std::string* why) {
        std::lock_guard bl(bind_);  // one binding at a time; kiln.boot's prepares are serial anyway
        {
            std::lock_guard lk(m_);
            if (decided_) return bound_ ? &view_ : (*why = why_, nullptr);
        }
        std::string reason;
        const bool bound = decide(&reason);
        std::lock_guard lk(m_);
        if (decided_) {  // finished() came first: nothing of this binding is served
            if (bound) view_.release();
            return *why = why_, nullptr;
        }
        decided_ = true;
        bound_ = bound;
        why_ = reason;
        return bound ? &view_ : (*why = why_, nullptr);
    }

    /// The checks are over: the view (if any) is released, and no later call binds one.
    void finished() {
        bool release = false;
        {
            std::lock_guard lk(m_);
            release = bound_;
            bound_ = false;
            decided_ = true;
            why_ = "the boot generation's checks are over";
        }
        if (release) view_.release();  // waits for a read in flight (none: prepares are over)
    }

   private:
    bool decide(std::string* why) {
        kiln::boot_view::LaunchFacts launch;
        if (!in_ || !in_->kiln_mode || !in_->kiln_mode(&launch))
            return *why = "not a kiln-backend start (the archive's writer of this start is not kiln-eml's startup pass)", false;
        const KilnEmlArchiveApiV1* api = in_->table ? in_->table() : nullptr;
        if (api)  // without a table bind() names that
            for (const char* row : kArchiveRows) {
                std::string text;
                if (!in_->row || !in_->row(row, &text)) {
                    std::string w;
                    view_.refuse(std::string("kiln.caps row ") + row + " is not available" + (text.empty() ? "" : " (" + text + ")"), &w);
                    return *why = "a kiln-backend start without its committed archive generation: " + w, false;
                }
            }
        std::string w;
        if (!view_.bind(in_->owner ? in_->owner() : 0, launch, api, in_->identity, &w))
            return *why = "a kiln-backend start without its committed archive generation: " + w, false;
        why->clear();
        return true;
    }

    const Inputs* in_;
    std::mutex bind_, m_;
    bool decided_ = false, bound_ = false;
    std::string why_;
    kiln::boot_view::CommittedView view_;
};

}  // namespace kc::bootview_eml
