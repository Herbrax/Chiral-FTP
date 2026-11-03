// macOS: the menu bar, the way any Mac app has one -- chiral-ftp, Edit, Go,
// View, Window and Help; SDL puts up a bare one of its own and this replaces
// it -- and the system's folder chooser for the server's shared folder.
//
// Every command lands in the same queue as the keys (platform::detail::
// command), so the UI handles a menu choice exactly like a key press, on
// its own thread, between frames.

#import <Cocoa/Cocoa.h>

#include <atomic>
#include <cstring>
#include <string>

#include "backend.h"
#include "i18n.h"

@interface ChiralMenuTarget : NSObject <NSMenuDelegate>
@end

@implementation ChiralMenuTarget
- (void)command:(NSMenuItem *)item {
  platform::detail::command((Action)item.tag);
}

/* Just before a menu opens: hide what the UI says has no place right now
   (platform::set_command_shown) -- on a computer, the file manager and the
   client unless Settings puts them in the window. */
- (void)menuNeedsUpdate:(NSMenu *)menu {
  for(NSMenuItem *item in menu.itemArray) {
    if(item.action == @selector(command:)) item.hidden = !platform::command_shown((Action)item.tag);
  }
}

/* Paste into the text entry that is open, if one is; the text goes the way
   typed characters do, so it is ignored when nothing is being typed. */
- (void)paste:(id)sender {
  (void)sender;
  NSString *text = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
  if(text.length) platform::detail::typed(text.UTF8String);
}

- (void)openRepository:(id)sender {
  (void)sender;
  [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:@"https://github.com/Herbrax/chiral-ftp"]];
}

- (void)reportBug:(id)sender {
  (void)sender;
  [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:@"https://github.com/Herbrax/chiral-ftp/issues"]];
}
@end

namespace {

ChiralMenuTarget *g_target = nil;

NSString *ns(const char *s) {
  return [NSString stringWithUTF8String:s];
}

/* tr(), except in Arabic. The catalogue stores Arabic already shaped and in
   drawing order for the app's own renderer (tools/gen-i18n.py); Cocoa lays
   out right-to-left text itself and would turn that back to front. */
const char *menu_text(const char *english) {
  return strcmp(i18n_current(), "ar") ? tr(english) : english;
}

NSMenu *submenu(NSMenu *bar, const char *title) {
  NSMenuItem *holder = [bar addItemWithTitle:ns(title) action:nil keyEquivalent:@""];
  NSMenu *menu = [[NSMenu alloc] initWithTitle:ns(title)];
  holder.submenu = menu;
  return menu;
}

// An item that sends one of the app's commands.
void command(NSMenu *menu, const char *title, Action a, NSString *key,
             NSEventModifierFlags mods = NSEventModifierFlagCommand) {
  NSMenuItem *item = [menu addItemWithTitle:ns(title) action:@selector(command:) keyEquivalent:key];
  item.keyEquivalentModifierMask = mods;
  item.target = g_target;
  item.tag = a;
}

// An item Cocoa itself answers: hide, minimise, full screen and the like.
NSMenuItem *standard(NSMenu *menu, const char *title, SEL action, NSString *key, id target,
                     NSEventModifierFlags mods = NSEventModifierFlagCommand) {
  NSMenuItem *item = [menu addItemWithTitle:ns(title) action:action keyEquivalent:key];
  item.keyEquivalentModifierMask = mods;
  item.target = target;
  return item;
}

}  // namespace

void macos_menu_install() {
  @autoreleasepool {
    g_target = [ChiralMenuTarget new];
    NSMenu *bar = [[NSMenu alloc] initWithTitle:@""];

    // The application menu's title is always the app's name, whatever it is given.
    NSMenu *app = submenu(bar, "chiral-ftp");
    command(app, menu_text(N_("About chiral-ftp")), ACT_CMD_ABOUT, @"", 0);
    [app addItem:[NSMenuItem separatorItem]];
    command(app, (std::string(menu_text(N_("Settings"))) + "\xe2\x80\xa6").c_str(), ACT_CMD_SETTINGS, @",");
    [app addItem:[NSMenuItem separatorItem]];
    standard(app, menu_text(N_("Hide chiral-ftp")), @selector(hide:), @"h", NSApp);
    standard(app, menu_text(N_("Hide Others")), @selector(hideOtherApplications:), @"h", NSApp,
             NSEventModifierFlagCommand | NSEventModifierFlagOption);
    standard(app, menu_text(N_("Show All")), @selector(unhideAllApplications:), @"", NSApp, 0);
    [app addItem:[NSMenuItem separatorItem]];
    // SDL turns terminate: into a quit event, so the app shuts down its own way.
    standard(app, menu_text(N_("Quit chiral-ftp")), @selector(terminate:), @"q", NSApp);

    NSMenu *edit = submenu(bar, menu_text(N_("Edit")));
    standard(edit, menu_text(N_("Paste")), @selector(paste:), @"v", g_target);

    NSMenu *go = submenu(bar, menu_text(N_("Go")));
    command(go, menu_text(N_("Start screen")), ACT_CMD_HOME, @"h", NSEventModifierFlagCommand | NSEventModifierFlagShift);
    [go addItem:[NSMenuItem separatorItem]];
    go.delegate = g_target;
    command(go, menu_text(N_("FTP / SFTP server")), ACT_CMD_FTPD, @"1");
    command(go, menu_text(N_("Web app")), ACT_CMD_WEB, @"2");
    [go addItem:[NSMenuItem separatorItem]];
    command(go, (std::string(menu_text(N_("Quick connect"))) + "\xe2\x80\xa6").c_str(), ACT_CMD_QUICK, @"k");

    NSMenu *view = submenu(bar, menu_text(N_("View")));
    command(view, menu_text(N_("Transfers")), ACT_CMD_QUEUE, @"t");
    command(view, menu_text(N_("Message log")), ACT_CMD_LOG, @"l");
    [view addItem:[NSMenuItem separatorItem]];
    command(view, menu_text(N_("Actual Size")), ACT_CMD_ZOOM_RESET, @"0");
    command(view, menu_text(N_("Zoom In")), ACT_CMD_ZOOM_IN, @"+");
    command(view, menu_text(N_("Zoom Out")), ACT_CMD_ZOOM_OUT, @"-");
    [view addItem:[NSMenuItem separatorItem]];
    // macOS retitles this "Exit Full Screen" itself while in full screen.
    standard(view, menu_text(N_("Enter Full Screen")), @selector(toggleFullScreen:), @"f", nil,
             NSEventModifierFlagCommand | NSEventModifierFlagControl);

    NSMenu *window = submenu(bar, menu_text(N_("Window")));
    standard(window, menu_text(N_("Minimize")), @selector(performMiniaturize:), @"m", nil);
    standard(window, menu_text(N_("Zoom")), @selector(performZoom:), @"", nil, 0);

    NSMenu *help = submenu(bar, menu_text(N_("Help")));
    standard(help, menu_text(N_("chiral-ftp on GitHub")), @selector(openRepository:), @"", g_target, 0);
    standard(help, menu_text(N_("Report a Bug")), @selector(reportBug:), @"", g_target, 0);

    NSApp.mainMenu = bar;
    NSApp.windowsMenu = window;
    NSApp.helpMenu = help;
  }
}

/* The standard Open panel, folders only. Modal: the window waits, the
   servers (on their own threads) don't. */
bool macos_choose_folder(const char *start, std::string &out) {
  @autoreleasepool {
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.canChooseDirectories = YES;
    panel.canChooseFiles = NO;
    panel.allowsMultipleSelection = NO;
    panel.canCreateDirectories = YES;
    panel.prompt = ns(menu_text(N_("Share")));
    if(start && *start) panel.directoryURL = [NSURL fileURLWithPath:ns(start) isDirectory:YES];
    if([panel runModal] != NSModalResponseOK || !panel.URL) return false;
    out = panel.URL.path.UTF8String;
    return true;
  }
}

/* ---------- updates (platform/sdl/update.cpp) ---------- */

/* Where this app is, "/Applications/chiral-ftp.app"; empty when the program
   isn't running from a bundle (a build run as ./chiral-ftp). */
std::string macos_bundle_path() {
  @autoreleasepool {
    NSString *path = [[NSBundle mainBundle] bundlePath];
    if(!path || ![path.pathExtension isEqualToString:@"app"]) return "";
    return path.UTF8String;
  }
}

/* The update API's question. Waits, at most timeout_s. */
bool macos_post_json(const std::string &url, const std::string &body, int timeout_s, long *status, std::string *reply) {
  @autoreleasepool {
    NSURL *u = [NSURL URLWithString:ns(url.c_str())];
    if(!u) return false;
    NSMutableURLRequest *req = [NSMutableURLRequest requestWithURL:u];
    req.HTTPMethod = @"POST";
    req.HTTPBody = [NSData dataWithBytes:body.data() length:body.size()];
    req.timeoutInterval = timeout_s;
    [req setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
    __block bool ok = false;
    __block long code = 0;
    __block std::string text;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    NSURLSessionDataTask *task = [[NSURLSession sharedSession]
        dataTaskWithRequest:req
          completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
            if(!error && [response isKindOfClass:[NSHTTPURLResponse class]]) {
              ok = true;
              code = (long)((NSHTTPURLResponse *)response).statusCode;
              if(data.length && data.length < 64 * 1024) text.assign((const char *)data.bytes, data.length);
            }
            dispatch_semaphore_signal(done);
          }];
    [task resume];
    if(dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(timeout_s + 2) * NSEC_PER_SEC))) {
      [task cancel];
      dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);  // the handler still runs, once
      return false;
    }
    *status = code;
    *reply = text;
    return ok;
  }
}

/* The release's file, into path. received follows the task as it goes;
   cancel stops it. */
bool macos_download(const std::string &url, const std::string &path, const std::atomic<bool> &cancel,
                    std::atomic<long long> &received) {
  @autoreleasepool {
    NSURL *u = [NSURL URLWithString:ns(url.c_str())];
    if(!u) return false;
    NSURLRequest *req = [NSURLRequest requestWithURL:u cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
                                     timeoutInterval:60];
    NSString *dest = ns(path.c_str());
    __block bool ok = false;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    /* The file the session downloads to is deleted when the handler returns,
       so it is moved into place in the handler. */
    NSURLSessionDownloadTask *task = [[NSURLSession sharedSession]
        downloadTaskWithRequest:req
              completionHandler:^(NSURL *location, NSURLResponse *response, NSError *error) {
                const long code = [response isKindOfClass:[NSHTTPURLResponse class]]
                                      ? (long)((NSHTTPURLResponse *)response).statusCode : 0;
                if(!error && location && code == 200) {
                  [[NSFileManager defaultManager] removeItemAtPath:dest error:nil];
                  ok = [[NSFileManager defaultManager] moveItemAtURL:location
                                                               toURL:[NSURL fileURLWithPath:dest]
                                                               error:nil];
                }
                dispatch_semaphore_signal(done);
              }];
    [task resume];
    while(dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC))) {
      received = task.countOfBytesReceived;
      if(cancel) [task cancel];  // the handler follows, with an error
    }
    received = task.countOfBytesReceived;
    return ok && !cancel;
  }
}
