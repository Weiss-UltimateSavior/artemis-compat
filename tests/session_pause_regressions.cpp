// The same SessionPause state machine is used by the NativeActivity loop.
// Real Lua drag callbacks and synthetic hit geometry need no GL or game files.
#include "jni/session_pause.h"
#include "render/compositor.h"
#include "pack/pack_manager.h"
#include <cstdlib>
#include <iostream>

namespace {
void Check(bool ok, const char* why) {
    if (!ok) { std::cerr<<why<<'\n';std::exit(1); }
}
void Add(artc::Compositor& compositor,const char* id,int x,bool draggable) {
    compositor.SetProps(id,{{"left",std::to_string(x)},{"top","0"},{"w","100"},{"h","100"}});
    for(auto& l:const_cast<std::vector<artc::Layer>&>(compositor.Layers()))
        if(l.id==id){l.texture=1;l.draggable=draggable;}
}
}
int main() {
    artc::PackManager packs;artc::Compositor compositor;compositor.Init(640,480);
    Add(compositor,"1",0,true);Add(compositor,"2",160,false);Add(compositor,"3",320,true);
    artc::LuaEngine lua;
    Check(lua.Init(&packs,artc::Ini{},"android",640,480,&compositor),"initialize drag fixture");
    Check(lua.DoString(R"(
        drags=0; outs=0; clicks=0; gl_ready=true
        function moved(e,p) drags=drags+1 end
        function released(e,p) assert(gl_ready);outs=outs+1 end
        function clicked(e,p) clicks=clicks+1 end
        e:lyevent{id='1',type='drag',['function']='moved'}
        e:lyevent{id='1',type='dragout',['function']='released'}
        e:lyevent{id='2',type='click',['function']='clicked'}
    )","drag callbacks"),"register actual Lua callbacks");
    artc::SessionPause pause;
    pause.BeginFrame(lua,false);
    lua.PushKeyDown(1);lua.SetTouchCount(1);lua.BeginDrag(50,50);lua.DragMove(60,50);
    Check(lua.DragActive() && lua.DragMoved(),"a real moved slider is active");
    Check(lua.DoString("gl_ready=false","lose drawable surface"),"mark no GL context");
    pause.BeginFrame(lua,true);pause.BeginFrame(lua,true);
    Check(lua.DragActive(),"suspend defers Lua dragout until GL can be used");
    Check(lua.DoString("assert(outs==0 and not e:isDown(1) and not e:isDecide(1) and e:getTouchCount()==0)","paused input"),
          "pause drops physical state without a release decision or Lua callback");
    Check(lua.DoString("gl_ready=true","restore drawable surface"),"mark GL current");
    pause.BeginFrame(lua,false);pause.BeginFrame(lua,false);
    Check(!lua.DragActive() && !lua.DragMoved(),"resume clears grab and moved marker");
    Check(lua.DoString("assert(outs==1 and clicks==0 and not e:isDecide(1))","resume callback"),
          "cancel emits one dragout and no synthetic click edge");
    const float old_left=compositor.GetLayerInfo("1").left;
    lua.DragMove(90,50);
    Check(compositor.GetLayerInfo("1").left==old_left,"isolated next MOVE cannot move the old slider");
    // Follow the host's normal DOWN/UP path on a non-draggable button.
    lua.PushKeyDown(1);lua.BeginDrag(210,50);lua.PushKeyUp(1);
    Check(!lua.DragMoved(),"ordinary tap does not inherit the cancelled drag marker");
    if(!lua.DragMoved())lua.ClickAt(210,50);
    lua.RunEnterFrame();lua.EndDrag();lua.EndFrame();
    Check(lua.DoString("assert(clicks==1 and outs==1)","fresh tap"),"new ordinary tap remains selectable");
    lua.BeginDrag(370,50);lua.DragMove(380,50);
    Check(lua.DragActive() && compositor.GetLayerInfo("3").left==330,"a new slider can be grabbed and moved");
    lua.CancelDrag();
    // Actual context loss never supplies the unpaused/drawable edge.
    lua.BeginDrag(old_left+50,50);
    lua.RequestReset();
    Check(lua.DoString("gl_ready=false","context lost"),"simulate a lost context");
    pause.BeginFrame(lua,true);pause.BeginFrame(lua,true);
    Check(lua.DoString("assert(outs==1)","lost context"),"lost context does not run dragout");
    // Request reset during an active grab, then suspend before it is handled.
    // Use the production entry point, not a manual helper.Reset() shortcut.
    Check(!pause.BeginFrame(lua,true),"pause preserves a pending reset");
    Check(lua.DoString("gl_ready=true","restore for reset"),"GL is usable for reset");
    Check(pause.BeginFrame(lua,false),"drawable resume handles reset before old dragout");
    Check(lua.DoString("assert(outs==1)","reset without old callback"),
          "reset never dispatches the previous VM's deferred dragout");
    Check(!lua.ConsumeResetRequest(),"reset request is consumed exactly once");
    artc::LuaEngine replacement;
    Check(replacement.Init(&packs,artc::Ini{},"android",640,480,&compositor),"create reset session");
    Check(replacement.DoString(R"(
        outs=0
        function released(e,p) outs=outs+1 end
        e:lyevent{id='3',type='dragout',['function']='released'}
    )","replacement callbacks"),"register replacement callback");
    replacement.BeginDrag(380,50);Check(replacement.DragActive(),"replacement has its own grab");
    pause.BeginFrame(replacement,false);
    Check(replacement.DragActive() && replacement.DoString("assert(outs==0)","no stale cancel"),
          "reset discards cancellation intent belonging to the previous VM");
    replacement.CancelDrag();
    std::cout<<"session pause regressions passed\n";
}
