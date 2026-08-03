#import <Cocoa/Cocoa.h>
#import "NativeSampler.h"
#import "CodexStatusProvider.h"
#import "HUDView.h"

static NSString *FormatRate(double value) {
    if (value >= 1.0) return [NSString stringWithFormat:@"%.1f MB/s", value];
    return [NSString stringWithFormat:@"%.0f KB/s", value * 1024.0];
}

static NSString *CSVSafe(NSString *value) {
    return [[value ?: @"" stringByReplacingOccurrencesOfString:@"," withString:@"_"] stringByReplacingOccurrencesOfString:@"\n" withString:@" "];
}

static NSString *FormatReset(NSTimeInterval timestamp) {
    if (timestamp <= 0) return @"恢复时间未返回";
    NSTimeInterval remaining = timestamp - NSDate.date.timeIntervalSince1970;
    if (remaining <= 0) return @"即将恢复";
    NSInteger minutes = (NSInteger)ceil(remaining / 60.0);
    if (minutes < 60) return [NSString stringWithFormat:@"%ld分钟后恢复", (long)minutes];
    NSInteger hours = minutes / 60;
    NSInteger rest = minutes % 60;
    if (hours < 48) return rest ? [NSString stringWithFormat:@"%ld小时%ld分后恢复", (long)hours, (long)rest] : [NSString stringWithFormat:@"%ld小时后恢复", (long)hours];
    return [NSString stringWithFormat:@"%ld天%ld小时后恢复", (long)(hours / 24), (long)(hours % 24)];
}

static NSString *FormatTokens(long long tokens) {
    if (tokens >= 1000000000LL) return [NSString stringWithFormat:@"%.2fB", tokens / 1000000000.0];
    if (tokens >= 1000000LL) return [NSString stringWithFormat:@"%.2fM", tokens / 1000000.0];
    if (tokens >= 1000LL) return [NSString stringWithFormat:@"%.1fK", tokens / 1000.0];
    return [NSString stringWithFormat:@"%lld", tokens];
}

static NSString *FormatUsageDate(NSString *date) {
    NSArray<NSString *> *parts = [date componentsSeparatedByString:@"-"];
    if (parts.count != 3) return date.length > 0 ? date : @"--";
    return [NSString stringWithFormat:@"%ld/%ld", (long)[parts[1] integerValue], (long)[parts[2] integerValue]];
}

static NSString *FormatPlan(NSString *plan) {
    NSDictionary *names = @{
        @"free": @"Free", @"go": @"Go", @"plus": @"Plus", @"pro": @"Pro", @"prolite": @"Pro Lite",
        @"team": @"Team", @"business": @"Business", @"self_serve_business_usage_based": @"Business",
        @"ent26": @"Enterprise", @"enterprise_cbp_usage_based": @"Enterprise", @"enterprise": @"Enterprise", @"edu": @"Edu"
    };
    return names[plan ?: @""] ?: @"未知订阅";
}

static NSString *FormatAge(NSTimeInterval timestamp) {
    if (timestamp <= 0) return @"连接中";
    NSInteger age = MAX(0, (NSInteger)(NSDate.date.timeIntervalSince1970 - timestamp));
    if (age < 10) return @"刚刚";
    if (age < 60) return [NSString stringWithFormat:@"%ld秒前", (long)age];
    return [NSString stringWithFormat:@"%ld分钟前", (long)(age / 60)];
}

static NSString *FormatModuleState(NSTimeInterval timestamp, NSString *error) {
    if (error.length > 0) return timestamp > 0 ? [NSString stringWithFormat:@"失败·上次%@", FormatAge(timestamp)] : @"失败";
    return FormatAge(timestamp);
}

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property(nonatomic, strong) NSPanel *panel;
@property(nonatomic, strong) NSPanel *settingsWindow;
@property(nonatomic, strong) HUDView *hudView;
@property(nonatomic, strong) NSTimer *systemTimer;
@property(nonatomic, strong) NSTimer *codexTimer;
@property(nonatomic, strong) NativeSampler *sampler;
@property(nonatomic, strong) CodexStatusProvider *codexProvider;
@property(nonatomic, strong) NativeSnapshot *lastSnapshot;
@property(nonatomic, strong) NSMutableArray<NativeSnapshot *> *minuteSamples;
@property(nonatomic, strong) NSMutableArray<NSDictionary *> *cpuHistory;
@property(nonatomic, strong) NSURL *historyDirectory;
@property(nonatomic, copy) NSString *instanceID;
@property(nonatomic) NSTimeInterval minuteStart;
@property(nonatomic) NSTimeInterval refreshInterval;
@property(nonatomic) NSInteger codexTick;
@property(nonatomic) NSInteger currentPage;
@property(nonatomic) BOOL compact;
@property(nonatomic) BOOL showFiveHourQuota;
@property(nonatomic) BOOL showWeeklyQuota;
@property(nonatomic) BOOL showPlan;
@property(nonatomic) BOOL showUsage;
@property(nonatomic) BOOL showModelQuota;
@property(nonatomic) BOOL showSystem;
@property(nonatomic) BOOL showAttribution;
@property(nonatomic) BOOL showTrend;
@property(nonatomic) BOOL showMemoryApps;
@property(nonatomic) BOOL homeShowFiveHour;
@property(nonatomic) BOOL homeShowWeekly;
@property(nonatomic) BOOL homeShowPlan;
@property(nonatomic) BOOL homeShowUsage;
@property(nonatomic) BOOL homeShowModelQuota;
@property(nonatomic) BOOL homeShowDiagnosis;
@property(nonatomic) BOOL homeShowSystem;
@property(nonatomic) BOOL homeShowAttribution;
@property(nonatomic) BOOL homeShowTrend;
@property(nonatomic) BOOL homeShowMemoryApps;
@property(nonatomic) BOOL historyEnabled;
@property(nonatomic) BOOL alwaysOnTop;
@property(nonatomic) BOOL collapsed;
@property(nonatomic) CGFloat backgroundOpacity;
@property(nonatomic) CGFloat windowScale;
@property(nonatomic, copy) NSString *accentName;
- (void)showSettingsWindow:(id)sender;
- (NSView *)settingsContentView;
@end

@implementation AppDelegate

- (void)configureApplicationMenu {
    NSMenu *mainMenu = [NSMenu new];

    NSMenuItem *appRoot = [NSMenuItem new];
    NSMenu *appMenu = [[NSMenu alloc] initWithTitle:@"Codex Monitor HUD"];
    NSMenuItem *about = [appMenu addItemWithTitle:@"关于 Codex Monitor HUD" action:@selector(orderFrontStandardAboutPanel:) keyEquivalent:@""];
    about.target = NSApp;
    [appMenu addItem:NSMenuItem.separatorItem];
    NSMenuItem *show = [appMenu addItemWithTitle:@"显示悬浮窗" action:@selector(showHUD:) keyEquivalent:@""];
    show.target = self;
    NSMenuItem *hide = [appMenu addItemWithTitle:@"隐藏 Codex Monitor HUD" action:@selector(hide:) keyEquivalent:@"h"];
    hide.target = NSApp;
    [appMenu addItem:NSMenuItem.separatorItem];
    NSMenuItem *quit = [appMenu addItemWithTitle:@"退出 Codex Monitor HUD" action:@selector(terminate:) keyEquivalent:@"q"];
    quit.target = NSApp;
    appRoot.submenu = appMenu;
    [mainMenu addItem:appRoot];

    NSMenuItem *windowRoot = [NSMenuItem new];
    NSMenu *windowMenu = [[NSMenu alloc] initWithTitle:@"窗口"];
    NSMenuItem *minimize = [windowMenu addItemWithTitle:@"最小化到程序栏" action:@selector(minimizeToDock:) keyEquivalent:@"m"];
    minimize.target = self;
    NSMenuItem *restore = [windowMenu addItemWithTitle:@"显示悬浮窗" action:@selector(showHUD:) keyEquivalent:@""];
    restore.target = self;
    windowRoot.submenu = windowMenu;
    [mainMenu addItem:windowRoot];
    NSApp.windowsMenu = windowMenu;
    NSApp.mainMenu = mainMenu;
}

- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [self configureApplicationMenu];
    NSUserDefaults *d = NSUserDefaults.standardUserDefaults;
    self.compact = [d objectForKey:@"compact"] ? [d boolForKey:@"compact"] : YES;
    BOOL legacyQuotaVisible = [d objectForKey:@"showQuota"] ? [d boolForKey:@"showQuota"] : YES;
    self.showFiveHourQuota = [d objectForKey:@"showFiveHourQuota"] ? [d boolForKey:@"showFiveHourQuota"] : legacyQuotaVisible;
    self.showWeeklyQuota = [d objectForKey:@"showWeeklyQuota"] ? [d boolForKey:@"showWeeklyQuota"] : legacyQuotaVisible;
    self.showPlan = [d objectForKey:@"showPlan"] ? [d boolForKey:@"showPlan"] : YES;
    self.showUsage = [d objectForKey:@"showUsage"] ? [d boolForKey:@"showUsage"] : YES;
    self.showModelQuota = [d objectForKey:@"showModelQuota"] ? [d boolForKey:@"showModelQuota"] : NO;
    self.showSystem = [d objectForKey:@"showSystem"] ? [d boolForKey:@"showSystem"] : YES;
    self.showAttribution = [d objectForKey:@"showAttribution"] ? [d boolForKey:@"showAttribution"] : YES;
    self.showTrend = [d objectForKey:@"showTrend"] ? [d boolForKey:@"showTrend"] : YES;
    self.showMemoryApps = [d objectForKey:@"showMemoryApps"] ? [d boolForKey:@"showMemoryApps"] : YES;
    self.homeShowFiveHour = [d objectForKey:@"homeShowFiveHour"] ? [d boolForKey:@"homeShowFiveHour"] : YES;
    self.homeShowWeekly = [d objectForKey:@"homeShowWeekly"] ? [d boolForKey:@"homeShowWeekly"] : YES;
    self.homeShowPlan = [d objectForKey:@"homeShowPlan"] ? [d boolForKey:@"homeShowPlan"] : YES;
    self.homeShowUsage = [d objectForKey:@"homeShowUsage"] ? [d boolForKey:@"homeShowUsage"] : YES;
    self.homeShowModelQuota = [d objectForKey:@"homeShowModelQuota"] ? [d boolForKey:@"homeShowModelQuota"] : NO;
    self.homeShowDiagnosis = [d objectForKey:@"homeShowDiagnosis"] ? [d boolForKey:@"homeShowDiagnosis"] : YES;
    self.homeShowSystem = [d objectForKey:@"homeShowSystem"] ? [d boolForKey:@"homeShowSystem"] : YES;
    self.homeShowAttribution = [d objectForKey:@"homeShowAttribution"] ? [d boolForKey:@"homeShowAttribution"] : YES;
    self.homeShowTrend = [d objectForKey:@"homeShowTrend"] ? [d boolForKey:@"homeShowTrend"] : YES;
    self.homeShowMemoryApps = [d objectForKey:@"homeShowMemoryApps"] ? [d boolForKey:@"homeShowMemoryApps"] : NO;
    self.historyEnabled = [d objectForKey:@"historyEnabled"] ? [d boolForKey:@"historyEnabled"] : YES;
    self.alwaysOnTop = [d objectForKey:@"alwaysOnTop"] ? [d boolForKey:@"alwaysOnTop"] : YES;
    self.backgroundOpacity = [d objectForKey:@"opacity"] ? [d doubleForKey:@"opacity"] : 0.82;
    self.backgroundOpacity = MAX(0.55, MIN(1.0, self.backgroundOpacity));
    double savedScale = [d objectForKey:@"windowScale"] ? [d doubleForKey:@"windowScale"] : 1.0;
    self.windowScale = fabs(savedScale - 0.85) < 0.05 ? 0.85 : (fabs(savedScale - 1.2) < 0.1 ? 1.2 : 1.0);
    self.refreshInterval = [d objectForKey:@"refreshInterval"] ? [d doubleForKey:@"refreshInterval"] : 5.0;
    self.refreshInterval = MAX(5.0, MIN(20.0, self.refreshInterval));
    self.currentPage = MAX(0, MIN(2, [d integerForKey:@"currentPage"]));
    self.accentName = [d stringForKey:@"accentName"] ?: @"green";
    self.sampler = [NativeSampler new];
    self.minuteSamples = [NSMutableArray array];
    self.cpuHistory = [NSMutableArray array];
    NSString *historyPath = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/CodexSystemMonitor/native-history"];
    self.historyDirectory = [NSURL fileURLWithPath:historyPath isDirectory:YES];
    [[NSFileManager defaultManager] createDirectoryAtURL:self.historyDirectory withIntermediateDirectories:YES attributes:nil error:nil];
    self.instanceID = NSUUID.UUID.UUIDString;
    [self appendLifecycleEvent:[self launchLifecycleEvent]];
    [self createPanel];
    [self startSystemTimer];
    [self startCodexProviderIfNeeded];
    [self updateDisplay];
    [[[NSWorkspace sharedWorkspace] notificationCenter] addObserver:self selector:@selector(workspaceDidWake:) name:NSWorkspaceDidWakeNotification object:nil];
}

- (void)applicationWillTerminate:(NSNotification *)notification {
    [self appendLifecycleEvent:@"terminate"];
    [self savePosition];
    [self.systemTimer invalidate];
    [self.codexTimer invalidate];
    [self.codexProvider stop];
    [[[NSWorkspace sharedWorkspace] notificationCenter] removeObserver:self];
}

- (BOOL)applicationShouldHandleReopen:(NSApplication *)sender hasVisibleWindows:(BOOL)hasVisibleWindows {
    [self showHUD:nil];
    return YES;
}

- (void)showHUD:(id)sender {
    if (self.panel.isMiniaturized) [self.panel deminiaturize:nil];
    [NSApp activateIgnoringOtherApps:YES];
    [self.panel orderFrontRegardless];
}

- (NSColor *)accentColor {
    if ([self.accentName isEqualToString:@"blue"]) return NSColor.systemBlueColor;
    if ([self.accentName isEqualToString:@"purple"]) return NSColor.systemPurpleColor;
    if ([self.accentName isEqualToString:@"orange"]) return NSColor.systemOrangeColor;
    return NSColor.systemGreenColor;
}

- (CGFloat)homePanelHeight {
    CGFloat height = 52;
    BOOL hasCodex = self.homeShowFiveHour || self.homeShowWeekly || self.homeShowPlan || self.homeShowUsage || self.homeShowModelQuota;
    BOOL hasComputer = self.homeShowDiagnosis || self.homeShowSystem || self.homeShowAttribution || self.homeShowMemoryApps || self.homeShowTrend;
    if (hasCodex) height += 26;
    if (self.homeShowFiveHour || self.homeShowWeekly) height += 82;
    if (self.homeShowPlan || self.homeShowUsage || self.homeShowModelQuota) height += 66;
    if (hasComputer) height += 26;
    if (self.homeShowDiagnosis || self.homeShowSystem) height += 66;
    if (self.homeShowAttribution) height += 25;
    if (self.homeShowMemoryApps) height += 118;
    if (self.homeShowTrend) height += 25;
    height += 22;
    return MAX(170, MIN(460, height + (self.compact ? 0 : 110)));
}

- (NSSize)basePanelSize {
    if (self.collapsed) return NSMakeSize(430, 54);
    CGFloat height = self.currentPage == 0 ? [self homePanelHeight] : (self.currentPage == 1 ? 260 : (self.showMemoryApps ? 421 : 303));
    if (!self.compact && self.currentPage != 0) height += 110;
    return NSMakeSize(430, height);
}

- (NSSize)panelSize {
    NSSize base = [self basePanelSize];
    return NSMakeSize(round(base.width * self.windowScale), round(base.height * self.windowScale));
}

- (void)createPanel {
    NSSize size = [self panelSize];
    NSSize baseSize = [self basePanelSize];
    self.panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, size.width, size.height)
                                            styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskFullSizeContentView | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskNonactivatingPanel)
                                              backing:NSBackingStoreBuffered defer:NO];
    self.panel.level = self.alwaysOnTop ? NSFloatingWindowLevel : NSNormalWindowLevel;
    self.panel.opaque = NO;
    self.panel.backgroundColor = NSColor.clearColor;
    self.panel.hasShadow = YES;
    self.panel.title = @"Codex Monitor HUD";
    self.panel.titleVisibility = NSWindowTitleHidden;
    self.panel.titlebarAppearsTransparent = YES;
    [self.panel standardWindowButton:NSWindowCloseButton].hidden = YES;
    [self.panel standardWindowButton:NSWindowMiniaturizeButton].hidden = YES;
    [self.panel standardWindowButton:NSWindowZoomButton].hidden = YES;
    self.panel.hidesOnDeactivate = NO;
    self.panel.movableByWindowBackground = YES;
    self.panel.becomesKeyOnlyIfNeeded = YES;
    self.panel.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorFullScreenAuxiliary;
    self.panel.delegate = self;
    self.panel.alphaValue = 1.0;

    self.hudView = [[HUDView alloc] initWithFrame:NSMakeRect(0, 0, size.width, size.height)];
    self.hudView.bounds = NSMakeRect(0, 0, baseSize.width, baseSize.height);
    self.hudView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    self.hudView.accentColor = [self accentColor];
    self.hudView.codexStatusLabel.textColor = self.hudView.accentColor;
    [self.hudView setBackgroundOpacity:self.backgroundOpacity];
    [self.hudView setAlwaysOnTop:self.alwaysOnTop];
    [self.hudView setCompact:self.compact];
    [self.hudView setFiveHourQuotaVisible:self.showFiveHourQuota];
    [self.hudView setWeeklyQuotaVisible:self.showWeeklyQuota];
    [self.hudView setPlanVisible:self.showPlan];
    [self.hudView setUsageVisible:self.showUsage];
    [self.hudView setModelQuotaVisible:NO];
    [self.hudView setSystemVisible:self.showSystem];
    [self.hudView setAttributionVisible:self.showAttribution];
    [self.hudView setTrendVisible:self.showTrend];
    [self.hudView setMemoryAppsVisible:self.showMemoryApps];
    [self.hudView setHomeFiveHourVisible:self.homeShowFiveHour];
    [self.hudView setHomeWeeklyVisible:self.homeShowWeekly];
    [self.hudView setHomePlanVisible:self.homeShowPlan];
    [self.hudView setHomeUsageVisible:self.homeShowUsage];
    [self.hudView setHomeModelQuotaVisible:NO];
    [self.hudView setHomeDiagnosisVisible:self.homeShowDiagnosis];
    [self.hudView setHomeSystemVisible:self.homeShowSystem];
    [self.hudView setHomeAttributionVisible:self.homeShowAttribution];
    [self.hudView setHomeTrendVisible:self.homeShowTrend];
    [self.hudView setHomeMemoryAppsVisible:self.homeShowMemoryApps];
    [self.hudView setPage:self.currentPage];
    __weak typeof(self) weakSelf = self;
    self.hudView.menuProvider = ^NSMenu *{ return [weakSelf settingsMenu]; };
    self.hudView.settingsRequested = ^{ [weakSelf showSettingsWindow:nil]; };
    self.hudView.pageChanged = ^(NSInteger page) {
        weakSelf.currentPage = page;
        [NSUserDefaults.standardUserDefaults setInteger:page forKey:@"currentPage"];
        [weakSelf updateDetailLabels];
        [weakSelf resizePanel];
    };
    self.hudView.topmostChanged = ^(BOOL enabled) {
        weakSelf.alwaysOnTop = enabled;
        [NSUserDefaults.standardUserDefaults setBool:enabled forKey:@"alwaysOnTop"];
        weakSelf.panel.level = enabled ? NSFloatingWindowLevel : NSNormalWindowLevel;
        if (enabled) [weakSelf.panel orderFrontRegardless];
    };
    self.hudView.minimizeRequested = ^{
        [weakSelf.panel miniaturize:nil];
    };
    self.panel.contentView = self.hudView;
    if (![self restorePosition]) [self moveToCorner:@"bottomLeft"];
    [self.panel orderFrontRegardless];
}

- (void)startSystemTimer {
    [self.systemTimer invalidate];
    self.systemTimer = [NSTimer scheduledTimerWithTimeInterval:self.refreshInterval target:self selector:@selector(updateDisplay) userInfo:nil repeats:YES];
    [[NSRunLoop mainRunLoop] addTimer:self.systemTimer forMode:NSRunLoopCommonModes];
}

- (void)startCodexProviderIfNeeded {
    BOOL needsCodexData = self.showFiveHourQuota || self.showWeeklyQuota || self.showPlan || self.showUsage || self.showModelQuota || self.homeShowFiveHour || self.homeShowWeekly || self.homeShowPlan || self.homeShowUsage || self.homeShowModelQuota;
    if (!needsCodexData) { [self.codexProvider stop]; self.codexProvider = nil; [self.codexTimer invalidate]; self.codexTimer = nil; return; }
    if (!self.codexProvider) {
        self.codexProvider = [CodexStatusProvider new];
        __weak typeof(self) weakSelf = self;
        self.codexProvider.updateHandler = ^{ [weakSelf updateCodexDisplay]; };
    }
    [self.codexProvider start];
    [self.codexTimer invalidate];
    self.codexTimer = [NSTimer scheduledTimerWithTimeInterval:60.0 target:self selector:@selector(refreshCodexData) userInfo:nil repeats:YES];
    [[NSRunLoop mainRunLoop] addTimer:self.codexTimer forMode:NSRunLoopCommonModes];
}

- (void)refreshCodexData {
    self.codexTick++;
    if (self.codexProvider) [self.codexProvider start];
}

- (BOOL)restorePosition {
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    if ([defaults objectForKey:@"originX"] == nil || [defaults objectForKey:@"originY"] == nil) return NO;
    NSRect proposed = NSMakeRect([defaults doubleForKey:@"originX"], [defaults doubleForKey:@"originY"], [self panelSize].width, [self panelSize].height);
    for (NSScreen *screen in NSScreen.screens) {
        if (NSIntersectsRect(screen.visibleFrame, proposed)) {
            NSRect visible = screen.visibleFrame;
            proposed.origin.x = MAX(NSMinX(visible), MIN(proposed.origin.x, NSMaxX(visible) - proposed.size.width));
            proposed.origin.y = MAX(NSMinY(visible), MIN(proposed.origin.y, NSMaxY(visible) - proposed.size.height));
            [self.panel setFrame:proposed display:NO]; return YES;
        }
    }
    return NO;
}

- (void)savePosition {
    if (!self.panel) return;
    [NSUserDefaults.standardUserDefaults setDouble:self.panel.frame.origin.x forKey:@"originX"];
    [NSUserDefaults.standardUserDefaults setDouble:self.panel.frame.origin.y forKey:@"originY"];
}

- (void)windowDidMove:(NSNotification *)notification { [self savePosition]; }

- (void)workspaceDidWake:(NSNotification *)notification {
    [self appendLifecycleEvent:@"wake"];
    self.sampler = [NativeSampler new];
    [self.minuteSamples removeAllObjects];
    [self.cpuHistory removeAllObjects];
    self.minuteStart = 0;
    [self updateDisplay];
    [self.codexProvider refreshQuota];
    [self.panel orderFrontRegardless];
}

- (void)appendLifecycleEvent:(NSString *)event {
    if (!self.historyDirectory || self.instanceID.length == 0 || event.length == 0) return;
    NSDate *now = NSDate.date;
    NSDateFormatter *stamp = [NSDateFormatter new];
    stamp.dateFormat = @"yyyy-MM-dd HH:mm:ss";
    stamp.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
    NSURL *file = [self.historyDirectory URLByAppendingPathComponent:@"events.csv"];
    NSString *header = @"timestamp,epoch,event,instance_id\n";
    NSString *line = [NSString stringWithFormat:@"%@,%.0f,%@,%@\n", [stamp stringFromDate:now], now.timeIntervalSince1970, CSVSafe(event), CSVSafe(self.instanceID)];
    if (![NSFileManager.defaultManager fileExistsAtPath:file.path]) [header writeToURL:file atomically:YES encoding:NSUTF8StringEncoding error:nil];
    NSFileHandle *handle = [NSFileHandle fileHandleForWritingAtPath:file.path];
    if (handle) { [handle seekToEndOfFile]; [handle writeData:[line dataUsingEncoding:NSUTF8StringEncoding]]; [handle closeFile]; }
}

- (NSString *)launchLifecycleEvent {
    NSURL *file = [self.historyDirectory URLByAppendingPathComponent:@"events.csv"];
    NSString *contents = [NSString stringWithContentsOfURL:file encoding:NSUTF8StringEncoding error:nil];
    for (NSString *line in [contents componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet].reverseObjectEnumerator) {
        if (line.length == 0 || [line hasPrefix:@"timestamp,"]) continue;
        NSArray<NSString *> *fields = [line componentsSeparatedByString:@","];
        return fields.count >= 3 && [fields[2] isEqualToString:@"terminate"] ? @"start" : @"restart";
    }
    return @"start";
}

- (NSDictionary *)statusForSnapshot:(NativeSnapshot *)snapshot {
    NSString *title = @"电脑正常 · Codex未造成瓶颈", *code = @"normal";
    NSColor *color = [self accentColor];
    double largestOtherMemory = 0;
    for (NativeTopApp *app in snapshot.topMemoryApps) {
        if (![app.name isEqualToString:@"Codex/ChatGPT"]) largestOtherMemory = MAX(largestOtherMemory, app.memoryGiB);
    }
    BOOL codexMemoryDominant = snapshot.codexMemoryPercent >= 30.0 && (largestOtherMemory <= 0 || snapshot.codexMemoryGiB >= largestOtherMemory * 1.3);
    if (snapshot.memoryPressureLevel >= 2) {
        title = codexMemoryDominant ? @"内存严重吃紧 · Codex很可能是主因" : @"内存严重吃紧 · 主因待判断";
        code = codexMemoryDominant ? @"memory_codex_critical" : @"memory_critical"; color = NSColor.systemRedColor;
    }
    else if (snapshot.memoryPressureLevel == 1) {
        title = codexMemoryDominant ? @"内存开始吃紧 · Codex可能是主因" : @"内存开始吃紧 · 主因待判断";
        code = codexMemoryDominant ? @"memory_codex_warning" : @"memory_warning"; color = NSColor.systemOrangeColor;
    }
    else if (snapshot.thermalLevel >= 2) { title = @"系统热压力正在限制性能"; code = @"thermal"; color = NSColor.systemRedColor; }
    else if (snapshot.systemCPUPercent >= 90.0) {
        if (snapshot.codexCPUPercent >= 25.0 && snapshot.codexCPUPercent >= snapshot.systemCPUPercent * 0.4) { title = @"CPU吃紧 · Codex很可能是主因"; code = @"cpu_codex"; }
        else { title = @"CPU吃紧 · 主要是其他程序"; code = @"cpu_other"; }
        color = NSColor.systemOrangeColor;
    } else if (snapshot.systemCPUPercent >= 75.0) { title = @"CPU负载较高"; code = @"cpu_high"; color = NSColor.systemYellowColor; }
    return @{@"title": title, @"code": code, @"color": color};
}

- (NSString *)topAppsText:(NSArray<NativeTopApp *> *)apps cpu:(BOOL)cpu {
    NSMutableArray<NSString *> *parts = [NSMutableArray array];
    for (NativeTopApp *app in apps) [parts addObject:cpu ? [NSString stringWithFormat:@"%@ %.0f%%", app.name, app.cpuPercent] : [NSString stringWithFormat:@"%@ %.1fG", app.name, app.memoryGiB]];
    return parts.count ? [parts componentsJoinedByString:@" · "] : @"暂无";
}

- (void)appendCPUHistory:(NativeSnapshot *)snapshot {
    [self.cpuHistory addObject:@{@"time": @(snapshot.timestamp), @"cpu": @(snapshot.systemCPUPercent)}];
    while (self.cpuHistory.count > 0 && snapshot.timestamp - [self.cpuHistory.firstObject[@"time"] doubleValue] > 600.0) [self.cpuHistory removeObjectAtIndex:0];
}

- (NSArray<NSNumber *> *)sparklineValues {
    if (self.cpuHistory.count <= 18) return [self.cpuHistory valueForKey:@"cpu"];
    NSMutableArray<NSNumber *> *result = [NSMutableArray arrayWithCapacity:18];
    for (NSInteger bucket = 0; bucket < 18; bucket++) {
        NSInteger start = bucket * self.cpuHistory.count / 18;
        NSInteger end = (bucket + 1) * self.cpuHistory.count / 18;
        double sum = 0; NSInteger count = 0;
        for (NSInteger index = start; index < end; index++) { sum += [self.cpuHistory[index][@"cpu"] doubleValue]; count++; }
        [result addObject:@(count ? sum / count : 0)];
    }
    return result;
}

- (NSDictionary *)diagnosisForStatusCode:(NSString *)code {
    if ([code hasPrefix:@"memory_codex"]) return @{@"bottleneck": @"内存", @"impact": @"高", @"confidence": @"中等置信度 · 推断"};
    if ([code hasPrefix:@"memory_"]) return @{@"bottleneck": @"内存", @"impact": @"待判断", @"confidence": @"低置信度"};
    if ([code isEqualToString:@"cpu_codex"]) return @{@"bottleneck": @"CPU", @"impact": @"高", @"confidence": @"较高置信度 · 推断"};
    if ([code isEqualToString:@"cpu_other"]) return @{@"bottleneck": @"CPU", @"impact": @"低", @"confidence": @"中等置信度 · 其他程序"};
    if ([code isEqualToString:@"cpu_high"]) return @{@"bottleneck": @"CPU偏高", @"impact": @"待判断", @"confidence": @"低置信度"};
    if ([code isEqualToString:@"thermal"]) return @{@"bottleneck": @"热压力", @"impact": @"待判断", @"confidence": @"低置信度"};
    return @{@"bottleneck": @"无", @"impact": @"低", @"confidence": @"较高置信度"};
}

- (void)updateDisplay {
    NativeSnapshot *snapshot = [self.sampler sample];
    self.lastSnapshot = snapshot;
    [self appendCPUHistory:snapshot];
    NSDictionary *status = [self statusForSnapshot:snapshot];
    NSDictionary *diagnosis = [self diagnosisForStatusCode:status[@"code"]];
    self.hudView.computerStatusLabel.stringValue = [NSString stringWithFormat:@"● %@", status[@"title"]];
    self.hudView.computerStatusLabel.textColor = status[@"color"];
    self.hudView.homeComputerStatusLabel.stringValue = [NSString stringWithFormat:@"● %@", status[@"title"]];
    self.hudView.homeComputerStatusLabel.textColor = status[@"color"];
    self.hudView.bottleneckCard.valueLabel.stringValue = diagnosis[@"bottleneck"];
    self.hudView.impactCard.valueLabel.stringValue = diagnosis[@"impact"];
    self.hudView.bottleneckCard.subtitleLabel.stringValue = @"当前判断";
    self.hudView.impactCard.subtitleLabel.stringValue = diagnosis[@"confidence"];
    self.hudView.bottleneckCard.valueLabel.textColor = status[@"color"];
    self.hudView.impactCard.valueLabel.textColor = status[@"color"];
    self.hudView.homeBottleneckCard.valueLabel.stringValue = diagnosis[@"bottleneck"];
    self.hudView.homeBottleneckCard.subtitleLabel.stringValue = [NSString stringWithFormat:@"Codex影响 %@ · %@", diagnosis[@"impact"], diagnosis[@"confidence"]];
    self.hudView.homeBottleneckCard.valueLabel.textColor = status[@"color"];
    self.hudView.cpuCard.valueLabel.stringValue = [NSString stringWithFormat:@"%.0f%%", snapshot.systemCPUPercent];
    self.hudView.memoryPressureCard.valueLabel.stringValue = snapshot.memoryPressureText ?: @"未知";
    self.hudView.cpuCard.valueLabel.textColor = snapshot.systemCPUPercent >= 75 ? NSColor.systemOrangeColor : [self accentColor];
    self.hudView.memoryPressureCard.valueLabel.textColor = snapshot.memoryPressureLevel > 0 ? NSColor.systemOrangeColor : [self accentColor];
    self.hudView.homeSystemCard.valueLabel.stringValue = [NSString stringWithFormat:@"CPU %.0f%%", snapshot.systemCPUPercent];
    self.hudView.homeSystemCard.subtitleLabel.stringValue = [NSString stringWithFormat:@"内存压力 %@ · Swap %.1fG", snapshot.memoryPressureText ?: @"未知", snapshot.swapUsedGiB];
    self.hudView.homeSystemCard.valueLabel.textColor = snapshot.systemCPUPercent >= 75 ? NSColor.systemOrangeColor : [self accentColor];
    self.hudView.attributionLabel.stringValue = [NSString stringWithFormat:@"Codex CPU %.1f%%  ·  内存 %.1f / %.0fG (%.0f%%)", snapshot.codexCPUPercent, snapshot.codexMemoryGiB, snapshot.totalMemoryGiB, snapshot.codexMemoryPercent];
    self.hudView.homeAttributionLabel.stringValue = [NSString stringWithFormat:@"Codex CPU %.1f%% · 内存 %.1f / %.0fG (%.0f%%)", snapshot.codexCPUPercent, snapshot.codexMemoryGiB, snapshot.totalMemoryGiB, snapshot.codexMemoryPercent];
    [self.hudView.memoryAppsCard updateApps:snapshot.topMemoryApps totalMemoryGiB:snapshot.totalMemoryGiB];
    [self.hudView.homeMemoryAppsCard updateApps:snapshot.topMemoryApps totalMemoryGiB:snapshot.totalMemoryGiB];
    self.hudView.healthLabel.stringValue = [NSString stringWithFormat:@"系统  Swap %.1fG (%+.0fM/10分)  ·  热状态 %@", snapshot.swapUsedGiB, snapshot.swapDelta10MinMiB, snapshot.thermalText];
    double sum = 0, peak = 0;
    for (NSDictionary *item in self.cpuHistory) { double value = [item[@"cpu"] doubleValue]; sum += value; peak = MAX(peak, value); }
    double average = self.cpuHistory.count ? sum / self.cpuHistory.count : 0;
    self.hudView.sparkline.values = [self sparklineValues]; self.hudView.sparkline.accentColor = status[@"color"];
    self.hudView.homeSparkline.values = [self sparklineValues]; self.hudView.homeSparkline.accentColor = status[@"color"];
    NSTimeInterval sampled = 0;
    if (self.cpuHistory.count > 1) sampled = [self.cpuHistory.lastObject[@"time"] doubleValue] - [self.cpuHistory.firstObject[@"time"] doubleValue];
    if (sampled >= 570) self.hudView.trendTitleLabel.stringValue = @"CPU近10分钟";
    else if (sampled < 60) self.hudView.trendTitleLabel.stringValue = [NSString stringWithFormat:@"CPU已采样%.0f秒", MAX(0, sampled)];
    else self.hudView.trendTitleLabel.stringValue = [NSString stringWithFormat:@"CPU已采样%ld分", (long)MAX(1, floor(sampled / 60.0))];
    self.hudView.homeTrendTitleLabel.stringValue = self.hudView.trendTitleLabel.stringValue;
    self.hudView.trendLabel.stringValue = [NSString stringWithFormat:@"平均%.0f%% · 峰值%.0f%%", average, peak];
    self.hudView.homeTrendLabel.stringValue = self.hudView.trendLabel.stringValue;
    self.hudView.computerFreshnessLabel.stringValue = [NSString stringWithFormat:@"来源  macOS系统接口 · 刚刚更新 · 每%.0f秒", self.refreshInterval];
    [self updateCodexDisplay];
    [self updateDetailLabels];
    [self recordSnapshot:snapshot statusCode:status[@"code"]];
}

- (NSString *)visibleCodexStatus:(CodexStatusSnapshot *)s fiveHour:(BOOL)fiveHour weekly:(BOOL)weekly plan:(BOOL)plan usage:(BOOL)usage model:(BOOL)model {
    BOOL quotaVisible = fiveHour || weekly || model;
    if (quotaVisible && s.quotaErrorText.length > 0) return @"额度更新失败，显示上次数据";
    if (plan && s.accountErrorText.length > 0) return @"订阅信息更新失败";
    if (usage && s.usageErrorText.length > 0) return @"Token用量更新失败";
    if (!s.quotaAvailable && !s.accountAvailable && !s.usageAvailable) return s.statusText ?: @"正在连接本机Codex";
    if (fiveHour && !s.fiveHourAvailable) return @"5小时额度暂未返回";
    if (weekly && !s.weeklyAvailable) return @"每周额度暂未返回";
    if (plan && !s.accountAvailable) return @"订阅信息暂未返回";
    if (usage && !s.usageAvailable) return @"Token用量暂未返回";
    if (model && !s.modelQuotaAvailable) return @"模型专属额度暂未返回";
    return @"Codex数据正常";
}

- (NSString *)freshnessText:(CodexStatusSnapshot *)s fiveHour:(BOOL)fiveHour weekly:(BOOL)weekly plan:(BOOL)plan usage:(BOOL)usage model:(BOOL)model {
    NSMutableArray<NSString *> *parts = [NSMutableArray array];
    if (fiveHour || weekly || model) {
        NSString *name = fiveHour && weekly ? @"额度" : (fiveHour ? @"5小时" : (weekly ? @"每周" : @"模型额度"));
        [parts addObject:[NSString stringWithFormat:@"%@ %@", name, FormatModuleState(s.quotaUpdatedAt, s.quotaErrorText)]];
    }
    if (plan) [parts addObject:[NSString stringWithFormat:@"订阅 %@", FormatModuleState(s.accountUpdatedAt, s.accountErrorText)]];
    if (usage) [parts addObject:[NSString stringWithFormat:@"用量 %@", FormatModuleState(s.usageUpdatedAt, s.usageErrorText)]];
    return parts.count ? [parts componentsJoinedByString:@" · "] : @"未启用Codex显示模块";
}

- (void)updateCodexDisplay {
    CodexStatusSnapshot *s = self.codexProvider.snapshot;
    if (!s) { [self updateDetailLabels]; return; }
    NSString *plan = s.accountAvailable ? FormatPlan(s.planType) : nil;
    NSString *statusText = [self visibleCodexStatus:s fiveHour:self.showFiveHourQuota weekly:self.showWeeklyQuota plan:self.showPlan usage:self.showUsage model:self.showModelQuota];
    self.hudView.codexStatusLabel.stringValue = self.showPlan && plan ? [NSString stringWithFormat:@"● %@ · %@", plan, statusText] : [NSString stringWithFormat:@"● %@", statusText];
    NSString *homeStatusText = [self visibleCodexStatus:s fiveHour:self.homeShowFiveHour weekly:self.homeShowWeekly plan:self.homeShowPlan usage:self.homeShowUsage model:self.homeShowModelQuota];
    self.hudView.homeCodexStatusLabel.stringValue = self.homeShowPlan && plan ? [NSString stringWithFormat:@"● %@ · %@", plan, homeStatusText] : [NSString stringWithFormat:@"● %@", homeStatusText];
    [self.hudView.fiveHourCard showAvailable:s.fiveHourAvailable remaining:s.fiveHourRemainingPercent reset:FormatReset(s.fiveHourResetAt) accent:[self accentColor]];
    [self.hudView.weeklyCard showAvailable:s.weeklyAvailable remaining:s.weeklyRemainingPercent reset:FormatReset(s.weeklyResetAt) accent:[self accentColor]];
    [self.hudView.homeFiveHourCard showAvailable:s.fiveHourAvailable remaining:s.fiveHourRemainingPercent reset:FormatReset(s.fiveHourResetAt) accent:[self accentColor]];
    [self.hudView.homeWeeklyCard showAvailable:s.weeklyAvailable remaining:s.weeklyRemainingPercent reset:FormatReset(s.weeklyResetAt) accent:[self accentColor]];
    if (s.quotaErrorText.length > 0 && s.quotaAvailable) {
        self.hudView.fiveHourCard.windowLabel.stringValue = @"上次数据";
        self.hudView.weeklyCard.windowLabel.stringValue = @"上次数据";
        self.hudView.homeFiveHourCard.windowLabel.stringValue = @"上次数据";
        self.hudView.homeWeeklyCard.windowLabel.stringValue = @"上次数据";
    }
    self.hudView.planCard.valueLabel.stringValue = plan ?: @"当前未返回";
    self.hudView.planCard.subtitleLabel.stringValue = s.accountErrorText.length > 0 ? s.accountErrorText : [NSString stringWithFormat:@"不显示邮箱 · %@", FormatAge(s.accountUpdatedAt)];
    self.hudView.homePlanCard.valueLabel.stringValue = self.hudView.planCard.valueLabel.stringValue;
    self.hudView.homePlanCard.subtitleLabel.stringValue = self.hudView.planCard.subtitleLabel.stringValue;
    if (s.usageAvailable) {
        self.hudView.usageCard.valueLabel.stringValue = s.todayUsageAvailable ? [NSString stringWithFormat:@"今日 %@", FormatTokens(s.todayTokens)] : (s.latestUsageDate.length > 0 ? @"今日待结算" : @"今日未返回");
        NSString *trend = @"";
        if (s.previousSevenDayTokens > 0) {
            double change = ((double)s.sevenDayTokens / s.previousSevenDayTokens - 1.0) * 100.0;
            trend = [NSString stringWithFormat:@" · %@%.0f%%", change >= 0 ? @"↑" : @"↓", fabs(change)];
        }
        NSString *through = s.todayUsageAvailable ? @"" : (s.latestUsageDate.length > 0 ? [NSString stringWithFormat:@"截至%@ · ", FormatUsageDate(s.latestUsageDate)] : @"");
        self.hudView.usageCard.subtitleLabel.stringValue = s.usageErrorText.length > 0 ? [NSString stringWithFormat:@"%@7天 %@%@ · 更新失败", through, FormatTokens(s.sevenDayTokens), trend] : [NSString stringWithFormat:@"%@7天 %@%@ · 不等于额度", through, FormatTokens(s.sevenDayTokens), trend];
        self.hudView.homeUsageCard.valueLabel.stringValue = self.hudView.usageCard.valueLabel.stringValue;
        self.hudView.homeUsageCard.subtitleLabel.stringValue = self.hudView.usageCard.subtitleLabel.stringValue;
    } else {
        self.hudView.usageCard.valueLabel.stringValue = @"当前未返回";
        self.hudView.usageCard.subtitleLabel.stringValue = s.usageErrorText ?: @"不会换算成额度百分比";
        self.hudView.homeUsageCard.valueLabel.stringValue = self.hudView.usageCard.valueLabel.stringValue;
        self.hudView.homeUsageCard.subtitleLabel.stringValue = self.hudView.usageCard.subtitleLabel.stringValue;
    }
    if (s.modelQuotaAvailable) {
        self.hudView.modelQuotaCard.valueLabel.stringValue = [NSString stringWithFormat:@"%@ %.0f%%", s.modelQuotaName, s.modelQuotaRemainingPercent];
        self.hudView.modelQuotaCard.subtitleLabel.stringValue = [NSString stringWithFormat:@"%@ · %@", s.modelQuotaWindowLabel ?: @"独立额度", FormatReset(s.modelQuotaResetAt)];
        self.hudView.homeModelQuotaCard.valueLabel.stringValue = self.hudView.modelQuotaCard.valueLabel.stringValue;
        self.hudView.homeModelQuotaCard.subtitleLabel.stringValue = self.hudView.modelQuotaCard.subtitleLabel.stringValue;
    }
    [self.hudView setModelQuotaVisible:self.showModelQuota && s.modelQuotaAvailable];
    [self.hudView setHomeModelQuotaVisible:self.homeShowModelQuota && s.modelQuotaAvailable];
    BOOL quotaStale = (self.showFiveHourQuota || self.showWeeklyQuota || self.showModelQuota || self.homeShowFiveHour || self.homeShowWeekly || self.homeShowModelQuota) && s.quotaUpdatedAt > 0 && NSDate.date.timeIntervalSince1970 - s.quotaUpdatedAt > 180;
    BOOL anyError = ((self.showFiveHourQuota || self.showWeeklyQuota || self.showModelQuota || self.homeShowFiveHour || self.homeShowWeekly || self.homeShowModelQuota) && s.quotaErrorText.length > 0) || ((self.showPlan || self.homeShowPlan) && s.accountErrorText.length > 0) || ((self.showUsage || self.homeShowUsage) && s.usageErrorText.length > 0);
    self.hudView.codexFreshnessLabel.stringValue = [self freshnessText:s fiveHour:self.showFiveHourQuota weekly:self.showWeeklyQuota plan:self.showPlan usage:self.showUsage model:self.showModelQuota];
    self.hudView.codexFreshnessLabel.textColor = quotaStale ? NSColor.systemRedColor : (anyError ? NSColor.systemOrangeColor : NSColor.tertiaryLabelColor);
    self.hudView.codexStatusLabel.textColor = quotaStale ? NSColor.systemRedColor : [self accentColor];
    self.hudView.homeCodexStatusLabel.textColor = self.hudView.codexStatusLabel.textColor;
    NSString *homeCodexFreshness = [self freshnessText:s fiveHour:self.homeShowFiveHour weekly:self.homeShowWeekly plan:self.homeShowPlan usage:self.homeShowUsage model:self.homeShowModelQuota];
    self.hudView.homeFreshnessLabel.stringValue = [homeCodexFreshness isEqualToString:@"未启用Codex显示模块"] ? @"电脑 刚刚" : [NSString stringWithFormat:@"电脑 刚刚 · %@", homeCodexFreshness];
    self.hudView.homeFreshnessLabel.textColor = self.hudView.codexFreshnessLabel.textColor;
    [self updateDetailLabels];
}

- (void)updateDetailLabels {
    if (!self.hudView || self.compact) return;
    for (NSTextField *label in self.hudView.detailLabels) { label.hidden = YES; label.stringValue = @""; }
    NSInteger row = 0;
    if (self.currentPage == 0) {
        CodexStatusSnapshot *s = self.codexProvider.snapshot;
        NSMutableArray<NSString *> *quotaParts = [NSMutableArray array];
        if (self.homeShowFiveHour) [quotaParts addObject:s.fiveHourAvailable ? [NSString stringWithFormat:@"5小时 %.0f%%", s.fiveHourRemainingPercent] : @"5小时未返回"];
        if (self.homeShowWeekly) [quotaParts addObject:s.weeklyAvailable ? [NSString stringWithFormat:@"每周 %.0f%%", s.weeklyRemainingPercent] : @"每周未返回"];
        if (quotaParts.count && row < 5) { self.hudView.detailLabels[row].stringValue = [NSString stringWithFormat:@"额度摘要   %@", [quotaParts componentsJoinedByString:@" · "]]; self.hudView.detailLabels[row++].hidden = NO; }
        if (self.homeShowPlan && row < 5) { self.hudView.detailLabels[row].stringValue = [NSString stringWithFormat:@"订阅类型   %@", s.accountAvailable ? FormatPlan(s.planType) : @"当前未返回"]; self.hudView.detailLabels[row++].hidden = NO; }
        if (self.homeShowUsage && row < 5) {
            NSString *today = s.todayUsageAvailable ? [NSString stringWithFormat:@"今日 %@", FormatTokens(s.todayTokens)] : (s.latestUsageDate.length > 0 ? @"今日待结算" : @"今日未返回");
            self.hudView.detailLabels[row].stringValue = s.usageAvailable ? [NSString stringWithFormat:@"用量摘要   %@ · 7天 %@", today, FormatTokens(s.sevenDayTokens)] : @"用量摘要   当前接口未返回";
            self.hudView.detailLabels[row++].hidden = NO;
        }
        NativeSnapshot *n = self.lastSnapshot;
        if ((self.homeShowDiagnosis || self.homeShowSystem) && row < 5) { self.hudView.detailLabels[row].stringValue = n ? [NSString stringWithFormat:@"电脑摘要   CPU %.0f%% · 内存压力 %@ · Swap %.1fG", n.systemCPUPercent, n.memoryPressureText, n.swapUsedGiB] : @"电脑摘要   正在采样"; self.hudView.detailLabels[row++].hidden = NO; }
        if (self.homeShowAttribution && row < 5) { self.hudView.detailLabels[row].stringValue = n ? [NSString stringWithFormat:@"Codex摘要  CPU %.1f%% · 内存 %.1fG", n.codexCPUPercent, n.codexMemoryGiB] : @"Codex摘要  正在采样"; self.hudView.detailLabels[row++].hidden = NO; }
        if (row < 5) { self.hudView.detailLabels[row].stringValue = @"齿轮可一次查看并修改全部显示选项"; self.hudView.detailLabels[row++].hidden = NO; }
    } else if (self.currentPage == 1) {
        CodexStatusSnapshot *s = self.codexProvider.snapshot;
        if (self.showFiveHourQuota && row < 5) { self.hudView.detailLabels[row].stringValue = s.fiveHourAvailable ? [NSString stringWithFormat:@"5小时额度  剩余 %.0f%% · %@", s.fiveHourRemainingPercent, FormatReset(s.fiveHourResetAt)] : @"5小时额度  当前接口未返回"; self.hudView.detailLabels[row++].hidden = NO; }
        if (self.showWeeklyQuota && row < 5) { self.hudView.detailLabels[row].stringValue = s.weeklyAvailable ? [NSString stringWithFormat:@"每周额度   剩余 %.0f%% · %@", s.weeklyRemainingPercent, FormatReset(s.weeklyResetAt)] : @"每周额度   当前接口未返回"; self.hudView.detailLabels[row++].hidden = NO; }
        if (self.showPlan && row < 5) { self.hudView.detailLabels[row].stringValue = [NSString stringWithFormat:@"订阅类型   %@", s.accountAvailable ? FormatPlan(s.planType) : @"当前未返回"]; self.hudView.detailLabels[row++].hidden = NO; }
        if (self.showUsage && row < 5) {
            NSString *today = s.todayUsageAvailable ? [NSString stringWithFormat:@"今日 %@", FormatTokens(s.todayTokens)] : (s.latestUsageDate.length > 0 ? @"今日待结算" : @"今日未返回");
            self.hudView.detailLabels[row].stringValue = s.usageAvailable ? [NSString stringWithFormat:@"用量趋势   %@ · 7天 %@ · 连续%ld天", today, FormatTokens(s.sevenDayTokens), (long)s.currentStreakDays] : @"用量趋势   当前接口未返回";
            self.hudView.detailLabels[row++].hidden = NO;
        }
        if (self.showModelQuota && row < 5) { self.hudView.detailLabels[row].stringValue = s.modelQuotaAvailable ? [NSString stringWithFormat:@"模型额度   %@ %.0f%%", s.modelQuotaName, s.modelQuotaRemainingPercent] : @"模型额度   当前未返回"; self.hudView.detailLabels[row++].hidden = NO; }
        if (row < 5) { self.hudView.detailLabels[row].stringValue = @"数据来源   Codex本机账户接口 · 60秒读取一次"; self.hudView.detailLabels[row++].hidden = NO; }
    } else if (self.lastSnapshot) {
        NativeSnapshot *s = self.lastSnapshot;
        if (self.showAttribution && row < 5) { self.hudView.detailLabels[row].stringValue = [NSString stringWithFormat:@"Codex %ld进程 · 渲染%ld · 工具%ld · 最大 %.1fG", (long)s.codexProcessCount, (long)s.codexRendererCount, (long)s.codexHelperCount, s.codexLargestGiB]; self.hudView.detailLabels[row++].hidden = NO; }
        if (self.showAttribution && row < 5) { self.hudView.detailLabels[row].stringValue = [NSString stringWithFormat:@"Codex磁盘  读 %@ · 写 %@", FormatRate(s.codexDiskReadMBps), FormatRate(s.codexDiskWriteMBps)]; self.hudView.detailLabels[row++].hidden = NO; }
        if (self.showSystem && row < 5) { self.hudView.detailLabels[row].stringValue = [NSString stringWithFormat:@"物理网卡  ↓ %@ · ↑ %@ · 压缩内存 %.1fG", FormatRate(s.networkDownMBps), FormatRate(s.networkUpMBps), s.compressedGiB]; self.hudView.detailLabels[row++].hidden = NO; }
        if (self.showSystem && row < 5) { self.hudView.detailLabels[row].stringValue = [NSString stringWithFormat:@"CPU前列  %@", [self topAppsText:s.topCPUApps cpu:YES]]; self.hudView.detailLabels[row++].hidden = NO; }
        if (self.showMemoryApps && row < 5) { self.hudView.detailLabels[row].stringValue = [NSString stringWithFormat:@"内存前列 %@", [self topAppsText:s.topMemoryApps cpu:NO]]; self.hudView.detailLabels[row++].hidden = NO; }
    }
}

- (void)recordSnapshot:(NativeSnapshot *)snapshot statusCode:(NSString *)statusCode {
    if (!self.historyEnabled) { [self.minuteSamples removeAllObjects]; self.minuteStart = 0; return; }
    if (self.minuteStart <= 0) self.minuteStart = snapshot.timestamp;
    [self.minuteSamples addObject:snapshot];
    if (snapshot.timestamp - self.minuteStart < 60.0) return;
    double systemSum = 0, systemMax = 0, codexCPUSum = 0, codexCPUMax = 0, memorySum = 0, memoryMax = 0, memoryPercentSum = 0, memoryPercentMax = 0;
    double processSum = 0, processMax = 0, largestMax = 0, downSum = 0, downMax = 0, upSum = 0, upMax = 0, readSum = 0, writeSum = 0;
    NSInteger pressureWorst = 0, thermalWorst = 0, worstRank = 0;
    NSString *worstCode = statusCode;
    NSDictionary *ranks = @{@"normal": @0, @"cpu_high": @1, @"cpu_other": @2, @"cpu_codex": @2, @"memory_warning": @3, @"memory_codex_warning": @3, @"thermal": @4, @"memory_critical": @5, @"memory_codex_critical": @5};
    for (NativeSnapshot *item in self.minuteSamples) {
        systemSum += item.systemCPUPercent; systemMax = MAX(systemMax, item.systemCPUPercent); codexCPUSum += item.codexCPUPercent; codexCPUMax = MAX(codexCPUMax, item.codexCPUPercent);
        memorySum += item.codexMemoryGiB; memoryMax = MAX(memoryMax, item.codexMemoryGiB); memoryPercentSum += item.codexMemoryPercent; memoryPercentMax = MAX(memoryPercentMax, item.codexMemoryPercent);
        processSum += item.codexProcessCount; processMax = MAX(processMax, item.codexProcessCount); largestMax = MAX(largestMax, item.codexLargestGiB);
        pressureWorst = MAX(pressureWorst, item.memoryPressureLevel); thermalWorst = MAX(thermalWorst, item.thermalLevel);
        downSum += item.networkDownMBps; downMax = MAX(downMax, item.networkDownMBps); upSum += item.networkUpMBps; upMax = MAX(upMax, item.networkUpMBps); readSum += item.codexDiskReadMBps; writeSum += item.codexDiskWriteMBps;
        NSString *itemCode = [self statusForSnapshot:item][@"code"]; NSInteger rank = [ranks[itemCode] integerValue]; if (rank > worstRank) { worstRank = rank; worstCode = itemCode; }
    }
    double count = MAX(1, self.minuteSamples.count); NativeSnapshot *last = self.minuteSamples.lastObject;
    NSDateFormatter *day = [NSDateFormatter new]; day.dateFormat = @"yyyy-MM-dd"; day.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
    NSDateFormatter *stamp = [NSDateFormatter new]; stamp.dateFormat = @"yyyy-MM-dd HH:mm:ss"; stamp.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
    NSDate *date = [NSDate dateWithTimeIntervalSince1970:last.timestamp];
    NSURL *file = [self.historyDirectory URLByAppendingPathComponent:[[day stringFromDate:date] stringByAppendingString:@".csv"]];
    NSString *header = @"timestamp,epoch,samples,system_cpu_avg,system_cpu_max,codex_cpu_avg,codex_cpu_max,codex_memory_gib_avg,codex_memory_gib_max,codex_memory_pct_avg,codex_memory_pct_max,codex_processes_avg,codex_processes_max,codex_largest_gib_max,memory_pressure_worst,swap_used_gib,swap_delta_10m_mib,thermal_worst,network_down_mbps_avg,network_down_mbps_max,network_up_mbps_avg,network_up_mbps_max,codex_disk_read_mbps_avg,codex_disk_write_mbps_avg,bottleneck\n";
    NSString *line = [NSString stringWithFormat:@"%@,%.0f,%ld,%.2f,%.2f,%.2f,%.2f,%.3f,%.3f,%.2f,%.2f,%.1f,%.0f,%.3f,%ld,%.3f,%.1f,%ld,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%@\n", [stamp stringFromDate:date], last.timestamp, (long)self.minuteSamples.count, systemSum/count, systemMax, codexCPUSum/count, codexCPUMax, memorySum/count, memoryMax, memoryPercentSum/count, memoryPercentMax, processSum/count, processMax, largestMax, (long)pressureWorst, last.swapUsedGiB, last.swapDelta10MinMiB, (long)thermalWorst, downSum/count, downMax, upSum/count, upMax, readSum/count, writeSum/count, CSVSafe(worstCode)];
    if (![NSFileManager.defaultManager fileExistsAtPath:file.path]) [header writeToURL:file atomically:YES encoding:NSUTF8StringEncoding error:nil];
    NSFileHandle *handle = [NSFileHandle fileHandleForWritingAtPath:file.path];
    if (handle) { [handle seekToEndOfFile]; [handle writeData:[line dataUsingEncoding:NSUTF8StringEncoding]]; [handle closeFile]; }
    [self.minuteSamples removeAllObjects]; self.minuteStart = snapshot.timestamp;
}

- (void)resizePanel {
    NSRect old = self.panel.frame; NSSize size = [self panelSize]; NSSize baseSize = [self basePanelSize]; NSScreen *screen = self.panel.screen ?: NSScreen.mainScreen;
    CGFloat y = old.origin.y; if (screen && NSMidY(old) >= NSMidY(screen.visibleFrame)) y = NSMaxY(old) - size.height;
    [self.panel setFrame:NSMakeRect(old.origin.x, y, size.width, size.height) display:YES animate:YES];
    self.hudView.frame = NSMakeRect(0, 0, size.width, size.height);
    self.hudView.bounds = NSMakeRect(0, 0, baseSize.width, baseSize.height);
    [self.hudView setCompact:self.compact]; [self updateDetailLabels]; [self savePosition];
}

- (void)toggleDetail:(id)sender { self.compact = !self.compact; [NSUserDefaults.standardUserDefaults setBool:self.compact forKey:@"compact"]; [self resizePanel]; }
- (void)toggleFiveHourQuota:(id)sender { self.showFiveHourQuota = !self.showFiveHourQuota; [NSUserDefaults.standardUserDefaults setBool:self.showFiveHourQuota forKey:@"showFiveHourQuota"]; [self.hudView setFiveHourQuotaVisible:self.showFiveHourQuota]; [self startCodexProviderIfNeeded]; [self updateCodexDisplay]; }
- (void)toggleWeeklyQuota:(id)sender { self.showWeeklyQuota = !self.showWeeklyQuota; [NSUserDefaults.standardUserDefaults setBool:self.showWeeklyQuota forKey:@"showWeeklyQuota"]; [self.hudView setWeeklyQuotaVisible:self.showWeeklyQuota]; [self startCodexProviderIfNeeded]; [self updateCodexDisplay]; }
- (void)togglePlan:(id)sender { self.showPlan = !self.showPlan; [NSUserDefaults.standardUserDefaults setBool:self.showPlan forKey:@"showPlan"]; [self.hudView setPlanVisible:self.showPlan]; [self startCodexProviderIfNeeded]; [self updateCodexDisplay]; }
- (void)toggleUsage:(id)sender { self.showUsage = !self.showUsage; [NSUserDefaults.standardUserDefaults setBool:self.showUsage forKey:@"showUsage"]; [self.hudView setUsageVisible:self.showUsage]; [self startCodexProviderIfNeeded]; [self updateCodexDisplay]; }
- (void)toggleModelQuota:(id)sender { self.showModelQuota = !self.showModelQuota; [NSUserDefaults.standardUserDefaults setBool:self.showModelQuota forKey:@"showModelQuota"]; [self.hudView setModelQuotaVisible:self.showModelQuota && self.codexProvider.snapshot.modelQuotaAvailable]; [self startCodexProviderIfNeeded]; [self updateCodexDisplay]; }
- (void)toggleSystem:(id)sender { self.showSystem = !self.showSystem; [NSUserDefaults.standardUserDefaults setBool:self.showSystem forKey:@"showSystem"]; [self.hudView setSystemVisible:self.showSystem]; [self updateDetailLabels]; }
- (void)toggleAttribution:(id)sender { self.showAttribution = !self.showAttribution; [NSUserDefaults.standardUserDefaults setBool:self.showAttribution forKey:@"showAttribution"]; [self.hudView setAttributionVisible:self.showAttribution]; [self updateDetailLabels]; }
- (void)toggleTrend:(id)sender { self.showTrend = !self.showTrend; [NSUserDefaults.standardUserDefaults setBool:self.showTrend forKey:@"showTrend"]; [self.hudView setTrendVisible:self.showTrend]; [self updateDetailLabels]; }
- (void)toggleMemoryApps:(id)sender { self.showMemoryApps = !self.showMemoryApps; [NSUserDefaults.standardUserDefaults setBool:self.showMemoryApps forKey:@"showMemoryApps"]; [self.hudView setMemoryAppsVisible:self.showMemoryApps]; [self resizePanel]; }
- (void)toggleHomeFiveHour:(id)sender { self.homeShowFiveHour = !self.homeShowFiveHour; [NSUserDefaults.standardUserDefaults setBool:self.homeShowFiveHour forKey:@"homeShowFiveHour"]; [self.hudView setHomeFiveHourVisible:self.homeShowFiveHour]; [self startCodexProviderIfNeeded]; [self updateCodexDisplay]; [self resizePanel]; }
- (void)toggleHomeWeekly:(id)sender { self.homeShowWeekly = !self.homeShowWeekly; [NSUserDefaults.standardUserDefaults setBool:self.homeShowWeekly forKey:@"homeShowWeekly"]; [self.hudView setHomeWeeklyVisible:self.homeShowWeekly]; [self startCodexProviderIfNeeded]; [self updateCodexDisplay]; [self resizePanel]; }
- (void)toggleHomePlan:(id)sender { self.homeShowPlan = !self.homeShowPlan; [NSUserDefaults.standardUserDefaults setBool:self.homeShowPlan forKey:@"homeShowPlan"]; [self.hudView setHomePlanVisible:self.homeShowPlan]; [self startCodexProviderIfNeeded]; [self updateCodexDisplay]; [self resizePanel]; }
- (void)toggleHomeUsage:(id)sender { self.homeShowUsage = !self.homeShowUsage; [NSUserDefaults.standardUserDefaults setBool:self.homeShowUsage forKey:@"homeShowUsage"]; [self.hudView setHomeUsageVisible:self.homeShowUsage]; [self startCodexProviderIfNeeded]; [self updateCodexDisplay]; [self resizePanel]; }
- (void)toggleHomeModelQuota:(id)sender { self.homeShowModelQuota = !self.homeShowModelQuota; [NSUserDefaults.standardUserDefaults setBool:self.homeShowModelQuota forKey:@"homeShowModelQuota"]; [self.hudView setHomeModelQuotaVisible:self.homeShowModelQuota && self.codexProvider.snapshot.modelQuotaAvailable]; [self startCodexProviderIfNeeded]; [self updateCodexDisplay]; [self resizePanel]; }
- (void)toggleHomeDiagnosis:(id)sender { self.homeShowDiagnosis = !self.homeShowDiagnosis; [NSUserDefaults.standardUserDefaults setBool:self.homeShowDiagnosis forKey:@"homeShowDiagnosis"]; [self.hudView setHomeDiagnosisVisible:self.homeShowDiagnosis]; [self resizePanel]; }
- (void)toggleHomeSystem:(id)sender { self.homeShowSystem = !self.homeShowSystem; [NSUserDefaults.standardUserDefaults setBool:self.homeShowSystem forKey:@"homeShowSystem"]; [self.hudView setHomeSystemVisible:self.homeShowSystem]; [self resizePanel]; }
- (void)toggleHomeAttribution:(id)sender { self.homeShowAttribution = !self.homeShowAttribution; [NSUserDefaults.standardUserDefaults setBool:self.homeShowAttribution forKey:@"homeShowAttribution"]; [self.hudView setHomeAttributionVisible:self.homeShowAttribution]; [self resizePanel]; }
- (void)toggleHomeTrend:(id)sender { self.homeShowTrend = !self.homeShowTrend; [NSUserDefaults.standardUserDefaults setBool:self.homeShowTrend forKey:@"homeShowTrend"]; [self.hudView setHomeTrendVisible:self.homeShowTrend]; [self resizePanel]; }
- (void)toggleHomeMemoryApps:(id)sender { self.homeShowMemoryApps = !self.homeShowMemoryApps; [NSUserDefaults.standardUserDefaults setBool:self.homeShowMemoryApps forKey:@"homeShowMemoryApps"]; [self.hudView setHomeMemoryAppsVisible:self.homeShowMemoryApps]; [self resizePanel]; }
- (void)toggleHistory:(id)sender { self.historyEnabled = !self.historyEnabled; [NSUserDefaults.standardUserDefaults setBool:self.historyEnabled forKey:@"historyEnabled"]; }
- (void)toggleAlwaysOnTop:(id)sender { self.alwaysOnTop = !self.alwaysOnTop; [NSUserDefaults.standardUserDefaults setBool:self.alwaysOnTop forKey:@"alwaysOnTop"]; self.panel.level = self.alwaysOnTop ? NSFloatingWindowLevel : NSNormalWindowLevel; [self.hudView setAlwaysOnTop:self.alwaysOnTop]; if (self.alwaysOnTop) [self.panel orderFrontRegardless]; }
- (void)minimizeToDock:(id)sender { [self.panel miniaturize:nil]; }
- (void)toggleCollapsed:(id)sender { self.collapsed = !self.collapsed; [self.hudView setCollapsed:self.collapsed]; [self resizePanel]; }
- (void)selectPage:(NSMenuItem *)sender { self.currentPage = sender.tag; [NSUserDefaults.standardUserDefaults setInteger:self.currentPage forKey:@"currentPage"]; [self.hudView setPage:self.currentPage]; [self updateDetailLabels]; [self resizePanel]; }
- (void)setOpacity:(NSMenuItem *)sender { self.backgroundOpacity = sender.tag / 100.0; [NSUserDefaults.standardUserDefaults setDouble:self.backgroundOpacity forKey:@"opacity"]; [self.hudView setBackgroundOpacity:self.backgroundOpacity]; }
- (void)setAccent:(NSMenuItem *)sender { self.accentName = sender.representedObject; [NSUserDefaults.standardUserDefaults setObject:self.accentName forKey:@"accentName"]; self.hudView.accentColor = [self accentColor]; self.hudView.codexStatusLabel.textColor = self.hudView.accentColor; [self updateDisplay]; [self updateCodexDisplay]; }
- (void)setRefresh:(NSMenuItem *)sender { self.refreshInterval = sender.tag; [NSUserDefaults.standardUserDefaults setDouble:self.refreshInterval forKey:@"refreshInterval"]; [self startSystemTimer]; [self updateDisplay]; }
- (void)chooseWindowScale:(NSMenuItem *)sender {
    self.windowScale = sender.tag / 100.0;
    [NSUserDefaults.standardUserDefaults setDouble:self.windowScale forKey:@"windowScale"];
    [self resizePanel];
}
- (void)setWindowScaleFromControl:(NSSegmentedControl *)sender {
    NSArray<NSNumber *> *scales = @[@0.85, @1.0, @1.2];
    NSInteger index = MAX(0, MIN((NSInteger)scales.count - 1, sender.selectedSegment));
    self.windowScale = scales[(NSUInteger)index].doubleValue;
    [NSUserDefaults.standardUserDefaults setDouble:self.windowScale forKey:@"windowScale"];
    [self resizePanel];
}

- (void)moveToCorner:(NSString *)corner {
    NSScreen *screen = self.panel.screen ?: NSScreen.mainScreen; if (!screen) return;
    NSRect v = screen.visibleFrame; NSSize s = [self panelSize]; CGFloat p = 16;
    BOOL right = [corner containsString:@"Right"], top = [corner containsString:@"top"] || [corner containsString:@"Top"];
    CGFloat x = right ? NSMaxX(v) - s.width - p : NSMinX(v) + p;
    CGFloat y = top ? NSMaxY(v) - s.height - p : NSMinY(v) + p;
    [self.panel setFrame:NSMakeRect(x, y, s.width, s.height) display:YES]; [self savePosition];
}
- (void)moveCornerFromMenu:(NSMenuItem *)sender { [self moveToCorner:sender.representedObject]; }
- (void)openDataFolder:(id)sender { [[NSWorkspace sharedWorkspace] openURL:self.historyDirectory]; }
- (NSString *)launchAgentPath { return [NSHomeDirectory() stringByAppendingPathComponent:@"Library/LaunchAgents/com.codexmonitorhud.app.plist"]; }
- (BOOL)launchAtLoginEnabled { return [NSFileManager.defaultManager fileExistsAtPath:[self launchAgentPath]]; }
- (void)toggleLaunchAtLogin:(id)sender {
    NSString *path = [self launchAgentPath];
    if ([self launchAtLoginEnabled]) {
        [NSFileManager.defaultManager removeItemAtPath:path error:nil];
        return;
    }
    NSString *logDirectory = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/CodexSystemMonitor/logs"];
    [NSFileManager.defaultManager createDirectoryAtPath:logDirectory withIntermediateDirectories:YES attributes:nil error:nil];
    NSDictionary *plist = @{
        @"Label": @"com.codexmonitorhud.app",
        @"ProgramArguments": @[NSBundle.mainBundle.executablePath],
        @"RunAtLoad": @YES,
        @"LimitLoadToSessionType": @"Aqua",
        @"ProcessType": @"Interactive",
        @"StandardOutPath": [logDirectory stringByAppendingPathComponent:@"hud.stdout.log"],
        @"StandardErrorPath": [logDirectory stringByAppendingPathComponent:@"hud.stderr.log"]
    };
    [plist writeToFile:path atomically:YES];
}
- (void)quitHUD:(id)sender { [NSApp terminate:nil]; }

- (NSMenuItem *)item:(NSString *)title action:(SEL)action state:(BOOL)state {
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:@""]; item.target = self; item.state = state ? NSControlStateValueOn : NSControlStateValueOff; return item;
}

- (NSButton *)settingsCheckbox:(NSString *)title action:(SEL)action state:(BOOL)state {
    NSButton *button = [NSButton checkboxWithTitle:title target:self action:action];
    button.state = state ? NSControlStateValueOn : NSControlStateValueOff;
    button.font = [NSFont systemFontOfSize:13 weight:NSFontWeightRegular];
    button.controlSize = NSControlSizeRegular;
    button.toolTip = state ? @"当前显示，点击后隐藏" : @"当前隐藏，点击后显示";
    return button;
}

- (NSBox *)settingsGroup:(NSString *)title controls:(NSArray<NSView *> *)controls {
    NSBox *box = [NSBox new];
    box.title = title;
    box.titleFont = [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold];
    box.boxType = NSBoxPrimary;
    NSStackView *stack = [NSStackView stackViewWithViews:controls];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeLeading;
    stack.spacing = 7;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [box.contentView addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[
        [stack.leadingAnchor constraintEqualToAnchor:box.contentView.leadingAnchor constant:12],
        [stack.trailingAnchor constraintLessThanOrEqualToAnchor:box.contentView.trailingAnchor constant:-12],
        [stack.topAnchor constraintEqualToAnchor:box.contentView.topAnchor constant:8],
        [stack.bottomAnchor constraintEqualToAnchor:box.contentView.bottomAnchor constant:-12]
    ]];
    return box;
}

- (NSView *)settingsContentView {
    NSArray<NSView *> *homeControls = @[
        [self settingsCheckbox:@"5小时额度" action:@selector(toggleHomeFiveHour:) state:self.homeShowFiveHour],
        [self settingsCheckbox:@"每周额度" action:@selector(toggleHomeWeekly:) state:self.homeShowWeekly],
        [self settingsCheckbox:@"订阅类型" action:@selector(toggleHomePlan:) state:self.homeShowPlan],
        [self settingsCheckbox:@"Token用量" action:@selector(toggleHomeUsage:) state:self.homeShowUsage],
        [self settingsCheckbox:@"模型专属额度" action:@selector(toggleHomeModelQuota:) state:self.homeShowModelQuota],
        [self settingsCheckbox:@"瓶颈判断" action:@selector(toggleHomeDiagnosis:) state:self.homeShowDiagnosis],
        [self settingsCheckbox:@"电脑核心状态" action:@selector(toggleHomeSystem:) state:self.homeShowSystem],
        [self settingsCheckbox:@"Codex性能占用" action:@selector(toggleHomeAttribution:) state:self.homeShowAttribution],
        [self settingsCheckbox:@"内存占用排行" action:@selector(toggleHomeMemoryApps:) state:self.homeShowMemoryApps],
        [self settingsCheckbox:@"CPU趋势" action:@selector(toggleHomeTrend:) state:self.homeShowTrend]
    ];
    NSArray<NSView *> *codexControls = @[
        [self settingsCheckbox:@"5小时额度" action:@selector(toggleFiveHourQuota:) state:self.showFiveHourQuota],
        [self settingsCheckbox:@"每周额度" action:@selector(toggleWeeklyQuota:) state:self.showWeeklyQuota],
        [self settingsCheckbox:@"订阅类型" action:@selector(togglePlan:) state:self.showPlan],
        [self settingsCheckbox:@"Token用量趋势" action:@selector(toggleUsage:) state:self.showUsage],
        [self settingsCheckbox:@"模型专属额度" action:@selector(toggleModelQuota:) state:self.showModelQuota]
    ];
    NSArray<NSView *> *computerControls = @[
        [self settingsCheckbox:@"电脑核心状态" action:@selector(toggleSystem:) state:self.showSystem],
        [self settingsCheckbox:@"Codex性能占用" action:@selector(toggleAttribution:) state:self.showAttribution],
        [self settingsCheckbox:@"内存占用排行" action:@selector(toggleMemoryApps:) state:self.showMemoryApps],
        [self settingsCheckbox:@"10分钟CPU趋势" action:@selector(toggleTrend:) state:self.showTrend]
    ];
    NSBox *homeBox = [self settingsGroup:@"主页显示（勾选 = 显示）" controls:homeControls];
    NSBox *codexBox = [self settingsGroup:@"Codex页面" controls:codexControls];
    NSBox *computerBox = [self settingsGroup:@"电脑性能页面" controls:computerControls];
    NSStackView *moduleColumns = [NSStackView stackViewWithViews:@[homeBox, codexBox, computerBox]];
    moduleColumns.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    moduleColumns.distribution = NSStackViewDistributionFillEqually;
    moduleColumns.alignment = NSLayoutAttributeTop;
    moduleColumns.spacing = 12;

    NSSegmentedControl *sizeControl = [NSSegmentedControl segmentedControlWithLabels:@[@"小 85%", @"标准 100%", @"大 120%"] trackingMode:NSSegmentSwitchTrackingSelectOne target:self action:@selector(setWindowScaleFromControl:)];
    sizeControl.selectedSegment = fabs(self.windowScale - 0.85) < 0.05 ? 0 : (fabs(self.windowScale - 1.2) < 0.05 ? 2 : 1);
    sizeControl.controlSize = NSControlSizeRegular;
    NSTextField *sizeLabel = [NSTextField labelWithString:@"悬浮窗整体大小"];
    sizeLabel.font = [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold];
    NSArray<NSView *> *behaviorControls = @[
        [self settingsCheckbox:@"始终置顶" action:@selector(toggleAlwaysOnTop:) state:self.alwaysOnTop],
        [self settingsCheckbox:@"显示详细信息" action:@selector(toggleDetail:) state:!self.compact],
        [self settingsCheckbox:@"保存每分钟趋势" action:@selector(toggleHistory:) state:self.historyEnabled],
        [self settingsCheckbox:@"登录后自动启动" action:@selector(toggleLaunchAtLogin:) state:[self launchAtLoginEnabled]]
    ];
    NSStackView *sizeStack = [NSStackView stackViewWithViews:@[sizeLabel, sizeControl]];
    sizeStack.orientation = NSUserInterfaceLayoutOrientationVertical; sizeStack.alignment = NSLayoutAttributeLeading; sizeStack.spacing = 7;
    NSBox *behaviorBox = [self settingsGroup:@"常驻与显示" controls:behaviorControls];
    NSBox *sizeBox = [self settingsGroup:@"外观" controls:@[sizeStack]];
    NSStackView *bottom = [NSStackView stackViewWithViews:@[behaviorBox, sizeBox]];
    bottom.orientation = NSUserInterfaceLayoutOrientationHorizontal; bottom.distribution = NSStackViewDistributionFillEqually; bottom.spacing = 12;

    NSTextField *hint = [NSTextField labelWithString:@"所有勾选状态集中显示；修改后立即生效。更多颜色、透明度、刷新频率仍可在悬浮窗右键菜单调整。"];
    hint.font = [NSFont systemFontOfSize:12]; hint.textColor = NSColor.secondaryLabelColor;
    NSStackView *root = [NSStackView stackViewWithViews:@[hint, moduleColumns, bottom]];
    root.orientation = NSUserInterfaceLayoutOrientationVertical; root.alignment = NSLayoutAttributeLeading; root.spacing = 14;
    root.translatesAutoresizingMaskIntoConstraints = NO;
    NSView *content = [NSView new]; [content addSubview:root];
    [NSLayoutConstraint activateConstraints:@[
        [root.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:18], [root.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-18],
        [root.topAnchor constraintEqualToAnchor:content.topAnchor constant:16], [root.bottomAnchor constraintEqualToAnchor:content.bottomAnchor constant:-18],
        [moduleColumns.widthAnchor constraintEqualToAnchor:root.widthAnchor], [bottom.widthAnchor constraintEqualToAnchor:root.widthAnchor]
    ]];
    return content;
}

- (void)showSettingsWindow:(id)sender {
    if (self.settingsWindow.visible) { [NSApp activateIgnoringOtherApps:YES]; [self.settingsWindow makeKeyAndOrderFront:nil]; return; }
    self.settingsWindow = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 760, 520) styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable) backing:NSBackingStoreBuffered defer:NO];
    self.settingsWindow.title = @"Codex Monitor HUD 设置";
    self.settingsWindow.contentView = [self settingsContentView];
    self.settingsWindow.releasedWhenClosed = NO;
    self.settingsWindow.level = NSFloatingWindowLevel;
    [self.settingsWindow center];
    [NSApp activateIgnoringOtherApps:YES];
    [self.settingsWindow makeKeyAndOrderFront:nil];
}

- (NSMenu *)settingsMenu {
    NSMenu *menu = [NSMenu new];
    NSMenuItem *allSettings = [[NSMenuItem alloc] initWithTitle:@"显示设置…" action:@selector(showSettingsWindow:) keyEquivalent:@""];
    allSettings.target = self; [menu addItem:allSettings]; [menu addItem:NSMenuItem.separatorItem];
    NSMenu *page = [NSMenu new];
    NSArray<NSString *> *pageNames = @[@"主页", @"Codex", @"电脑性能"];
    for (NSUInteger index = 0; index < pageNames.count; index++) { NSMenuItem *i = [self item:pageNames[index] action:@selector(selectPage:) state:self.currentPage == (NSInteger)index]; i.tag = (NSInteger)index; [page addItem:i]; }
    NSMenuItem *pageRoot = [[NSMenuItem alloc] initWithTitle:@"显示页面" action:nil keyEquivalent:@""]; pageRoot.submenu = page; [menu addItem:pageRoot];
    NSMenu *homeModules = [NSMenu new];
    [homeModules addItem:[self item:@"5小时额度" action:@selector(toggleHomeFiveHour:) state:self.homeShowFiveHour]];
    [homeModules addItem:[self item:@"每周额度" action:@selector(toggleHomeWeekly:) state:self.homeShowWeekly]];
    [homeModules addItem:[self item:@"订阅类型" action:@selector(toggleHomePlan:) state:self.homeShowPlan]];
    [homeModules addItem:[self item:@"Token用量" action:@selector(toggleHomeUsage:) state:self.homeShowUsage]];
    [homeModules addItem:[self item:@"模型专属额度" action:@selector(toggleHomeModelQuota:) state:self.homeShowModelQuota]];
    [homeModules addItem:NSMenuItem.separatorItem];
    [homeModules addItem:[self item:@"瓶颈判断" action:@selector(toggleHomeDiagnosis:) state:self.homeShowDiagnosis]];
    [homeModules addItem:[self item:@"电脑核心状态" action:@selector(toggleHomeSystem:) state:self.homeShowSystem]];
    [homeModules addItem:[self item:@"Codex性能占用" action:@selector(toggleHomeAttribution:) state:self.homeShowAttribution]];
    [homeModules addItem:[self item:@"内存占用排行" action:@selector(toggleHomeMemoryApps:) state:self.homeShowMemoryApps]];
    [homeModules addItem:[self item:@"CPU趋势" action:@selector(toggleHomeTrend:) state:self.homeShowTrend]];
    NSMenuItem *homeRoot = [[NSMenuItem alloc] initWithTitle:@"主页内容" action:nil keyEquivalent:@""]; homeRoot.submenu = homeModules; [menu addItem:homeRoot];
    NSMenu *modules = [NSMenu new];
    [modules addItem:[self item:@"5小时额度" action:@selector(toggleFiveHourQuota:) state:self.showFiveHourQuota]];
    [modules addItem:[self item:@"每周额度" action:@selector(toggleWeeklyQuota:) state:self.showWeeklyQuota]];
    [modules addItem:[self item:@"订阅类型" action:@selector(togglePlan:) state:self.showPlan]];
    [modules addItem:[self item:@"Token用量趋势" action:@selector(toggleUsage:) state:self.showUsage]];
    [modules addItem:[self item:@"电脑核心状态" action:@selector(toggleSystem:) state:self.showSystem]];
    [modules addItem:[self item:@"Codex性能占用" action:@selector(toggleAttribution:) state:self.showAttribution]];
    [modules addItem:[self item:@"10分钟CPU趋势" action:@selector(toggleTrend:) state:self.showTrend]];
    [modules addItem:[self item:@"内存占用排行" action:@selector(toggleMemoryApps:) state:self.showMemoryApps]];
    NSMenuItem *moduleRoot = [[NSMenuItem alloc] initWithTitle:@"分页面内容" action:nil keyEquivalent:@""]; moduleRoot.submenu = modules; [menu addItem:moduleRoot];
    NSMenu *advanced = [NSMenu new];
    [advanced addItem:[self item:@"模型专属额度（有数据时显示）" action:@selector(toggleModelQuota:) state:self.showModelQuota]];
    NSMenuItem *advancedRoot = [[NSMenuItem alloc] initWithTitle:@"高级显示" action:nil keyEquivalent:@""]; advancedRoot.submenu = advanced; [menu addItem:advancedRoot];
    [menu addItem:NSMenuItem.separatorItem];

    NSMenu *opacity = [NSMenu new];
    for (NSNumber *value in @[@55, @70, @82, @90, @100]) { NSMenuItem *i = [self item:[NSString stringWithFormat:@"%@%%", value] action:@selector(setOpacity:) state:fabs(self.backgroundOpacity - value.doubleValue/100.0) < 0.01]; i.tag = value.integerValue; [opacity addItem:i]; }
    NSMenuItem *opacityRoot = [[NSMenuItem alloc] initWithTitle:@"透明度" action:nil keyEquivalent:@""]; opacityRoot.submenu = opacity; [menu addItem:opacityRoot];
    NSMenu *sizes = [NSMenu new];
    for (NSNumber *value in @[@85, @100, @120]) { NSMenuItem *i = [self item:[NSString stringWithFormat:@"%@%%", value] action:@selector(chooseWindowScale:) state:fabs(self.windowScale - value.doubleValue/100.0) < 0.01]; i.tag = value.integerValue; [sizes addItem:i]; }
    NSMenuItem *sizeRoot = [[NSMenuItem alloc] initWithTitle:@"整体大小" action:nil keyEquivalent:@""]; sizeRoot.submenu = sizes; [menu addItem:sizeRoot];
    NSMenu *colors = [NSMenu new];
    for (NSArray *pair in @[@[@"绿色", @"green"], @[@"蓝色", @"blue"], @[@"紫色", @"purple"], @[@"橙色", @"orange"]]) { NSMenuItem *i = [self item:pair[0] action:@selector(setAccent:) state:[self.accentName isEqualToString:pair[1]]]; i.representedObject = pair[1]; [colors addItem:i]; }
    NSMenuItem *colorRoot = [[NSMenuItem alloc] initWithTitle:@"强调颜色" action:nil keyEquivalent:@""]; colorRoot.submenu = colors; [menu addItem:colorRoot];
    NSMenu *refresh = [NSMenu new];
    for (NSNumber *value in @[@5, @10, @20]) { NSMenuItem *i = [self item:[NSString stringWithFormat:@"%@秒", value] action:@selector(setRefresh:) state:fabs(self.refreshInterval - value.doubleValue) < 0.1]; i.tag = value.integerValue; [refresh addItem:i]; }
    NSMenuItem *refreshRoot = [[NSMenuItem alloc] initWithTitle:@"电脑数据刷新" action:nil keyEquivalent:@""]; refreshRoot.submenu = refresh; [menu addItem:refreshRoot];
    NSMenu *positions = [NSMenu new];
    for (NSArray *pair in @[@[@"左下角", @"bottomLeft"], @[@"左上角", @"topLeft"], @[@"右下角", @"bottomRight"], @[@"右上角", @"topRight"]]) { NSMenuItem *i = [self item:pair[0] action:@selector(moveCornerFromMenu:) state:NO]; i.representedObject = pair[1]; [positions addItem:i]; }
    NSMenuItem *positionRoot = [[NSMenuItem alloc] initWithTitle:@"屏幕位置" action:nil keyEquivalent:@""]; positionRoot.submenu = positions; [menu addItem:positionRoot];
    [menu addItem:[self item:@"始终置顶" action:@selector(toggleAlwaysOnTop:) state:self.alwaysOnTop]];
    [menu addItem:[self item:@"最小化到程序栏" action:@selector(minimizeToDock:) state:NO]];
    [menu addItem:[self item:(self.collapsed ? @"退出窄栏模式" : @"窄栏模式") action:@selector(toggleCollapsed:) state:self.collapsed]];
    [menu addItem:[self item:@"显示详细信息" action:@selector(toggleDetail:) state:!self.compact]];
    [menu addItem:[self item:@"保存每分钟趋势" action:@selector(toggleHistory:) state:self.historyEnabled]];
    [menu addItem:[self item:@"登录后自动启动" action:@selector(toggleLaunchAtLogin:) state:[self launchAtLoginEnabled]]];
    [menu addItem:NSMenuItem.separatorItem];
    NSMenuItem *open = [[NSMenuItem alloc] initWithTitle:@"打开趋势数据文件夹" action:@selector(openDataFolder:) keyEquivalent:@""]; open.target = self; [menu addItem:open];
    NSMenuItem *quit = [[NSMenuItem alloc] initWithTitle:@"退出悬浮窗" action:@selector(quitHUD:) keyEquivalent:@""]; quit.target = self; [menu addItem:quit];
    return menu;
}

@end

static void CountSettingsControls(NSView *view, NSInteger *checkboxCount, NSInteger *hiddenFiveHourCount, NSInteger *hiddenPlanCount, NSInteger *selectedSizeSegment) {
    if ([view isKindOfClass:NSButton.class]) {
        NSButton *button = (NSButton *)view;
        (*checkboxCount)++;
        if ([button.title isEqualToString:@"5小时额度"] && button.state == NSControlStateValueOff) (*hiddenFiveHourCount)++;
        if ([button.title isEqualToString:@"订阅类型"] && button.state == NSControlStateValueOff) (*hiddenPlanCount)++;
    } else if ([view isKindOfClass:NSSegmentedControl.class]) {
        NSSegmentedControl *control = (NSSegmentedControl *)view;
        if (control.segmentCount == 3) *selectedSizeSegment = control.selectedSegment;
    }
    for (NSView *subview in view.subviews) CountSettingsControls(subview, checkboxCount, hiddenFiveHourCount, hiddenPlanCount, selectedSizeSegment);
}

static int RunUIDiagnostic(void) {
    [NSApplication sharedApplication];
    AppDelegate *delegate = [AppDelegate new];
    delegate.homeShowFiveHour = NO; delegate.showFiveHourQuota = NO;
    delegate.homeShowPlan = NO; delegate.showPlan = NO;
    delegate.homeShowWeekly = YES; delegate.showWeeklyQuota = YES;
    delegate.homeShowUsage = YES; delegate.showUsage = YES;
    delegate.windowScale = 1.2; delegate.compact = YES; delegate.currentPage = 0;
    NSView *settings = [delegate settingsContentView];
    NSInteger checkboxCount = 0, hiddenFiveHourCount = 0, hiddenPlanCount = 0, selectedSizeSegment = -1;
    CountSettingsControls(settings, &checkboxCount, &hiddenFiveHourCount, &hiddenPlanCount, &selectedSizeSegment);
    BOOL settingsPass = checkboxCount == 23 && hiddenFiveHourCount == 2 && hiddenPlanCount == 2;
    BOOL scalePass = selectedSizeSegment == 2 && fabs([delegate panelSize].width - 516.0) < 0.1;
    delegate.windowScale = 0.85; scalePass = scalePass && fabs([delegate panelSize].width - 366.0) < 0.1;
    delegate.windowScale = 1.0; scalePass = scalePass && fabs([delegate panelSize].width - 430.0) < 0.1;
    delegate.hudView = [[HUDView alloc] initWithFrame:NSMakeRect(0, 0, 430, 600)];
    [delegate.hudView setHomeFiveHourVisible:NO]; [delegate.hudView setFiveHourQuotaVisible:NO];
    [delegate.hudView setHomePlanVisible:NO]; [delegate.hudView setPlanVisible:NO];
    [delegate.hudView setHomeWeeklyVisible:YES]; [delegate.hudView setWeeklyQuotaVisible:YES];
    BOOL cardVisibilityPass = delegate.hudView.homeFiveHourCard.hidden && delegate.hudView.fiveHourCard.hidden && delegate.hudView.homePlanCard.hidden && delegate.hudView.planCard.hidden && !delegate.hudView.homeWeeklyCard.hidden && !delegate.hudView.weeklyCard.hidden;
    delegate.compact = NO; delegate.homeShowUsage = NO; delegate.homeShowDiagnosis = NO; delegate.homeShowSystem = NO; delegate.homeShowAttribution = NO;
    CodexStatusProvider *provider = [CodexStatusProvider new]; delegate.codexProvider = provider;
    provider.snapshot.quotaAvailable = YES; provider.snapshot.weeklyAvailable = YES; provider.snapshot.weeklyRemainingPercent = 42;
    provider.snapshot.accountAvailable = YES; provider.snapshot.planType = @"pro"; provider.snapshot.accountErrorText = @"test-error";
    [delegate updateDetailLabels];
    NSMutableArray<NSString *> *visibleDetails = [NSMutableArray array];
    for (NSTextField *label in delegate.hudView.detailLabels) if (!label.hidden) [visibleDetails addObject:label.stringValue];
    NSString *details = [visibleDetails componentsJoinedByString:@" | "];
    NSString *status = [delegate visibleCodexStatus:provider.snapshot fiveHour:NO weekly:YES plan:NO usage:NO model:NO];
    BOOL hiddenContentPass = [details containsString:@"每周"] && ![details containsString:@"5小时"] && ![details containsString:@"订阅"] && [status isEqualToString:@"Codex数据正常"];
    printf("settings_visibility_test=%s\n", settingsPass ? "pass" : "fail");
    printf("window_scale_test=%s\n", scalePass ? "pass" : "fail");
    printf("hidden_content_test=%s\n", cardVisibilityPass && hiddenContentPass ? "pass" : "fail");
    return settingsPass && scalePass && cardVisibilityPass && hiddenContentPass ? 0 : 5;
}

static int RunDiagnostic(void) {
    NativeSampler *sampler = [NativeSampler new]; [sampler sample];
    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:5.1]];
    NativeSnapshot *s = [sampler sample];
    printf("system_cpu=%.2f\n", s.systemCPUPercent); printf("codex_cpu=%.2f\n", s.codexCPUPercent); printf("codex_cpu_cores=%.2f\n", s.codexCPUCores);
    printf("codex_memory_gib=%.3f\n", s.codexMemoryGiB); printf("total_memory_gib=%.3f\n", s.totalMemoryGiB); printf("codex_memory_percent=%.2f\n", s.codexMemoryPercent);
    printf("codex_processes=%ld\n", (long)s.codexProcessCount); printf("memory_pressure=%ld\n", (long)s.memoryPressureLevel); printf("swap_gib=%.3f\n", s.swapUsedGiB); printf("thermal=%ld\n", (long)s.thermalLevel);
    return s.codexProcessCount > 0 && s.totalMemoryGiB > 0 ? 0 : 2;
}

static int RunCodexDiagnostic(void) {
    CodexStatusProvider *provider = [CodexStatusProvider new];
    [provider start];
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:10.0];
    while (!(provider.snapshot.quotaAvailable && provider.snapshot.accountAvailable && provider.snapshot.usageAvailable) && deadline.timeIntervalSinceNow > 0) {
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
    }
    CodexStatusSnapshot *s = provider.snapshot;
    printf("quota_available=%s\n", s.quotaAvailable ? "true" : "false");
    printf("five_hour_available=%s\n", s.fiveHourAvailable ? "true" : "false");
    if (s.fiveHourAvailable) printf("five_hour_remaining=%.0f\n", s.fiveHourRemainingPercent);
    printf("weekly_available=%s\n", s.weeklyAvailable ? "true" : "false");
    if (s.weeklyAvailable) printf("weekly_remaining=%.0f\n", s.weeklyRemainingPercent);
    printf("account_available=%s\n", s.accountAvailable ? "true" : "false");
    if (s.accountAvailable) printf("plan_type=%s\n", s.planType.UTF8String ?: "unknown");
    printf("usage_available=%s\n", s.usageAvailable ? "true" : "false");
    if (s.usageAvailable) {
        printf("today_usage_available=%s\n", s.todayUsageAvailable ? "true" : "false");
        printf("usage_through_date=%s\n", s.latestUsageDate.UTF8String ?: "none");
        printf("seven_day_tokens=%lld\n", s.sevenDayTokens);
    }
    printf("model_quota_available=%s\n", s.modelQuotaAvailable ? "true" : "false");
    if (s.modelQuotaAvailable) printf("model_quota_name=%s\n", s.modelQuotaName.UTF8String ?: "unknown");
    [provider stop];
    return s.quotaAvailable ? 0 : 3;
}

static int RunLogicDiagnostic(void) {
    BOOL cpuTimebasePass = fabs(NativeRawCPUPercentFromAbsoluteTime(24000000, 1.0, 125, 3) - 100.0) < 0.001;
    NSCalendar *calendar = [NSCalendar calendarWithIdentifier:NSCalendarIdentifierGregorian];
    calendar.timeZone = NSTimeZone.localTimeZone;
    NSDate *now = [calendar startOfDayForDate:NSDate.date];
    NSDateFormatter *formatter = [NSDateFormatter new]; formatter.calendar = calendar; formatter.timeZone = calendar.timeZone; formatter.dateFormat = @"yyyy-MM-dd";
    NSMutableArray *buckets = [NSMutableArray array];
    for (NSDictionary *sample in @[@{@"offset": @0, @"tokens": @100}, @{@"offset": @1, @"tokens": @50}, @{@"offset": @3, @"tokens": @25}, @{@"offset": @8, @"tokens": @40}]) {
        NSDate *date = [calendar dateByAddingUnit:NSCalendarUnitDay value:-[sample[@"offset"] integerValue] toDate:now options:0];
        [buckets addObject:@{@"startDate": [formatter stringFromDate:date], @"tokens": sample[@"tokens"]}];
    }
    NSDictionary *usage = CodexCalendarUsage(buckets, now);
    NSString *todayKey = [formatter stringFromDate:now];
    BOOL currentPass = [usage[@"todayAvailable"] boolValue] && [usage[@"today"] longLongValue] == 100 && [usage[@"recent"] longLongValue] == 175 && [usage[@"previous"] longLongValue] == 40 && [usage[@"latestDate"] isEqualToString:todayKey];
    NSMutableArray *delayedBuckets = [NSMutableArray array];
    for (NSDictionary *sample in @[@{@"offset": @1, @"tokens": @50}, @{@"offset": @2, @"tokens": @25}, @{@"offset": @8, @"tokens": @40}]) {
        NSDate *date = [calendar dateByAddingUnit:NSCalendarUnitDay value:-[sample[@"offset"] integerValue] toDate:now options:0];
        [delayedBuckets addObject:@{@"startDate": [formatter stringFromDate:date], @"tokens": sample[@"tokens"]}];
    }
    NSDictionary *delayed = CodexCalendarUsage(delayedBuckets, now);
    NSString *yesterdayKey = [formatter stringFromDate:[calendar dateByAddingUnit:NSCalendarUnitDay value:-1 toDate:now options:0]];
    BOOL delayedPass = ![delayed[@"todayAvailable"] boolValue] && [delayed[@"today"] longLongValue] == 0 && [delayed[@"recent"] longLongValue] == 75 && [delayed[@"previous"] longLongValue] == 40 && [delayed[@"latestDate"] isEqualToString:yesterdayKey];
    NSDictionary *empty = CodexCalendarUsage(@[], now);
    BOOL emptyPass = ![empty[@"todayAvailable"] boolValue] && [empty[@"recent"] longLongValue] == 0 && [empty[@"previous"] longLongValue] == 0 && [empty[@"latestDate"] length] == 0;
    BOOL pass = currentPass && delayedPass && emptyPass && cpuTimebasePass;
    printf("calendar_usage_test=%s\n", pass ? "pass" : "fail");
    printf("cpu_timebase_test=%s\n", cpuTimebasePass ? "pass" : "fail");
    return pass ? 0 : 4;
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        if (argc > 1 && strcmp(argv[1], "--diagnostic") == 0) return RunDiagnostic();
        if (argc > 1 && strcmp(argv[1], "--codex-diagnostic") == 0) return RunCodexDiagnostic();
        if (argc > 1 && strcmp(argv[1], "--logic-diagnostic") == 0) return RunLogicDiagnostic();
        if (argc > 1 && strcmp(argv[1], "--ui-diagnostic") == 0) return RunUIDiagnostic();
        NSApplication *application = NSApplication.sharedApplication; AppDelegate *delegate = [AppDelegate new]; application.delegate = delegate; [application run];
    }
    return 0;
}
