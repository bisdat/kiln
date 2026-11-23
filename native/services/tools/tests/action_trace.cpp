// SPDX-License-Identifier: MIT
// The tool action trace on the host (kiln#133): every exit is named and counted, a stroke the game ran is classified
// from what its terrain kernel saw, and the trace reports a change of state once, however many strokes repeat it.
// No game, no hooks.
#include "../action_trace.h"
#include <cstdlib>
#include <iostream>
#include <thread>
using namespace kiln::tools;
static unsigned checks;
#define CHECK(x) do {++checks;if(!(x)){std::cerr<<__LINE__<<": " #x "\n";std::exit(1);}}while(false)
namespace {
void names() {
    for(size_t i=0;i<size_t(ActionExit::Count);++i) {
        CHECK(std::string(action_exit_name(ActionExit(i)))!="unknown");
        CHECK(std::string(action_exit_meaning(ActionExit(i)))!="unknown");
        for(size_t j=0;j<i;++j)CHECK(std::string(action_exit_name(ActionExit(i)))!=action_exit_name(ActionExit(j)));
    }
    CHECK(!action_exit_dispatched(ActionExit::OutsideBase)&&!action_exit_dispatched(ActionExit::ContinuationFault));
    CHECK(action_exit_dispatched(ActionExit::GameStopped)&&action_exit_dispatched(ActionExit::Removed)&&action_exit_dispatched(ActionExit::Added));
}
void outcomes() {
    StrokeTally t;
    // Remove: the kernel never ran (the game's own checks stopped the stroke), or ran without an undo record inside a base.
    CHECK(stroke_outcome(KILN_TOOL_POLICY_REMOVE,t,0)==ActionExit::GameStopped);
    CHECK(stroke_outcome(KILN_TOOL_POLICY_REMOVE,t,6)==ActionExit::GameStopped);
    t.no_undo_record=true;CHECK(stroke_outcome(KILN_TOOL_POLICY_REMOVE,t,6)==ActionExit::NoUndoRecord);
    // The kernel ran: no allowed cell minable; allowed cells minable but none under the brush; something removed.
    t={};t.kernels=2;CHECK(stroke_outcome(KILN_TOOL_POLICY_REMOVE,t,0)==ActionExit::NotRemovable);
    t.removable=true;CHECK(stroke_outcome(KILN_TOOL_POLICY_REMOVE,t,0)==ActionExit::NothingMatched);
    t.measured=false;CHECK(stroke_outcome(KILN_TOOL_POLICY_REMOVE,t,0)==ActionExit::Carved);
    t.removed=true;CHECK(stroke_outcome(KILN_TOOL_POLICY_REMOVE,t,0)==ActionExit::Removed);
    t.brush_fault=true;CHECK(stroke_outcome(KILN_TOOL_POLICY_REMOVE,t,0)==ActionExit::ReadFault);
    // Add: the game's result decides; the kernel tally does not apply.
    t={};CHECK(stroke_outcome(KILN_TOOL_POLICY_ADD,t,0)==ActionExit::Added);
    CHECK(stroke_outcome(KILN_TOOL_POLICY_ADD,t,6)==ActionExit::GameStopped);
}
void tally() {
    // Notes outside a stroke go nowhere; inside, they add up; a nested stroke has its own tally and restores the outer one.
    note_kernel(true,1);note_no_undo_record();note_brush_fault();CHECK(stroke_tally==nullptr);
    StrokeTally outer;
    {
        StrokeScope a(&outer);
        note_kernel(false,0);
        CHECK(outer.kernels==1&&!outer.removable&&!outer.removed&&outer.measured);
        StrokeTally inner;
        {StrokeScope b(&inner);note_kernel(true,1);note_no_undo_record();}
        CHECK(stroke_tally==&outer&&inner.kernels==1&&inner.removable&&inner.removed&&inner.no_undo_record);
        note_kernel(true,-1);
        CHECK(outer.kernels==2&&outer.removable&&!outer.removed&&!outer.measured&&!outer.no_undo_record);
        note_brush_fault();CHECK(outer.kernels==3&&outer.brush_fault);
    }
    CHECK(stroke_tally==nullptr);
    // Thread-local: another thread's stroke never sees this one's tally.
    StrokeTally mine;StrokeScope here(&mine);
    std::thread([]{CHECK(stroke_tally==nullptr);note_kernel(true,1);}).join();
    CHECK(mine.kernels==0);
}
void trace() {
    ActionTrace t;ActionExit state{};uint64_t covered=0;
    CHECK(!t.any()&&!t.pending()&&!t.take(&state,&covered)&&t.summary()=="no tool strokes yet"&&t.detail_text().empty());
    // 300 strokes refused the same way: one state change, one publication.
    for(int i=0;i<300;++i)t.record(ActionExit::NotServerWorld,{0x1234,0,0,0xff});
    CHECK(t.count(ActionExit::NotServerWorld)==300&&t.changes()==1&&t.pending());
    CHECK(t.take(&state,&covered)&&state==ActionExit::NotServerWorld&&covered==1&&!t.pending());
    CHECK(!t.take(&state,&covered));
    CHECK(t.detail_text()==" (program 0x00001234)");
    // Vanilla terraforming is counted but never changes the state.
    for(int i=0;i<5;++i)t.other();
    CHECK(t.others()==5&&!t.pending()&&t.last()==ActionExit::NotServerWorld);
    t.record(ActionExit::Removed,{0x1234,0xb00b0001,0.5f,0});
    CHECK(t.take(&state,&covered)&&state==ActionExit::Removed&&covered==1);
    CHECK(t.detail_text()==" (program 0x00001234, UsedItem 0xb00b0001, radius 0.50 m, game result 0)");
    // Faster than the publisher: three changes become one publication that says so, naming the state now.
    t.record(ActionExit::NoUsedItem);t.record(ActionExit::RadiusMismatch,{0x1234,0xb00b0002,1.5f,0xff});t.record(ActionExit::OutsideBase);
    CHECK(t.take(&state,&covered)&&state==ActionExit::OutsideBase&&covered==3);
    const auto s=t.summary();
    CHECK(s.find("not the server world 300")!=std::string::npos&&s.find("removed 1")!=std::string::npos&&
          s.find("no UsedItem 1")!=std::string::npos&&s.find("radius mismatch 1")!=std::string::npos&&
          s.find("outside base 1")!=std::string::npos&&s.find("; other programs 5")!=std::string::npos&&
          s.find("game stopped")==std::string::npos);
    CHECK(s.find("not the server world")<s.find("no UsedItem")&&s.find("no UsedItem")<s.find("removed")); // exit order
    // Out-of-range exits are ignored, not written past the counters.
    t.record(ActionExit::Count);t.record(ActionExit(200));CHECK(!t.pending()&&t.last()==ActionExit::OutsideBase);
    t.reset();CHECK(!t.any()&&t.count(ActionExit::Removed)==0&&t.others()==0&&!t.take(&state,&covered)&&t.detail_text().empty());
}
}
int main() {
    names();outcomes();tally();trace();
    std::cout<<"action trace: "<<checks<<" checks passed\n";
}
