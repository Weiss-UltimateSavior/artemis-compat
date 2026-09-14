// mac_main.mm — native macOS host for the compat engine.
//
// Opens a Cocoa window with a legacy (OpenGL 2.1) context, boots the pack
// chain exactly like the Android host (packs -> system.ini -> Lua ->
// system/first.iet -> native .asb runner) and drives the per-frame loop on an
// NSTimer. Mouse/keyboard events feed the same normalized input model as the
// Android JNI host, so the button/lyevent flow behaves identically.
//
//   artemis-mac <game-dir|pack.pfs> [--os windows|android]
//
// The compositor is compiled with ARTC_HAS_GLES for this target; the GLES2
// calls are mapped onto desktop GL by render/gles2_headers.h and the GLSL ES
// sources are adapted by render/shader_compat.h.

#define GL_SILENCE_DEPRECATION 1
#import <Cocoa/Cocoa.h>

#include "config/ini.h"
#include "log/logger.h"
#include "pack/pack_manager.h"
#include "render/compositor.h"
#include "render/gles2_headers.h"
#include "script/asb_parser.h"
#include "script/iet_interpreter.h"
#include "script/lua_engine.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace artc;

namespace {

namespace fs = std::filesystem;

constexpr int kKeyTap = 1;

// Set while a native [dialog] input box is up so the frame timer does not
// re-enter the engine from inside the nested modal run loop.
BOOL g_dialog_open = NO;

std::string ToLower(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower((unsigned char)c));
    return s;
}

bool EndsWith(const std::string &s, const std::string &suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// A directory holding root.pfs (and its patch chain), or a direct .pfs path.
std::string ResolvePack(const std::string &path) {
    if (!fs::is_directory(path)) return path;
    std::vector<std::string> packs;
    for (const auto &e : fs::directory_iterator(path)) {
        if (e.is_regular_file() && EndsWith(ToLower(e.path().filename().string()), ".pfs"))
            packs.push_back(e.path().string());
    }
    if (packs.empty()) return {};
    std::sort(packs.begin(), packs.end(), [](const std::string &a, const std::string &b) {
        const bool ra = EndsWith(ToLower(a), "/root.pfs");
        const bool rb = EndsWith(ToLower(b), "/root.pfs");
        if (ra != rb) return ra;
        return a < b;
    });
    return packs.front();
}

struct Engine {
    struct RawInput {
        bool is_key = false;
        bool is_move = false;
        int key = 0;
        bool down = false;
        float x = 0, y = 0; // view point coordinates (top-left origin)
    };

    artc::PackManager packs;
    artc::Ini ini;
    artc::Compositor compositor;
    artc::AsbRunner runner;
    std::unique_ptr<artc::LuaEngine> lua;
    int stage_w = 1280, stage_h = 720;
    std::string os_name = "windows";
    std::string save_dir;
    bool booted = false;
    bool exit_requested = false;

    // view metrics: points for input mapping, backing pixels for glViewport
    float pt_w = 1280, pt_h = 720;
    float fb_w = 1280, fb_h = 720;

    std::mutex mutex;
    std::deque<RawInput> events;

    bool Open(const std::string &game_path, const std::string &os) {
        os_name = os;
        const std::string pack = ResolvePack(game_path);
        if (pack.empty()) {
            artc::Log(kLogError, "no .pfs pack at " + game_path);
            return false;
        }
        save_dir = fs::path(pack).parent_path().string();
        if (!packs.OpenChain(pack, {})) {
            artc::Log(kLogError, "cannot open pack chain: " + pack);
            return false;
        }
        std::vector<uint8_t> ini_bytes;
        if (packs.Read("system.ini", ini_bytes))
            ini.Parse(std::string(ini_bytes.begin(), ini_bytes.end()));
        int w = ini.GetInt("ANDROID", "WIDTH", 0);
        int h = ini.GetInt("ANDROID", "HEIGHT", 0);
        if (w <= 0 || h <= 0) {
            w = ini.GetInt("WINDOWS", "WIDTH", 1280);
            h = ini.GetInt("WINDOWS", "HEIGHT", 720);
        }
        stage_w = std::max(1, w);
        stage_h = std::max(1, h);
        artc::Log(kLogInfo, "mac host: pack=" + pack + " stage=" +
                                std::to_string(stage_w) + "x" + std::to_string(stage_h));
        return true;
    }

    bool Boot() {
        compositor.ReleaseGl();
        compositor.SetPackManager(&packs);
        compositor.Init(stage_w, stage_h);
        compositor.SetPresent(nullptr);

        lua = std::make_unique<artc::LuaEngine>();
        lua->SetSaveDir(save_dir);
        if (!lua->Init(&packs, ini, os_name, stage_w, stage_h, &compositor)) {
            artc::Log(kLogError, "lua init failed");
            lua.reset();
            return false;
        }
        // Native [dialog] host box: message-only alert, yes/no confirm, or
        // text input depending on the tag attributes set by the framework.
        lua->SetDialogHandler([](artc::DialogRequest &req) -> bool {
            g_dialog_open = YES;
            bool accepted = false;
            @autoreleasepool {
                NSAlert *alert = [[NSAlert alloc] init];
                alert.messageText = [NSString
                    stringWithUTF8String:req.title.empty() ? "Notice" : req.title.c_str()];
                if (!req.message.empty())
                    alert.informativeText = [NSString stringWithUTF8String:req.message.c_str()];
                if (req.has_input) {
                    [alert addButtonWithTitle:@"OK"];
                    [alert addButtonWithTitle:@"Cancel"];
                    NSTextField *field = [[NSTextField alloc]
                        initWithFrame:NSMakeRect(0, 0, 280, 24)];
                    [alert setAccessoryView:field];
                    [alert.window setInitialFirstResponder:field];
                    const NSModalResponse r = [alert runModal];
                    if (r == NSAlertFirstButtonReturn) {
                        accepted = true;
                        std::string s = field.stringValue ? [field.stringValue UTF8String] : "";
                        if (req.textfieldsize > 0 && s.size() > (size_t)req.textfieldsize)
                            s.resize(req.textfieldsize);
                        req.text = s;
                    }
                } else if (req.has_result) {
                    [alert addButtonWithTitle:@"OK"];
                    [alert addButtonWithTitle:@"Cancel"];
                    accepted = ([alert runModal] == NSAlertFirstButtonReturn);
                } else {
                    [alert addButtonWithTitle:@"OK"];
                    [alert runModal];
                    accepted = true;  // message-only: dismissed
                }
            }
            req.accepted = accepted;
            g_dialog_open = NO;
            return accepted;
        });
        artc::IetRunner iet(&packs, lua.get());
        if (!iet.Run("system/first.iet")) {
            artc::Log(kLogError, "system/first.iet missing; boot aborted");
            lua.reset();
            return false;
        }
        runner = artc::AsbRunner();
        runner.SetPackSource(&packs);
        lua->SetScriptRunner(&runner);
        artc::AsbRunner *r = &runner;
        lua->SetJumpHandler([r](const std::string &file, const std::string &label) {
            r->Jump(file, label);
        });
        lua->SetCallHandler([r](const std::string &file, const std::string &label) {
            r->Call(file, label);
        });
        lua->SetStopHandler([r](const std::string &tag) {
            if (tag == "stop") { r->Halt(); return; }
            if (!r->Return()) r->Halt();
        });
        booted = true;
        artc::Log(kLogInfo, "mac host: boot sequence finished");
        return true;
    }

    void Enqueue(const RawInput &ev) {
        std::lock_guard<std::mutex> lk(mutex);
        events.push_back(ev);
        if (events.size() > 512) events.pop_front();
    }

    void Pointer(float x, float y, bool move, bool down) {
        RawInput ev;
        ev.x = x;
        ev.y = y;
        ev.is_move = move;
        ev.down = down;
        ev.key = kKeyTap;
        Enqueue(ev);
    }

    void Key(int key, bool down) {
        RawInput ev;
        ev.is_key = true;
        ev.key = key;
        ev.down = down;
        Enqueue(ev);
    }

    void MapPoint(float px, float py, float *sx, float *sy) const {
        const float s = std::min(pt_w / stage_w, pt_h / stage_h);
        const float vw = stage_w * s, vh = stage_h * s;
        const float vx = (pt_w - vw) * 0.5f, vy = (pt_h - vh) * 0.5f;
        *sx = (px - vx) / vw * stage_w;
        *sy = (py - vy) / vh * stage_h;
    }

    void DrainInput() {
        std::deque<RawInput> batch;
        {
            std::lock_guard<std::mutex> lk(mutex);
            batch.swap(events);
        }
        if (!lua) return;
        int touch = 0;
        bool tapped = false;
        float tap_x = 0, tap_y = 0;
        for (const auto &ev : batch) {
            if (ev.is_key) {
                if (ev.down) lua->PushKeyDown(ev.key);
                else lua->PushKeyUp(ev.key);
                continue;
            }
            float sx = 0, sy = 0;
            MapPoint(ev.x, ev.y, &sx, &sy);
            if (ev.is_move) {
                lua->SetMousePoint(sx, sy);
                lua->DragMove(sx, sy);
                continue;
            }
            lua->SetMousePoint(sx, sy);
            if (ev.down) {
                lua->PushKeyDown(kKeyTap);
                touch = 1;
                lua->BeginDrag(sx, sy);
            } else {
                lua->PushKeyUp(kKeyTap);
                touch = 0;
                const bool dragging = lua->DragMoved();
                lua->EndDrag();
                if (!dragging) { tapped = true; tap_x = sx; tap_y = sy; }
            }
            lua->SetTouchCount(touch);
        }
        if (tapped && lua) lua->ClickAt(tap_x, tap_y);
    }

    void DrainQueued() {
        while (lua && !lua->IsWaiting() && lua->HasQueuedTag()) {
            std::string name;
            std::vector<std::pair<std::string, std::string>> attrs;
            if (!lua->PopQueuedTag(&name, &attrs)) break;
            if (name == "jump" || name == "call") {
                std::string file, label;
                for (const auto &kv : attrs) {
                    if (kv.first == "file") file = kv.second;
                    else if (kv.first == "label") label = kv.second;
                }
                if (name == "call") runner.Call(file, label);
                else runner.Jump(file, label);
            } else {
                lua->DispatchTag(name, attrs, false);
            }
        }
    }

    void Step() {
        if (!lua) return;
        if (runner.Loaded() && !runner.Halted() && !lua->IsWaiting()) {
            for (int i = 0; i < 4 && runner.Loaded() && !runner.Halted(); ++i) {
                runner.ExecuteLine(*lua);
                DrainQueued();
                if (lua->IsWaiting()) break;
            }
        } else if (!lua->IsWaiting() && lua->HasQueuedTag()) {
            DrainQueued();
        }
    }

    void Present() {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glViewport(0, 0, (GLsizei)fb_w, (GLsizei)fb_h);
        glClearColor(0.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        const float s = std::min(fb_w / stage_w, fb_h / stage_h);
        const int vw = (int)(stage_w * s), vh = (int)(stage_h * s);
        const int vx = ((int)fb_w - vw) / 2, vy = ((int)fb_h - vh) / 2;
        glViewport(vx, vy, vw, vh);
        compositor.Draw();
    }

    void Tick(float backing_w, float backing_h, float points_w, float points_h) {
        fb_w = backing_w;
        fb_h = backing_h;
        pt_w = points_w;
        pt_h = points_h;

        if (lua && lua->ConsumeResetRequest()) {
            artc::Log(kLogInfo, "mac host: reset requested; re-running boot chain");
            lua.reset();
            booted = false;
        }
        if (lua && lua->ConsumeExitRequest()) {
            artc::Log(kLogInfo, "mac host: exit requested");
            exit_requested = true;
            return;
        }
        if (!booted && !Boot()) {
            exit_requested = true;
            return;
        }
        DrainInput();
        if (lua) {
            lua->RunEnterFrame();
            DrainQueued();
        }
        Step();
        if (lua) compositor.Update(lua->NowMs());
        Present();
        if (lua) lua->EndFrame();
    }
};

Engine *g_engine = nullptr;

int KeyCodeForEvent(NSEvent *event) {
    switch (event.keyCode) {
        case 53: return 8;   // Escape -> BS/MWOFF
        case 51: return 8;   // Delete (backspace)
        case 36:             // Return
        case 76: return 13;  // keypad Enter
        case 123: return 37; // left
        case 124: return 39; // right
        case 125: return 40; // down
        case 126: return 38; // up
        case 49: return 32;  // space
        default: return 0;
    }
}

} // namespace

// ---------------------------------------------------------------------------

@interface ArctView : NSOpenGLView
@end

@implementation ArctView

- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)becomeFirstResponder { return YES; }

- (void)prepareOpenGL {
    [super prepareOpenGL];
    GLint one = 1;
    [[self openGLContext] setValues:&one
                       forParameter:NSOpenGLContextParameterSwapInterval];
}

- (void)tick:(NSTimer *)timer {
    if (!g_engine) return;
    if (g_dialog_open) return;  // modal input box owns the run loop
    [[self openGLContext] makeCurrentContext];
    const NSRect bounds = [self bounds];
    const NSRect backing = [self convertRectToBacking:bounds];
    g_engine->Tick((float)backing.size.width, (float)backing.size.height,
                   (float)bounds.size.width, (float)bounds.size.height);
    [[self openGLContext] flushBuffer];
    if (g_engine->exit_requested) [NSApp terminate:nil];
}

- (void)sendPointer:(NSEvent *)event down:(BOOL)down move:(BOOL)move {
    if (!g_engine) return;
    const NSPoint p = [self convertPoint:[event locationInWindow] fromView:nil];
    g_engine->Pointer((float)p.x, (float)p.y, move, down);
}

- (void)mouseDown:(NSEvent *)event { [self sendPointer:event down:YES move:NO]; }
- (void)mouseUp:(NSEvent *)event { [self sendPointer:event down:NO move:NO]; }
- (void)mouseDragged:(NSEvent *)event { [self sendPointer:event down:YES move:YES]; }
- (void)mouseMoved:(NSEvent *)event { [self sendPointer:event down:NO move:YES]; }

// Right button = the framework's "back/return" key (id 2, MWOFF/RCLICK),
// which returns from the settings/manual/backlog/save screens.
- (void)rightMouseDown:(NSEvent *)event { if (g_engine) g_engine->Key(2, true); }
- (void)rightMouseUp:(NSEvent *)event { if (g_engine) g_engine->Key(2, false); }
// Middle button = key 4.
- (void)otherMouseDown:(NSEvent *)event { if (g_engine) g_engine->Key(4, true); }
- (void)otherMouseUp:(NSEvent *)event { if (g_engine) g_engine->Key(4, false); }

- (void)keyDown:(NSEvent *)event {
    const int key = KeyCodeForEvent(event);
    if (key > 0 && g_engine) g_engine->Key(key, true);
}
- (void)keyUp:(NSEvent *)event {
    const int key = KeyCodeForEvent(event);
    if (key > 0 && g_engine) g_engine->Key(key, false);
}

@end

// ---------------------------------------------------------------------------

@interface ArctAppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@end
@implementation ArctAppDelegate
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app {
    return YES;
}
@end

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc < 2) {
            std::fprintf(stderr,
                         "usage: artemis-mac <game-dir|pack.pfs> [--os windows|android]\n");
            return 2;
        }
        std::string game_path = argv[1];
        std::string os = "windows";
        for (int i = 2; i < argc; ++i) {
            if (std::strcmp(argv[i], "--os") == 0 && i + 1 < argc) os = argv[++i];
        }

        auto engine = std::make_unique<Engine>();
        if (!engine->Open(game_path, os)) return 1;
        g_engine = engine.get();

        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        ArctAppDelegate *delegate = [[ArctAppDelegate alloc] init];
        [NSApp setDelegate:delegate];

        const NSRect frame = NSMakeRect(0, 0, engine->stage_w, engine->stage_h);
        NSWindow *window = [[NSWindow alloc]
            initWithContentRect:frame
                      styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                 NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable)
                        backing:NSBackingStoreBuffered
                          defer:NO];
        [window setTitle:@"artemis-compat"];
        [window setDelegate:delegate];

        NSOpenGLPixelFormatAttribute attrs[] = {
            NSOpenGLPFADoubleBuffer,
            NSOpenGLPFAAccelerated,
            NSOpenGLPFAColorSize, 24,
            NSOpenGLPFAAlphaSize, 8,
            NSOpenGLPFAOpenGLProfile, NSOpenGLProfileVersionLegacy,
            0};
        NSOpenGLPixelFormat *pixel_format =
            [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs];
        ArctView *view = [[ArctView alloc] initWithFrame:frame pixelFormat:pixel_format];
        [window setContentView:view];
        [window makeFirstResponder:view];
        [window center];
        [window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        [window setAcceptsMouseMovedEvents:YES];

        NSTimer *timer = [NSTimer timerWithTimeInterval:1.0 / 60.0
                                                 target:view
                                               selector:@selector(tick:)
                                               userInfo:nil
                                                repeats:YES];
        [[NSRunLoop currentRunLoop] addTimer:timer forMode:NSRunLoopCommonModes];
        [NSApp run];
    }
    return 0;
}
