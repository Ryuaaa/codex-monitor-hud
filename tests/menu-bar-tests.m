// Synthetic, offline AppKit checks. Does not launch the normal app lifecycle,
// read sessions, alter the running HUD, or invoke a real data provider.
#define main HUDProductionMain
#import "../overlay/CodexMonitorHUD.m"
#undef main

@interface MenuTestProvider : CodexStatusProvider
@property NSInteger starts;
@property NSInteger refreshes;
@end
@implementation MenuTestProvider
- (void)start { self.starts++; }
- (void)stop {}
- (void)refreshQuotaInBackground { self.refreshes++; }
@end
@interface MenuTestSampler : NativeSampler
@property NSInteger samples;
@end
@implementation MenuTestSampler
- (NativeSnapshot *)sample {
    self.samples++;
    NativeSnapshot *s = [NativeSnapshot new];
    s.timestamp = NSDate.date.timeIntervalSince1970;
    s.systemCPUPercent = 24; s.systemMemoryUsedPercent = 45;
    s.systemMemoryUsedGiB = 14.4; s.totalMemoryGiB = 32;
    s.memoryPressureText = @"SYNTHETIC normal"; s.thermalText = @"SYNTHETIC normal";
    s.topMemoryApps = @[];
    return s;
}
@end
@interface MenuTestPanel : NSPanel
@property BOOL presented;
@end
@implementation MenuTestPanel
- (void)orderOut:(id)sender { self.presented = NO; }
- (void)orderFront:(id)sender { self.presented = YES; }
- (void)orderFrontRegardless { self.presented = YES; }
@end
static NSUInteger assertions = 0;
static void Check(BOOL condition, NSString *name) {
    assertions++;
    if (!condition) { fprintf(stderr, "FAIL: %s\n", name.UTF8String); exit(1); }
}
static void ResolveTestTextColors(NSView *view) {
    if ([view isKindOfClass:NSTextField.class]) {
        NSTextField *label = (NSTextField *)view;
        label.textColor = [label.textColor colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
    }
    for (NSView *child in view.subviews) ResolveTestTextColors(child);
}
int main(int argc, const char *argv[]) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
        if (argc > 2) [NSUserDefaults.standardUserDefaults setVolatileDomain:@{@"displayLanguage": [NSString stringWithUTF8String:argv[2]]} forName:NSArgumentDomain];
        Check([HUDMenuMode(nil) isEqual:@"both"] && [HUDMenuMode(@"bad") isEqual:@"both"], @"safe default preserves HUD");
        Check([HUDMenuModeForVisibility(YES, YES) isEqual:@"both"], @"both visibility maps to both mode");
        Check([HUDMenuModeForVisibility(YES, NO) isEqual:@"menuBar"], @"menu-only visibility maps to menu mode");
        Check([HUDMenuModeForVisibility(NO, YES) isEqual:@"floating"], @"floating-only visibility maps to floating mode");
        Check(HUDMenuModeForVisibility(NO, NO) == nil, @"both-off visibility is rejected");
        Check([HUDMenuMetrics(nil) isEqual:@[@"weekly"]], @"default weekly");
        Check(HUDMenuMetrics(@[]).count == 0, @"icon only");
        Check([HUDMenuMetrics(@[@"cpu", @"cpu", @"bad", @"memory", @"weekly"]) isEqual:@[@"cpu", @"memory"]], @"ordered unique maximum two");
        NSTimeInterval now = NSDate.date.timeIntervalSince1970;
        MenuTestProvider *provider = [MenuTestProvider new];
        CodexStatusSnapshot *s = provider.snapshot;
        Check([HUDMenuQuotaValue(s, YES, now) isEqual:@"—"], @"unavailable not zero");
        s.weeklyAvailable = YES; s.weeklyRemainingPercent = 58; s.weeklyResetAt = now + 7200;
        s.quotaUpdatedAt = now; s.weeklyDataState = @"live";
        Check([HUDMenuQuotaValue(s, YES, now) isEqual:@"58%"], @"live weekly");
        s.quotaErrorText = @"SYNTHETIC failure";
        Check([HUDMenuQuotaValue(s, YES, now) isEqual:@"58%*"], @"failure marks last valid");
        s.quotaErrorText = nil; s.weeklyDataState = @"previous";
        Check([HUDMenuQuotaValue(s, YES, now) hasSuffix:@"*"], @"previous snapshot");
        s.weeklyDataState = @"live"; s.quotaUpdatedAt = now - 901;
        Check([HUDMenuQuotaValue(s, YES, now) hasSuffix:@"*"], @"age cutoff");
        s.quotaUpdatedAt = now; s.weeklyResetAt = now;
        Check([HUDMenuQuotaValue(s, YES, now) isEqual:@"—"], @"reset boundary");
        s.weeklyResetAt = now + 7200; s.weeklyRemainingPercent = NAN;
        Check([HUDMenuQuotaValue(s, YES, now) isEqual:@"—"], @"nonfinite rejected");
        s.weeklyRemainingPercent = 0;
        Check([HUDMenuQuotaValue(s, YES, now) isEqual:@"0%"], @"real exhausted quota");
        s.weeklyRemainingPercent = 58;
        Check([HUDMenuQuotaValue(s, NO, now) isEqual:@"—"], @"five-hour independent");
        MenuTestSampler *sampler = [MenuTestSampler new];
        NativeSnapshot *n = [sampler sample];
        Check([HUDMenuSystemValue(n, YES, now) isEqual:@"45%"], @"memory percentage");
        n.timestamp = now - 61;
        Check([HUDMenuSystemValue(n, NO, now) isEqual:@"—"], @"stale CPU not live");
        n.timestamp = now;

        AppDelegate *d = [AppDelegate new];
        d.displayMode = @"menuBar"; d.menuBarMetrics = @[@"weekly"];
        d.refreshInterval = 5; d.windowScale = 1; d.accentName = @"blue";
        d.codexProvider = provider; d.sampler = sampler; d.lastSnapshot = n;
        MenuTestPanel *panel = [[MenuTestPanel alloc] initWithContentRect:NSMakeRect(40, 50, 430, 600) styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:YES];
        d.panel = panel;
        d.hudView = [[HUDView alloc] initWithFrame:NSMakeRect(0, 0, 430, 600)];
        d.cpuHistory = [NSMutableArray array]; d.minuteSamples = [NSMutableArray array];
        d.displayMode = @"both";
        NSBox *displaySettings = [d menuBarSettingsGroup];
        NSButton *menuBarVisibility = FindButtonWithTitle(displaySettings, HUDL(@"显示顶部菜单栏"));
        NSButton *floatingVisibility = FindButtonWithTitle(displaySettings, HUDL(@"显示屏幕悬浮窗"));
        Check(menuBarVisibility.state == NSControlStateValueOn && floatingVisibility.state == NSControlStateValueOn, @"both display entries are checked together");
        Check(menuBarVisibility.enabled && floatingVisibility.enabled, @"both display entries can be changed independently");
        Check(menuBarVisibility.target == d && menuBarVisibility.action == @selector(toggleMenuBarVisibility:) && floatingVisibility.target == d && floatingVisibility.action == @selector(toggleFloatingVisibility:), @"display checkboxes are wired to their actions");
        d.displayMode = @"floating"; [d updateDisplayModeCheckboxes];
        Check(menuBarVisibility.state == NSControlStateValueOff && floatingVisibility.state == NSControlStateValueOn && !floatingVisibility.enabled, @"floating-only keeps the last entry checked");
        d.displayMode = @"menuBar"; [d updateDisplayModeCheckboxes];
        Check(menuBarVisibility.state == NSControlStateValueOn && !menuBarVisibility.enabled && floatingVisibility.state == NSControlStateValueOff, @"menu-only keeps the last entry checked");
        d.displayMode = @"menuBar";
        Check([d menuBarNeedsQuota] && !d.showWeeklyQuota && !d.homeShowWeekly, @"top weekly independent from cards");
        [d applyDisplayMode];
        NSStatusItem *item = d.statusItem;
        Check(!panel.presented, @"menu-only hides floating window");
        Check(item != nil && [item.button.title containsString:@"58%"], @"status item displays weekly");
        [d applyDisplayMode];
        Check(d.statusItem == item, @"one status item on repeat application");
        d.displayMode = @"both"; [d applyDisplayMode];
        Check(d.statusItem == item && panel.presented, @"both mode reuses item and restores window");
        d.menuBarMetrics = @[]; [d updateMenuBar];
        Check(d.statusItem.button.title.length == 0 && d.statusItem.button.image != nil, @"icon-only retains entry");
        d.displayMode = @"floating"; [d applyDisplayMode];
        Check(d.statusItem == nil && panel.presented, @"floating-only removes status item and retains window");
        Check(panel.frame.origin.x == 40 && panel.frame.origin.y == 50, @"mode switch preserves window position");
        d.displayMode = @"menuBar"; d.menuBarMetrics = @[@"weekly"];
        [d startCodexProviderIfNeeded];
        Check(provider.starts == 1 && [provider.enabledRequestIDs isEqual:[NSSet setWithObject:@2]], @"only quota request for top weekly");
        [d startCodexProviderIfNeeded];
        Check(provider.starts == 1 && d.codexTimer.valid, @"no duplicate provider");
        s.weeklyResetAt = now + 30;
        [d scheduleCodexRefreshTimer];
        Check(d.codexTimer.fireDate.timeIntervalSince1970 <= now + 31, @"expiry reuses existing timer");
        [d refreshCodexData];
        Check(provider.refreshes == 0, @"local expiry does not force a request");
        [d.codexTimer invalidate];
        s.weeklyResetAt = now + 7200;
        Check(![d systemDataNeeded], @"menu-only quota no system polling");
        d.menuBarMetrics = @[@"cpu", @"memory"];
        [d configureSamplingAndTimers];
        Check(d.systemTimer.valid && !sampler.collectTopApps && !sampler.collectSecondaryMetrics, @"lightweight shared system sampling");
        NSTimer *old = d.systemTimer;
        [d configureSamplingAndTimers];
        Check(!old.valid && d.systemTimer.valid, @"old timer invalidated");
        [d.systemTimer invalidate];
        d.menuBarMetrics = @[@"weekly"]; d.showWeeklyQuota = YES;
        d.showTokenWindows = YES; d.showLocalCost = YES; d.showRecentTasks = YES;
        s.recentTasks = @[@{@"name": @"SYNTHETIC sample task — layout verification", @"id": @"TEST"}];
        [d updateCodexDisplay];
        [d prepareStatusPopover]; [d refreshStatusDetails];
        NSView *root = d.statusPopover.contentViewController.view;
        root.appearance = [NSAppearance appearanceNamed:NSAppearanceNameAqua];
        root.wantsLayer = YES;
        root.layer.backgroundColor = NSColor.whiteColor.CGColor;
        [root.appearance performAsCurrentDrawingAppearance:^{ ResolveTestTextColors(root); }];
        [root layoutSubtreeIfNeeded];
        Check(root.frame.size.width == 390 && root.frame.size.height == 510, @"bounded popover size");
        Check(FindButtonWithTitle(root, HUDL(@"显示设置…")) != nil && FindButtonWithTitle(root, HUDL(@"退出 Codex Monitor HUD")) != nil, @"settings and quit always accessible");
        Check(!d.statusPopover.animates && d.statusPopover.behavior == NSPopoverBehaviorTransient, @"no animation; click away closes");
        Check(d.statusDetailLines.count >= 9, @"quota tokens costs system history present");
        Check(![d.statusDetailLines.description containsString:HUDL(@"5小时剩余额度")], @"hidden five-hour quota stays hidden");
        NSTextField *first = (NSTextField *)d.statusDetails.arrangedSubviews.firstObject;
        [d refreshStatusDetails];
        Check(first == d.statusDetails.arrangedSubviews.firstObject, @"unchanged snapshot does not rebuild view");
        for (NSView *label in d.statusDetails.arrangedSubviews) Check(label.frame.size.width > 300 && label.frame.size.height > 0, @"wrapping label has nonzero layout");
        NSScrollView *scroll = d.statusDetails.enclosingScrollView;
        Check(scroll.hasVerticalScroller && d.statusDetails.frame.size.height > scroll.contentView.frame.size.height, @"long details scroll rather than clip");
        NSView *last = d.statusDetails.arrangedSubviews.lastObject;
        [d.statusDetails scrollRectToVisible:last.frame];
        Check(NSIntersectsRect(last.frame, d.statusDetails.visibleRect), @"last history row is reachable");
        [d.statusDetails scrollRectToVisible:first.frame];
        if (argc > 1) {
            NSBitmapImageRep *bitmap = [root bitmapImageRepForCachingDisplayInRect:root.bounds];
            [root.appearance performAsCurrentDrawingAppearance:^{ [root cacheDisplayInRect:root.bounds toBitmapImageRep:bitmap]; }];
            NSData *png = [bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
            Check([png writeToFile:[NSString stringWithUTF8String:argv[1]] atomically:YES], @"synthetic visual capture");
        }
        [d popoverDidClose:[NSNotification notificationWithName:NSPopoverDidCloseNotification object:d.statusPopover]];
        Check(!d.statusPopover && !d.statusDetails && !d.systemTimer.valid, @"close releases detail UI and unneeded sampler");
        [d.codexTimer invalidate];
        Check(provider.starts == 1 && provider.refreshes == 0, @"presentation never duplicates account requests");
        printf("menu_bar_checks=%lu pass (synthetic, offline)\n", (unsigned long)assertions);
    }
    return 0;
}
