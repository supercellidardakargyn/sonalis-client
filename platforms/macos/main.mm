#import <AppKit/AppKit.h>

#include "sonalis/core/c_api.h"

@interface SonalisDelegate : NSObject <NSApplicationDelegate>
@property(nonatomic, strong) NSWindow* window;
@end

@implementation SonalisDelegate
- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    (void)notification;
    NSRect frame = NSMakeRect(0, 0, 1240, 780);
    self.window = [[NSWindow alloc] initWithContentRect:frame
        styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                   NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable)
        backing:NSBackingStoreBuffered defer:NO];
    self.window.title = @"Sonalis";
    self.window.minSize = NSMakeSize(960, 640);

    NSStackView* root = [[NSStackView alloc] initWithFrame:frame];
    root.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    root.spacing = 0;
    root.edgeInsets = NSEdgeInsetsMake(18, 18, 18, 18);
    NSStackView* sidebar = [NSStackView stackViewWithViews:@[
        [NSTextField labelWithString:@"SONALIS"],
        [NSButton buttonWithTitle:@"Ana sayfa" target:nil action:nil],
        [NSButton buttonWithTitle:@"Odalar" target:nil action:nil],
        [NSButton buttonWithTitle:@"Mesajlar" target:nil action:nil],
        [NSButton buttonWithTitle:@"Ayarlar" target:nil action:nil]
    ]];
    sidebar.orientation = NSUserInterfaceLayoutOrientationVertical;
    sidebar.spacing = 10;
    [sidebar.widthAnchor constraintEqualToConstant:240].active = YES;
    NSTextField* status = [NSTextField labelWithString:@"Sonalis macOS · ortak çekirdek hazır"];
    [root addArrangedSubview:sidebar];
    [root addArrangedSubview:status];
    self.window.contentView = root;
    [self.window center];
    [self.window makeKeyAndOrderFront:nil];
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
    (void)sender;
    return NO;
}
@end

int main(int argc, const char* argv[]) {
    (void)argc; (void)argv;
    if (sonalis_core_abi_version() != SONALIS_CORE_ABI_VERSION) return 70;
    @autoreleasepool {
        NSApplication* app = [NSApplication sharedApplication];
        SonalisDelegate* delegate = [SonalisDelegate new];
        app.delegate = delegate;
        [app run];
    }
    return 0;
}
