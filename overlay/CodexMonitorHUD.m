#import <Cocoa/Cocoa.h>
#import "NativeSampler.h"
#import "CodexStatusProvider.h"
#import "CodexCostHistory.h"
#import "OpenAIServiceStatus.h"
#import "HUDView.h"
#import "UpdateManager.h"
#import <UserNotifications/UserNotifications.h>
#import <errno.h>
#import <fcntl.h>
#import <sys/file.h>
#import <unistd.h>

static NSTimeInterval const HUDAutomaticUpdateCheckInterval = 24.0 * 60.0 * 60.0;
static CGFloat const HUDMinimumWindowScale = 0.75;
static CGFloat const HUDScreenEdgeMargin = 24.0;
static NSString *const HUDTaskCenterBundleIdentifier = @"com.xiaoliedao.codex-monitor-task-center";
static NSString *const HUDTaskCenterReleasePage = @"https://github.com/Ryuaaa/codex-monitor-hud/releases";
static int HUDSingletonLockFD = -1;

typedef NS_ENUM(NSInteger, HUDSingletonLockResult) {
    HUDSingletonLockResultAcquired,
    HUDSingletonLockResultAlreadyRunning,
    HUDSingletonLockResultUnavailable
};

static HUDSingletonLockResult HUDTryAcquireSingletonLockAtPath(NSString *path, int *lockFD) {
    NSString *directory = path.stringByDeletingLastPathComponent;
    if (directory.length > 0 && ![NSFileManager.defaultManager createDirectoryAtPath:directory withIntermediateDirectories:YES attributes:nil error:nil]) {
        return HUDSingletonLockResultUnavailable;
    }
    int fd = open(path.fileSystemRepresentation, O_CREAT | O_RDWR, 0600);
    if (fd < 0) return HUDSingletonLockResultUnavailable;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        int lockError = errno;
        close(fd);
        return (lockError == EWOULDBLOCK || lockError == EAGAIN) ? HUDSingletonLockResultAlreadyRunning : HUDSingletonLockResultUnavailable;
    }
    if (lockFD) *lockFD = fd;
    else close(fd);
    return HUDSingletonLockResultAcquired;
}

static NSString *HUDApplicationSingletonLockPath(void) {
    return [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/CodexSystemMonitor/hud-instance.lock"];
}

static BOOL HUDAcquireApplicationSingletonLock(void) {
    HUDSingletonLockResult result = HUDTryAcquireSingletonLockAtPath(HUDApplicationSingletonLockPath(), &HUDSingletonLockFD);
    if (result == HUDSingletonLockResultAlreadyRunning) return NO;
    if (result == HUDSingletonLockResultUnavailable) fprintf(stderr, "warning: singleton lock unavailable\n");
    return YES;
}

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

static NSString *FormatQuotaWindow(double durationMins) {
    NSInteger minutes = (NSInteger)llround(durationMins);
    if (minutes <= 0) return @"周期未返回";
    if (minutes == 300) return @"5小时";
    if (minutes == 10080) return @"每周";
    if (minutes % 1440 == 0) return [NSString stringWithFormat:@"%ld天", (long)(minutes / 1440)];
    if (minutes % 60 == 0) return [NSString stringWithFormat:@"%ld小时", (long)(minutes / 60)];
    return [NSString stringWithFormat:@"%ld分钟", (long)minutes];
}

static BOOL HUDWeeklyAlertEligible(BOOL enabled,
                                   BOOL weeklyAvailable,
                                   NSString *weeklyDataState,
                                   NSTimeInterval weeklyResetAt,
                                   NSTimeInterval now,
                                   BOOL consumptionAvailable,
                                   double consumedPercent,
                                   double thresholdPercent) {
    return enabled && weeklyAvailable && [weeklyDataState isEqualToString:@"live"] &&
           weeklyResetAt > now && consumptionAvailable && consumedPercent >= thresholdPercent;
}

static NSString *HUDWeeklyAlertCycleKey(NSTimeInterval weeklyResetAt) {
    return [NSString stringWithFormat:@"%.0f", weeklyResetAt];
}

static void HUDApplyQuotaDataState(HUDQuotaCard *card, NSString *state, BOOL available, double remaining, NSTimeInterval resetAt, NSString *reachedType) {
    if ([state isEqualToString:@"previous"] && available) {
        card.windowLabel.stringValue = @"上次数据";
        card.resetLabel.stringValue = [NSString stringWithFormat:@"本轮未返回 · %@", FormatReset(resetAt)];
    } else if ([state isEqualToString:@"expired"]) {
        card.windowLabel.stringValue = @"已过期";
        card.valueLabel.stringValue = @"上次数据已过期";
        card.valueLabel.font = [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold];
        card.valueLabel.textColor = NSColor.systemOrangeColor;
        card.resetLabel.stringValue = @"等待官方返回新周期";
    } else if (available && reachedType.length > 0 && remaining <= 0.01) {
        card.windowLabel.stringValue = @"官方触顶";
        card.valueLabel.textColor = NSColor.systemRedColor;
    }
}

static NSString *FormatUSD(double value) {
    if (value > 0 && value < 0.01) return [NSString stringWithFormat:@"$%.3f", value];
    return [NSString stringWithFormat:@"$%.2f", MAX(0, value)];
}

static NSString *FormatForecastHeadline(NSString *headline) {
    if ([headline isEqualToString:@"可撑到重置"]) return @"可撑到重置";
    if ([headline isEqualToString:@"可能提前用完"]) return @"可能提前用完";
    if ([headline isEqualToString:@"近期用量平稳"]) return @"近期平稳";
    return headline.length > 0 ? headline : @"积累中";
}

static NSString *FormatDuration(NSInteger seconds) {
    if (seconds < 60) return [NSString stringWithFormat:@"%ld秒", (long)MAX(0, seconds)];
    NSInteger minutes = seconds / 60;
    if (minutes < 60) return [NSString stringWithFormat:@"%ld分钟", (long)minutes];
    NSInteger hours = minutes / 60;
    NSInteger rest = minutes % 60;
    return rest ? [NSString stringWithFormat:@"%ld小时%ld分", (long)hours, (long)rest] : [NSString stringWithFormat:@"%ld小时", (long)hours];
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

static NSColor *HUDQuotaColor(BOOL available, double remaining, NSColor *normal) {
    if (!available) return NSColor.tertiaryLabelColor;
    if (remaining <= 10) return NSColor.systemRedColor;
    if (remaining <= 25) return NSColor.systemOrangeColor;
    return normal ?: NSColor.systemGreenColor;
}

static NSString *FormatAge(NSTimeInterval timestamp) {
    if (timestamp <= 0) return @"连接中";
    NSInteger age = MAX(0, (NSInteger)(NSDate.date.timeIntervalSince1970 - timestamp));
    if (age < 10) return @"刚刚";
    if (age < 60) return [NSString stringWithFormat:@"%ld秒前", (long)age];
    return [NSString stringWithFormat:@"%ld分钟前", (long)(age / 60)];
}

static CGFloat HUDScaleForContentSize(NSSize contentSize, NSSize baseSize) {
    if (contentSize.width <= 0 || baseSize.width <= 0) return 1.0;
    return MAX(HUDMinimumWindowScale, contentSize.width / baseSize.width);
}

static CGFloat HUDUniformScaleForProposedContentSize(NSSize proposedSize, NSSize baseSize, CGFloat currentScale) {
    if (proposedSize.width <= 0 || proposedSize.height <= 0 || baseSize.width <= 0 || baseSize.height <= 0) {
        return MAX(HUDMinimumWindowScale, currentScale > 0 ? currentScale : 1.0);
    }
    CGFloat widthScale = proposedSize.width / baseSize.width;
    CGFloat heightScale = proposedSize.height / baseSize.height;
    CGFloat referenceScale = currentScale > 0 ? currentScale : 1.0;
    CGFloat scale = fabs(widthScale - referenceScale) >= fabs(heightScale - referenceScale) ? widthScale : heightScale;
    return MAX(HUDMinimumWindowScale, scale);
}

static NSSize HUDContentSizeForUniformScale(NSSize baseSize, CGFloat scale) {
    CGFloat safeScale = MAX(HUDMinimumWindowScale, scale);
    return NSMakeSize(baseSize.width * safeScale, baseSize.height * safeScale);
}

static BOOL HUDTimestampIsStale(NSTimeInterval timestamp, NSTimeInterval maximumAge) {
    return timestamp > 0 && NSDate.date.timeIntervalSince1970 - timestamp > maximumAge;
}

static NSString *FormatModuleState(NSTimeInterval timestamp, NSString *error, NSTimeInterval maximumAge) {
    if (error.length > 0) return timestamp > 0 ? [NSString stringWithFormat:@"失败·上次%@", FormatAge(timestamp)] : @"失败";
    if (HUDTimestampIsStale(timestamp, maximumAge)) return [NSString stringWithFormat:@"已过期·上次%@", FormatAge(timestamp)];
    return FormatAge(timestamp);
}

static NSArray<NSString *> *HUDDefaultHomeCodexOrder(void) { return @[@"activity", @"recent", @"quota", @"insights", @"tokenWindows", @"cost", @"forecast", @"service", @"quotaDetails", @"history"]; }
static NSArray<NSString *> *HUDDefaultHomeComputerOrder(void) { return @[@"summary", @"attribution", @"memory", @"trend"]; }
static NSTimeInterval HUDCodexActivityRefreshInterval(NSInteger activeTaskCount) { return activeTaskCount > 0 ? 5.0 : 20.0; }

static NSArray<NSString *> *HUDSanitizedOrder(id savedValue, NSArray<NSString *> *defaults) {
    NSArray *saved = [savedValue isKindOfClass:NSArray.class] ? savedValue : @[];
    NSMutableArray<NSString *> *result = [NSMutableArray array];
    for (id value in saved) if ([value isKindOfClass:NSString.class] && [defaults containsObject:value] && ![result containsObject:value]) [result addObject:value];
    for (NSString *value in defaults) if (![result containsObject:value]) [result addObject:value];
    return result;
}

static NSArray<NSDictionary<NSString *, NSString *> *> *HUDOrderItems(NSArray<NSString *> *order, NSDictionary<NSString *, NSString *> *titles) {
    NSMutableArray *items = [NSMutableArray array];
    for (NSString *identifier in order) if (titles[identifier]) [items addObject:@{ @"id": identifier, @"title": titles[identifier] }];
    return items;
}

static NSPasteboardType const HUDModuleOrderPasteboardType = @"com.codexmonitorhud.module-order";

@interface HUDFlippedView : NSView @end
@implementation HUDFlippedView
- (BOOL)isFlipped { return YES; }
@end

@interface HUDModuleOrderController : NSObject <NSTableViewDataSource, NSTableViewDelegate>
@property(nonatomic, copy) NSString *orderKey;
@property(nonatomic, strong) NSMutableArray<NSDictionary<NSString *, NSString *> *> *items;
@property(nonatomic, strong) NSTableView *tableView;
@property(nonatomic, strong) NSScrollView *scrollView;
@property(nonatomic, copy) void (^changed)(NSArray<NSString *> *order);
- (instancetype)initWithOrderKey:(NSString *)orderKey items:(NSArray<NSDictionary<NSString *, NSString *> *> *)items changed:(void (^)(NSArray<NSString *> *order))changed;
@end

@implementation HUDModuleOrderController
- (instancetype)initWithOrderKey:(NSString *)orderKey items:(NSArray<NSDictionary<NSString *, NSString *> *> *)items changed:(void (^)(NSArray<NSString *> *))changed {
    self = [super init]; if (!self) return nil;
    _orderKey = [orderKey copy]; _items = [items mutableCopy]; _changed = [changed copy];
    _tableView = [NSTableView new]; _tableView.headerView = nil; _tableView.rowHeight = 25; _tableView.intercellSpacing = NSMakeSize(0, 1);
    NSTableColumn *column = [[NSTableColumn alloc] initWithIdentifier:@"module"]; column.width = 500; column.minWidth = 240; column.resizingMask = NSTableColumnAutoresizingMask; [_tableView addTableColumn:column];
    _tableView.dataSource = self; _tableView.delegate = self; [_tableView registerForDraggedTypes:@[HUDModuleOrderPasteboardType]];
    _scrollView = [NSScrollView new]; _scrollView.documentView = _tableView; _scrollView.hasVerticalScroller = items.count > 5; _scrollView.drawsBackground = NO; _scrollView.borderType = NSBezelBorder;
    [_scrollView.heightAnchor constraintEqualToConstant:MIN(150, MAX(60, items.count * 27 + 8))].active = YES;
    return self;
}
- (NSInteger)numberOfRowsInTableView:(NSTableView *)tableView { return self.items.count; }
- (NSView *)tableView:(NSTableView *)tableView viewForTableColumn:(NSTableColumn *)tableColumn row:(NSInteger)row {
    NSTableCellView *cell = [tableView makeViewWithIdentifier:@"moduleCell" owner:self];
    if (!cell) {
        cell = [NSTableCellView new]; cell.identifier = @"moduleCell";
        NSTextField *label = [NSTextField labelWithString:@""]; label.translatesAutoresizingMaskIntoConstraints = NO; label.font = [NSFont systemFontOfSize:12.5];
        [cell addSubview:label]; cell.textField = label;
        [NSLayoutConstraint activateConstraints:@[[label.leadingAnchor constraintEqualToAnchor:cell.leadingAnchor constant:6], [label.trailingAnchor constraintLessThanOrEqualToAnchor:cell.trailingAnchor constant:-4], [label.centerYAnchor constraintEqualToAnchor:cell.centerYAnchor]]];
    }
    cell.textField.stringValue = [NSString stringWithFormat:@"≡  %@", self.items[row][@"title"]];
    return cell;
}
- (id<NSPasteboardWriting>)tableView:(NSTableView *)tableView pasteboardWriterForRow:(NSInteger)row {
    NSPasteboardItem *item = [NSPasteboardItem new];
    [item setString:[NSString stringWithFormat:@"%@|%@", self.orderKey, self.items[row][@"id"]] forType:HUDModuleOrderPasteboardType];
    return item;
}
- (NSDragOperation)tableView:(NSTableView *)tableView validateDrop:(id<NSDraggingInfo>)info proposedRow:(NSInteger)row proposedDropOperation:(NSTableViewDropOperation)dropOperation {
    NSString *payload = [info.draggingPasteboard stringForType:HUDModuleOrderPasteboardType];
    if (![payload hasPrefix:[self.orderKey stringByAppendingString:@"|"]]) return NSDragOperationNone;
    [tableView setDropRow:row dropOperation:NSTableViewDropAbove]; return NSDragOperationMove;
}
- (BOOL)tableView:(NSTableView *)tableView acceptDrop:(id<NSDraggingInfo>)info row:(NSInteger)row dropOperation:(NSTableViewDropOperation)dropOperation {
    NSString *payload = [info.draggingPasteboard stringForType:HUDModuleOrderPasteboardType];
    NSString *identifier = [[payload componentsSeparatedByString:@"|"] lastObject];
    NSUInteger source = [self.items indexOfObjectPassingTest:^BOOL(NSDictionary *item, __unused NSUInteger idx, __unused BOOL *stop) { return [item[@"id"] isEqualToString:identifier]; }];
    if (source == NSNotFound) return NO;
    NSDictionary *moved = self.items[source]; [self.items removeObjectAtIndex:source];
    NSInteger destination = MAX(0, MIN((NSInteger)self.items.count, row - (source < (NSUInteger)row ? 1 : 0)));
    [self.items insertObject:moved atIndex:destination]; [self.tableView reloadData];
    NSMutableArray<NSString *> *order = [NSMutableArray array]; for (NSDictionary *item in self.items) [order addObject:item[@"id"]];
    if (self.changed) self.changed(order); return YES;
}
@end

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property(nonatomic, strong) NSPanel *panel;
@property(nonatomic, strong) NSPanel *settingsWindow;
@property(nonatomic, strong) HUDView *hudView;
@property(nonatomic, strong) NSTimer *systemTimer;
@property(nonatomic, strong) NSTimer *codexTimer;
@property(nonatomic, strong) NSTimer *serviceStatusTimer;
@property(nonatomic, strong) NSTimer *updateTimer;
@property(nonatomic, strong) NativeSampler *sampler;
@property(nonatomic, strong) CodexStatusProvider *codexProvider;
@property(nonatomic, strong) HUDOpenAIServiceStatusProvider *serviceStatusProvider;
@property(nonatomic, strong) HUDUpdateManager *updateManager;
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
@property(nonatomic) BOOL showLocalCost;
@property(nonatomic) BOOL showTokenWindows;
@property(nonatomic) BOOL showQuotaForecast;
@property(nonatomic) BOOL showQuotaDetails;
@property(nonatomic) BOOL showPeakDailyTokens;
@property(nonatomic) BOOL weeklyConsumptionAlertEnabled;
@property(nonatomic) double weeklyConsumptionAlertThreshold;
@property(nonatomic, copy) NSString *weeklyConsumptionAlertMode;
@property(nonatomic) BOOL weeklyConsumptionSystemNotificationEnabled;
@property(nonatomic) BOOL showServiceStatus;
@property(nonatomic) BOOL showTaskActivity;
@property(nonatomic) BOOL showRecentTasks;
@property(nonatomic) BOOL showLongestTurn;
@property(nonatomic) BOOL showLongestStreak;
@property(nonatomic) BOOL showSystem;
@property(nonatomic) BOOL showAttribution;
@property(nonatomic) BOOL showTrend;
@property(nonatomic) BOOL showMemoryApps;
@property(nonatomic) BOOL homeShowFiveHour;
@property(nonatomic) BOOL homeShowWeekly;
@property(nonatomic) BOOL homeShowPlan;
@property(nonatomic) BOOL homeShowUsage;
@property(nonatomic) BOOL homeShowModelQuota;
@property(nonatomic) BOOL homeShowLocalCost;
@property(nonatomic) BOOL homeShowTokenWindows;
@property(nonatomic) BOOL homeShowQuotaForecast;
@property(nonatomic) BOOL homeShowQuotaDetails;
@property(nonatomic) BOOL homeShowPeakDailyTokens;
@property(nonatomic) BOOL homeShowServiceStatus;
@property(nonatomic) BOOL homeShowTaskActivity;
@property(nonatomic) BOOL homeShowRecentTasks;
@property(nonatomic) BOOL homeShowLongestTurn;
@property(nonatomic) BOOL homeShowLongestStreak;
@property(nonatomic) BOOL homeShowDiagnosis;
@property(nonatomic) BOOL homeShowSystem;
@property(nonatomic) BOOL homeShowAttribution;
@property(nonatomic) BOOL homeShowTrend;
@property(nonatomic) BOOL homeShowMemoryApps;
@property(nonatomic) BOOL historyEnabled;
@property(nonatomic) BOOL alwaysOnTop;
@property(nonatomic) BOOL positionLocked;
@property(nonatomic) BOOL collapsed;
@property(nonatomic) CGFloat backgroundOpacity;
@property(nonatomic) CGFloat windowScale;
@property(nonatomic, copy) NSString *accentName;
@property(nonatomic, copy) NSArray<NSString *> *homeCodexModuleOrder;
@property(nonatomic, copy) NSArray<NSString *> *homeComputerModuleOrder;
@property(nonatomic, strong) NSMutableArray<HUDModuleOrderController *> *settingsOrderControllers;
@property(nonatomic) NSTimeInterval lastCodexAccountFetchAt;
@property(nonatomic) NSTimeInterval lastActivityRefreshRequestAt;
@property(nonatomic) NSTimeInterval lastCostHistoryFetchAt;
@property(nonatomic) NSTimeInterval lastServiceStatusFetchAt;
@property(nonatomic, strong) NSTextField *updateStatusLabel;
@property(nonatomic, strong) NSButton *updateButton;
@property(nonatomic, strong) NSTextField *weeklyConsumptionThresholdLabel;
@property(nonatomic) BOOL updateCheckInProgress;
@property(nonatomic) BOOL updateInstalling;
- (void)showSettingsWindow:(id)sender;
- (void)openTaskCenter:(id)sender;
- (NSView *)settingsContentView;
- (NSSize)frameSizeForContentSize:(NSSize)contentSize;
- (CGFloat)maximumWindowScaleForBaseSize:(NSSize)baseSize;
- (void)configureResizeLimitsForBaseSize:(NSSize)baseSize contentSize:(NSSize)contentSize;
- (void)updateHUDGeometryForContentSize:(NSSize)contentSize baseSize:(NSSize)baseSize;
- (void)configureSamplingAndTimers;
- (void)scheduleCodexRefreshTimer;
- (void)startServiceStatusIfNeeded;
- (void)updateServiceStatusDisplay;
- (void)updateTokenWindowDisplay:(CodexStatusSnapshot *)snapshot;
- (void)updateQuotaDetailsDisplay:(CodexStatusSnapshot *)snapshot;
- (void)evaluateWeeklyConsumptionAlert:(CodexStatusSnapshot *)snapshot;
- (void)requestWeeklyNotificationPermissionIfNeeded;
- (void)applyPositionLock;
- (BOOL)systemDataNeeded;
- (void)performUpdateCheckManual:(BOOL)manual;
- (void)presentUpdatePrompt:(HUDReleaseInfo *)release;
- (void)installRelease:(HUDReleaseInfo *)release;
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
    NSMenuItem *taskCenter = [appMenu addItemWithTitle:@"打开任务中心" action:@selector(openTaskCenter:) keyEquivalent:@""];
    taskCenter.target = self;
    NSMenuItem *hide = [appMenu addItemWithTitle:@"隐藏 Codex Monitor HUD" action:@selector(hide:) keyEquivalent:@"h"];
    hide.target = NSApp;
    NSMenuItem *checkUpdates = [appMenu addItemWithTitle:@"检查更新…" action:@selector(checkForUpdatesManually:) keyEquivalent:@""];
    checkUpdates.target = self;
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
    self.showLocalCost = [d objectForKey:@"showLocalCost"] ? [d boolForKey:@"showLocalCost"] : YES;
    self.showTokenWindows = [d objectForKey:@"showTokenWindows"] ? [d boolForKey:@"showTokenWindows"] : YES;
    self.showQuotaForecast = [d objectForKey:@"showQuotaForecast"] ? [d boolForKey:@"showQuotaForecast"] : YES;
    self.showQuotaDetails = [d objectForKey:@"showQuotaDetails"] ? [d boolForKey:@"showQuotaDetails"] : NO;
    self.showPeakDailyTokens = [d objectForKey:@"showPeakDailyTokens"] ? [d boolForKey:@"showPeakDailyTokens"] : NO;
    self.weeklyConsumptionAlertEnabled = [d objectForKey:@"weeklyConsumptionAlertEnabled"] ? [d boolForKey:@"weeklyConsumptionAlertEnabled"] : NO;
    self.weeklyConsumptionAlertThreshold = [d objectForKey:@"weeklyConsumptionAlertThreshold"] ? [d doubleForKey:@"weeklyConsumptionAlertThreshold"] : 15.0;
    self.weeklyConsumptionAlertThreshold = MAX(1.0, MIN(100.0, self.weeklyConsumptionAlertThreshold));
    self.weeklyConsumptionAlertMode = [d stringForKey:@"weeklyConsumptionAlertMode"] ?: @"rolling24h";
    if (![@[@"rolling24h", @"naturalDay"] containsObject:self.weeklyConsumptionAlertMode]) self.weeklyConsumptionAlertMode = @"rolling24h";
    self.weeklyConsumptionSystemNotificationEnabled = [d objectForKey:@"weeklyConsumptionSystemNotificationEnabled"] ? [d boolForKey:@"weeklyConsumptionSystemNotificationEnabled"] : NO;
    self.showServiceStatus = [d objectForKey:@"showServiceStatus"] ? [d boolForKey:@"showServiceStatus"] : NO;
    self.showTaskActivity = [d objectForKey:@"showTaskActivity"] ? [d boolForKey:@"showTaskActivity"] : YES;
    self.showRecentTasks = [d objectForKey:@"showRecentTasks"] ? [d boolForKey:@"showRecentTasks"] : YES;
    self.showLongestTurn = [d objectForKey:@"showLongestTurn"] ? [d boolForKey:@"showLongestTurn"] : NO;
    self.showLongestStreak = [d objectForKey:@"showLongestStreak"] ? [d boolForKey:@"showLongestStreak"] : NO;
    self.showSystem = [d objectForKey:@"showSystem"] ? [d boolForKey:@"showSystem"] : YES;
    self.showAttribution = [d objectForKey:@"showAttribution"] ? [d boolForKey:@"showAttribution"] : YES;
    self.showTrend = [d objectForKey:@"showTrend"] ? [d boolForKey:@"showTrend"] : YES;
    self.showMemoryApps = [d objectForKey:@"showMemoryApps"] ? [d boolForKey:@"showMemoryApps"] : YES;
    self.homeShowFiveHour = [d objectForKey:@"homeShowFiveHour"] ? [d boolForKey:@"homeShowFiveHour"] : YES;
    self.homeShowWeekly = [d objectForKey:@"homeShowWeekly"] ? [d boolForKey:@"homeShowWeekly"] : YES;
    self.homeShowPlan = [d objectForKey:@"homeShowPlan"] ? [d boolForKey:@"homeShowPlan"] : YES;
    self.homeShowUsage = [d objectForKey:@"homeShowUsage"] ? [d boolForKey:@"homeShowUsage"] : YES;
    self.homeShowModelQuota = [d objectForKey:@"homeShowModelQuota"] ? [d boolForKey:@"homeShowModelQuota"] : NO;
    self.homeShowLocalCost = [d objectForKey:@"homeShowLocalCost"] ? [d boolForKey:@"homeShowLocalCost"] : NO;
    self.homeShowTokenWindows = [d objectForKey:@"homeShowTokenWindows"] ? [d boolForKey:@"homeShowTokenWindows"] : NO;
    self.homeShowQuotaForecast = [d objectForKey:@"homeShowQuotaForecast"] ? [d boolForKey:@"homeShowQuotaForecast"] : NO;
    self.homeShowQuotaDetails = [d objectForKey:@"homeShowQuotaDetails"] ? [d boolForKey:@"homeShowQuotaDetails"] : NO;
    self.homeShowPeakDailyTokens = [d objectForKey:@"homeShowPeakDailyTokens"] ? [d boolForKey:@"homeShowPeakDailyTokens"] : NO;
    self.homeShowServiceStatus = [d objectForKey:@"homeShowServiceStatus"] ? [d boolForKey:@"homeShowServiceStatus"] : NO;
    self.homeShowTaskActivity = [d objectForKey:@"homeShowTaskActivity"] ? [d boolForKey:@"homeShowTaskActivity"] : YES;
    self.homeShowRecentTasks = [d objectForKey:@"homeShowRecentTasks"] ? [d boolForKey:@"homeShowRecentTasks"] : NO;
    self.homeShowLongestTurn = [d objectForKey:@"homeShowLongestTurn"] ? [d boolForKey:@"homeShowLongestTurn"] : NO;
    self.homeShowLongestStreak = [d objectForKey:@"homeShowLongestStreak"] ? [d boolForKey:@"homeShowLongestStreak"] : NO;
    self.homeShowDiagnosis = [d objectForKey:@"homeShowDiagnosis"] ? [d boolForKey:@"homeShowDiagnosis"] : YES;
    self.homeShowSystem = [d objectForKey:@"homeShowSystem"] ? [d boolForKey:@"homeShowSystem"] : YES;
    self.homeShowAttribution = [d objectForKey:@"homeShowAttribution"] ? [d boolForKey:@"homeShowAttribution"] : YES;
    self.homeShowTrend = [d objectForKey:@"homeShowTrend"] ? [d boolForKey:@"homeShowTrend"] : YES;
    self.homeShowMemoryApps = [d objectForKey:@"homeShowMemoryApps"] ? [d boolForKey:@"homeShowMemoryApps"] : NO;
    if (![d boolForKey:@"codexBarCostMigrationV1"]) {
        self.showUsage = NO;
        self.homeShowUsage = NO;
        self.showLocalCost = YES;
        self.homeShowLocalCost = YES;
        [d setBool:NO forKey:@"showUsage"];
        [d setBool:NO forKey:@"homeShowUsage"];
        [d setBool:YES forKey:@"showLocalCost"];
        [d setBool:YES forKey:@"homeShowLocalCost"];
        [d setBool:YES forKey:@"codexBarCostMigrationV1"];
    }
    self.historyEnabled = [d objectForKey:@"historyEnabled"] ? [d boolForKey:@"historyEnabled"] : YES;
    self.alwaysOnTop = [d objectForKey:@"alwaysOnTop"] ? [d boolForKey:@"alwaysOnTop"] : YES;
    self.positionLocked = [d objectForKey:@"positionLocked"] ? [d boolForKey:@"positionLocked"] : NO;
    self.backgroundOpacity = [d objectForKey:@"opacity"] ? [d doubleForKey:@"opacity"] : 0.82;
    self.backgroundOpacity = MAX(0.55, MIN(1.0, self.backgroundOpacity));
    double savedScale = [d objectForKey:@"windowScale"] ? [d doubleForKey:@"windowScale"] : 1.0;
    self.windowScale = MAX(HUDMinimumWindowScale, savedScale);
    self.refreshInterval = [d objectForKey:@"refreshInterval"] ? [d doubleForKey:@"refreshInterval"] : 5.0;
    self.refreshInterval = MAX(5.0, MIN(20.0, self.refreshInterval));
    self.currentPage = MAX(0, MIN(2, [d integerForKey:@"currentPage"]));
    self.accentName = [d stringForKey:@"accentName"] ?: @"green";
    self.homeCodexModuleOrder = HUDSanitizedOrder([d arrayForKey:@"homeCodexModuleOrder"], HUDDefaultHomeCodexOrder());
    self.homeComputerModuleOrder = HUDSanitizedOrder([d arrayForKey:@"homeComputerModuleOrder"], HUDDefaultHomeComputerOrder());
    self.settingsOrderControllers = [NSMutableArray array];
    self.sampler = [NativeSampler new];
    self.updateManager = [HUDUpdateManager new];
    self.minuteSamples = [NSMutableArray array];
    self.cpuHistory = [NSMutableArray array];
    NSString *historyPath = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/CodexSystemMonitor/native-history"];
    self.historyDirectory = [NSURL fileURLWithPath:historyPath isDirectory:YES];
    [[NSFileManager defaultManager] createDirectoryAtURL:self.historyDirectory withIntermediateDirectories:YES attributes:nil error:nil];
    self.instanceID = NSUUID.UUID.UUIDString;
    [self appendLifecycleEvent:[self launchLifecycleEvent]];
    [self createPanel];
    [self configureSamplingAndTimers];
    [self startCodexProviderIfNeeded];
    [self startServiceStatusIfNeeded];
    [self performSelector:@selector(checkForUpdatesAutomatically) withObject:nil afterDelay:4.0];
    self.updateTimer = [NSTimer scheduledTimerWithTimeInterval:HUDAutomaticUpdateCheckInterval target:self selector:@selector(checkForUpdatesAutomatically) userInfo:nil repeats:YES];
    self.updateTimer.tolerance = 2.0 * 60.0 * 60.0;
    [[NSRunLoop mainRunLoop] addTimer:self.updateTimer forMode:NSRunLoopCommonModes];
    [[[NSWorkspace sharedWorkspace] notificationCenter] addObserver:self selector:@selector(workspaceDidWake:) name:NSWorkspaceDidWakeNotification object:nil];
}

- (void)applicationWillTerminate:(NSNotification *)notification {
    [self appendLifecycleEvent:@"terminate"];
    [self savePosition];
    [self.systemTimer invalidate];
    [self.codexTimer invalidate];
    [self.serviceStatusTimer invalidate];
    [self.updateTimer invalidate];
    [self.codexProvider stop];
    [self.serviceStatusProvider stop];
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

- (NSURL *)installedTaskCenterURL {
    NSArray<NSString *> *fallbackPaths = @[
        [NSHomeDirectory() stringByAppendingPathComponent:@"Applications/Codex Monitor Task Center.app"],
        @"/Applications/Codex Monitor Task Center.app"
    ];
    for (NSString *path in fallbackPaths) {
        NSBundle *bundle = [NSBundle bundleWithPath:path];
        if ([bundle.bundleIdentifier isEqualToString:HUDTaskCenterBundleIdentifier]) {
            return [NSURL fileURLWithPath:path isDirectory:YES];
        }
    }
    NSURL *registeredURL = [NSWorkspace.sharedWorkspace URLForApplicationWithBundleIdentifier:HUDTaskCenterBundleIdentifier];
    if (registeredURL && [NSFileManager.defaultManager fileExistsAtPath:registeredURL.path]) return registeredURL;
    return nil;
}

- (void)presentTaskCenterLaunchFailure:(NSString *)detail {
    NSAlert *alert = [NSAlert new];
    alert.messageText = @"暂时无法打开任务中心";
    alert.informativeText = detail.length > 0 ? detail : @"尚未找到已安装的 Codex Monitor 任务中心。悬浮窗会继续正常运行。";
    [alert addButtonWithTitle:@"打开下载页"];
    [alert addButtonWithTitle:@"取消"];
    void (^handleResponse)(NSModalResponse) = ^(NSModalResponse response) {
        if (response == NSAlertFirstButtonReturn) {
            [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:HUDTaskCenterReleasePage]];
        }
    };
    if (self.panel.visible && !self.panel.miniaturized) {
        [alert beginSheetModalForWindow:self.panel completionHandler:handleResponse];
    } else {
        [NSApp activateIgnoringOtherApps:YES];
        handleResponse([alert runModal]);
    }
}

- (void)openTaskCenter:(id)sender {
    NSURL *applicationURL = [self installedTaskCenterURL];
    if (!applicationURL) {
        [self presentTaskCenterLaunchFailure:nil];
        return;
    }

    NSWorkspaceOpenConfiguration *configuration = [NSWorkspaceOpenConfiguration configuration];
    configuration.activates = YES;
    __weak typeof(self) weakSelf = self;
    [NSWorkspace.sharedWorkspace openApplicationAtURL:applicationURL configuration:configuration completionHandler:^(__unused NSRunningApplication *application, NSError *error) {
        if (!error) return;
        dispatch_async(dispatch_get_main_queue(), ^{
            NSString *message = [NSString stringWithFormat:@"任务中心已找到，但启动失败：%@", error.localizedDescription ?: @"未知错误"];
            [weakSelf presentTaskCenterLaunchFailure:message];
        });
    }];
}

- (NSColor *)accentColor {
    if ([self.accentName isEqualToString:@"blue"]) return NSColor.systemBlueColor;
    if ([self.accentName isEqualToString:@"purple"]) return NSColor.systemPurpleColor;
    if ([self.accentName isEqualToString:@"orange"]) return NSColor.systemOrangeColor;
    return NSColor.systemGreenColor;
}

- (CGFloat)homePanelHeight {
    CGFloat height = 52;
    BOOL hasCodex = self.homeShowTaskActivity || self.homeShowRecentTasks || self.homeShowFiveHour || self.homeShowWeekly || self.homeShowPlan || self.homeShowUsage || self.homeShowModelQuota || self.homeShowTokenWindows || self.homeShowLocalCost || self.homeShowQuotaForecast || self.homeShowServiceStatus || self.homeShowQuotaDetails || self.homeShowLongestTurn || self.homeShowLongestStreak || self.homeShowPeakDailyTokens;
    BOOL hasComputer = self.homeShowDiagnosis || self.homeShowSystem || self.homeShowAttribution || self.homeShowMemoryApps || self.homeShowTrend;
    if (hasCodex) height += 26;
    if (self.homeShowTaskActivity) height += 66;
    if (self.homeShowRecentTasks) height += 94;
    if (self.homeShowFiveHour || self.homeShowWeekly) height += 82;
    if (self.homeShowPlan || self.homeShowUsage || self.homeShowModelQuota) height += 66;
    if (self.homeShowTokenWindows) height += 66;
    if (self.homeShowLocalCost) height += 66;
    if (self.homeShowQuotaForecast) height += 66;
    if (self.homeShowServiceStatus) height += 66;
    if (self.homeShowQuotaDetails) height += MAX(66, 48 + self.codexProvider.snapshot.rateLimitBuckets.count * 14);
    if (self.homeShowLongestTurn || self.homeShowLongestStreak || self.homeShowPeakDailyTokens) height += 66;
    if (hasComputer) height += 26;
    if (self.homeShowDiagnosis || self.homeShowSystem) height += 66;
    if (self.homeShowAttribution) height += 25;
    if (self.homeShowMemoryApps) height += 118;
    if (self.homeShowTrend) height += 25;
    height += 22;
    return MAX(170, height + (self.compact ? 0 : 110));
}

- (NSSize)basePanelSize {
    if (self.collapsed) return NSMakeSize(430, 54);
    BOOL hasQuota = self.showFiveHourQuota || self.showWeeklyQuota || self.showModelQuota;
    BOOL hasInsights = self.showPlan || self.showUsage || self.showModelQuota;
    CGFloat codexHeight = 260 + (self.showTaskActivity ? 66 : 0) + (self.showRecentTasks ? 94 : 0) + ((self.showLongestTurn || self.showLongestStreak || self.showPeakDailyTokens) ? 66 : 0);
    if (!hasQuota) codexHeight -= 82;
    if (!hasInsights) codexHeight -= 66;
    if (self.showTokenWindows) codexHeight += 66;
    if (self.showLocalCost) codexHeight += 66;
    if (self.showQuotaForecast) codexHeight += 66;
    if (self.showServiceStatus) codexHeight += 66;
    if (self.showQuotaDetails) codexHeight += MAX(66, 48 + self.codexProvider.snapshot.rateLimitBuckets.count * 14);
    codexHeight = MAX(170, codexHeight);
    CGFloat height = self.currentPage == 0 ? [self homePanelHeight] : (self.currentPage == 1 ? codexHeight : (self.showMemoryApps ? 421 : 303));
    if (!self.compact && self.currentPage != 0) height += 110;
    return NSMakeSize(430, height);
}

- (NSSize)panelSize {
    NSSize base = [self basePanelSize];
    self.windowScale = MIN(MAX(HUDMinimumWindowScale, self.windowScale), [self maximumWindowScaleForBaseSize:base]);
    return HUDContentSizeForUniformScale(base, self.windowScale);
}

- (CGFloat)maximumWindowScaleForBaseSize:(NSSize)baseSize {
    NSScreen *screen = self.panel.screen ?: NSScreen.mainScreen;
    if (!screen || baseSize.width <= 0 || baseSize.height <= 0) return MAX(HUDMinimumWindowScale, self.windowScale);
    NSRect visible = NSInsetRect(screen.visibleFrame, HUDScreenEdgeMargin / 2.0, HUDScreenEdgeMargin / 2.0);
    CGFloat widthScale = visible.size.width / baseSize.width;
    CGFloat heightScale = visible.size.height / baseSize.height;
    return MAX(HUDMinimumWindowScale, MIN(widthScale, heightScale));
}

- (NSSize)frameSizeForContentSize:(NSSize)contentSize {
    if (!self.panel) return contentSize;
    NSRect contentRect = NSMakeRect(0, 0, contentSize.width, contentSize.height);
    return [self.panel frameRectForContentRect:contentRect].size;
}

- (void)configureResizeLimitsForBaseSize:(NSSize)baseSize contentSize:(NSSize)contentSize {
    self.panel.contentAspectRatio = baseSize;
    if (self.collapsed || self.positionLocked) {
        self.panel.contentMinSize = contentSize;
        self.panel.contentMaxSize = contentSize;
        return;
    }
    self.panel.contentMinSize = HUDContentSizeForUniformScale(baseSize, HUDMinimumWindowScale);
    self.panel.contentMaxSize = HUDContentSizeForUniformScale(baseSize, [self maximumWindowScaleForBaseSize:baseSize]);
}

- (void)updateHUDGeometryForContentSize:(NSSize)contentSize baseSize:(NSSize)baseSize {
    if (!self.hudView || contentSize.width <= 0 || contentSize.height <= 0) return;
    self.windowScale = HUDScaleForContentSize(contentSize, baseSize);
    self.hudView.bounds = NSMakeRect(0, 0, baseSize.width, baseSize.height);
    self.hudView.layoutCanvas.frame = NSMakeRect(0, 0, baseSize.width, baseSize.height);
    [self.hudView.layoutCanvas setNeedsLayout:YES];
    [self.hudView.layoutCanvas layoutSubtreeIfNeeded];
}

- (NSWindowStyleMask)panelStyleMask {
    return NSWindowStyleMaskTitled | NSWindowStyleMaskFullSizeContentView | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable | NSWindowStyleMaskNonactivatingPanel;
}

- (void)createPanel {
    NSSize size = [self panelSize];
    NSSize baseSize = [self basePanelSize];
    self.panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, size.width, size.height)
                                            styleMask:[self panelStyleMask]
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
    [self configureResizeLimitsForBaseSize:baseSize contentSize:size];

    self.hudView = [[HUDView alloc] initWithFrame:NSMakeRect(0, 0, baseSize.width, baseSize.height)];
    self.hudView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    self.hudView.accentColor = [self accentColor];
    self.hudView.codexStatusLabel.textColor = self.hudView.accentColor;
    [self.hudView setBackgroundOpacity:self.backgroundOpacity];
    [self.hudView setAlwaysOnTop:self.alwaysOnTop];
    [self.hudView setPositionLocked:self.positionLocked];
    [self.hudView setCompact:self.compact];
    [self.hudView setFiveHourQuotaVisible:self.showFiveHourQuota];
    [self.hudView setWeeklyQuotaVisible:self.showWeeklyQuota];
    [self.hudView setPlanVisible:self.showPlan];
    [self.hudView setUsageVisible:self.showUsage];
    [self.hudView setModelQuotaVisible:NO];
    [self.hudView setLocalCostVisible:self.showLocalCost];
    [self.hudView setTokenWindowsVisible:self.showTokenWindows];
    [self.hudView setQuotaForecastVisible:self.showQuotaForecast];
    [self.hudView setServiceStatusVisible:self.showServiceStatus];
    [self.hudView setQuotaDetailsVisible:self.showQuotaDetails];
    [self.hudView setTaskActivityVisible:self.showTaskActivity];
    [self.hudView setRecentTasksVisible:self.showRecentTasks];
    [self.hudView setLongestTurnVisible:self.showLongestTurn];
    [self.hudView setLongestStreakVisible:self.showLongestStreak];
    [self.hudView setPeakDailyTokensVisible:self.showPeakDailyTokens];
    [self.hudView setSystemVisible:self.showSystem];
    [self.hudView setAttributionVisible:self.showAttribution];
    [self.hudView setTrendVisible:self.showTrend];
    [self.hudView setMemoryAppsVisible:self.showMemoryApps];
    [self.hudView setHomeFiveHourVisible:self.homeShowFiveHour];
    [self.hudView setHomeWeeklyVisible:self.homeShowWeekly];
    [self.hudView setHomePlanVisible:self.homeShowPlan];
    [self.hudView setHomeUsageVisible:self.homeShowUsage];
    [self.hudView setHomeModelQuotaVisible:NO];
    [self.hudView setHomeLocalCostVisible:self.homeShowLocalCost];
    [self.hudView setHomeTokenWindowsVisible:self.homeShowTokenWindows];
    [self.hudView setHomeQuotaForecastVisible:self.homeShowQuotaForecast];
    [self.hudView setHomeServiceStatusVisible:self.homeShowServiceStatus];
    [self.hudView setHomeQuotaDetailsVisible:self.homeShowQuotaDetails];
    [self.hudView setHomeTaskActivityVisible:self.homeShowTaskActivity];
    [self.hudView setHomeRecentTasksVisible:self.homeShowRecentTasks];
    [self.hudView setHomeLongestTurnVisible:self.homeShowLongestTurn];
    [self.hudView setHomeLongestStreakVisible:self.homeShowLongestStreak];
    [self.hudView setHomePeakDailyTokensVisible:self.homeShowPeakDailyTokens];
    [self.hudView setHomeDiagnosisVisible:self.homeShowDiagnosis];
    [self.hudView setHomeSystemVisible:self.homeShowSystem];
    [self.hudView setHomeAttributionVisible:self.homeShowAttribution];
    [self.hudView setHomeTrendVisible:self.homeShowTrend];
    [self.hudView setHomeMemoryAppsVisible:self.homeShowMemoryApps];
    [self.hudView applyHomeCodexOrder:self.homeCodexModuleOrder computerOrder:self.homeComputerModuleOrder];
    [self.hudView setPage:self.currentPage];
    __weak typeof(self) weakSelf = self;
    self.hudView.menuProvider = ^NSMenu *{ return [weakSelf settingsMenu]; };
    self.hudView.settingsRequested = ^{ [weakSelf showSettingsWindow:nil]; };
    self.hudView.taskCenterRequested = ^{ [weakSelf openTaskCenter:nil]; };
    self.hudView.pageChanged = ^(NSInteger page) {
        weakSelf.currentPage = page;
        [NSUserDefaults.standardUserDefaults setInteger:page forKey:@"currentPage"];
        [weakSelf updateDetailLabels];
        [weakSelf resizePanel];
        [weakSelf configureSamplingAndTimers];
    };
    self.hudView.topmostChanged = ^(BOOL enabled) {
        weakSelf.alwaysOnTop = enabled;
        [NSUserDefaults.standardUserDefaults setBool:enabled forKey:@"alwaysOnTop"];
        weakSelf.panel.level = enabled ? NSFloatingWindowLevel : NSNormalWindowLevel;
        if (enabled) [weakSelf.panel orderFrontRegardless];
    };
    self.hudView.positionLockChanged = ^(BOOL enabled) {
        weakSelf.positionLocked = enabled;
        [NSUserDefaults.standardUserDefaults setBool:enabled forKey:@"positionLocked"];
        [weakSelf applyPositionLock];
    };
    self.hudView.minimizeRequested = ^{
        [weakSelf.panel miniaturize:nil];
    };
    self.panel.contentView = self.hudView;
    [self updateHUDGeometryForContentSize:self.hudView.frame.size baseSize:baseSize];
    if (![self restorePosition]) [self moveToCorner:@"bottomLeft"];
    [self applyPositionLock];
    [self.panel orderFrontRegardless];
}

- (BOOL)systemDataNeeded {
    BOOL homeNeedsComputer = self.homeShowDiagnosis || self.homeShowSystem || self.homeShowAttribution || self.homeShowTrend || self.homeShowMemoryApps;
    return self.historyEnabled || self.currentPage == 2 || (self.currentPage == 0 && homeNeedsComputer);
}

- (void)configureSamplingAndTimers {
    BOOL wasRunning = self.systemTimer.valid;
    BOOL computerPage = self.currentPage == 2;
    BOOL homePage = self.currentPage == 0;
    self.sampler.collectTopApps = (computerPage && (self.showMemoryApps || (!self.compact && self.showSystem))) || (homePage && self.homeShowMemoryApps);
    self.sampler.collectSecondaryMetrics = self.historyEnabled || (computerPage && (self.showSystem || !self.compact)) || (homePage && self.homeShowSystem);
    self.sampler.collectThermalMetrics = self.historyEnabled || computerPage || (homePage && (self.homeShowDiagnosis || self.homeShowSystem));
    [self startSystemTimer];
    if (!wasRunning && [self systemDataNeeded] && self.hudView) [self updateDisplay];
}

- (void)applyPositionLock {
    if (!self.panel) return;
    self.panel.movableByWindowBackground = !self.positionLocked;
    NSWindowStyleMask mask = self.panel.styleMask;
    if (self.positionLocked) mask &= ~NSWindowStyleMaskResizable;
    else mask |= NSWindowStyleMaskResizable;
    self.panel.styleMask = mask;
    [self.hudView setPositionLocked:self.positionLocked];
    [self configureResizeLimitsForBaseSize:[self basePanelSize] contentSize:self.panel.contentView.frame.size];
}

- (void)startSystemTimer {
    [self.systemTimer invalidate];
    self.systemTimer = nil;
    if (![self systemDataNeeded]) return;
    self.systemTimer = [NSTimer scheduledTimerWithTimeInterval:self.refreshInterval target:self selector:@selector(updateDisplay) userInfo:nil repeats:YES];
    self.systemTimer.tolerance = MIN(0.5, self.refreshInterval * 0.1);
    [[NSRunLoop mainRunLoop] addTimer:self.systemTimer forMode:NSRunLoopCommonModes];
}

- (void)startCodexProviderIfNeeded {
    BOOL needsActivity = self.showTaskActivity || self.homeShowTaskActivity;
    BOOL needsCostHistory = self.showLocalCost || self.homeShowLocalCost || self.showTokenWindows || self.homeShowTokenWindows;
    BOOL needsForecast = self.showQuotaForecast || self.homeShowQuotaForecast || self.weeklyConsumptionAlertEnabled;
    BOOL needsAccountData = needsForecast || self.showTokenWindows || self.homeShowTokenWindows || self.showQuotaDetails || self.homeShowQuotaDetails || self.showFiveHourQuota || self.showWeeklyQuota || self.showPlan || self.showUsage || self.showModelQuota || self.showRecentTasks || self.showLongestTurn || self.showLongestStreak || self.showPeakDailyTokens || self.homeShowFiveHour || self.homeShowWeekly || self.homeShowPlan || self.homeShowUsage || self.homeShowModelQuota || self.homeShowRecentTasks || self.homeShowLongestTurn || self.homeShowLongestStreak || self.homeShowPeakDailyTokens;
    BOOL needsCodexData = needsActivity || needsAccountData || needsCostHistory;
    if (!needsCodexData) { [self.codexProvider stop]; self.codexProvider = nil; [self.codexTimer invalidate]; self.codexTimer = nil; return; }
    if (!self.codexProvider) {
        self.codexProvider = [CodexStatusProvider new];
        __weak typeof(self) weakSelf = self;
        self.codexProvider.updateHandler = ^{ [weakSelf updateCodexDisplay]; [weakSelf scheduleCodexRefreshTimer]; };
        self.lastCodexAccountFetchAt = 0;
        self.lastActivityRefreshRequestAt = 0;
        self.lastCostHistoryFetchAt = 0;
    }
    self.codexProvider.costHistoryEnabled = needsCostHistory;
    self.codexProvider.quotaForecastEnabled = needsForecast;
    self.codexProvider.accountDataEnabled = needsAccountData || needsActivity;
    NSTimeInterval now = NSDate.date.timeIntervalSince1970;
    if (needsCostHistory && self.lastCostHistoryFetchAt <= 0) { [self.codexProvider refreshCostHistory]; self.lastCostHistoryFetchAt = now; }
    if ((needsAccountData || needsActivity) && self.lastCodexAccountFetchAt <= 0) { [self.codexProvider start]; self.lastCodexAccountFetchAt = now; }
    if (needsActivity && self.lastActivityRefreshRequestAt <= 0) { [self.codexProvider refreshActivity]; self.lastActivityRefreshRequestAt = now; }
    [self scheduleCodexRefreshTimer];
}

- (NSTimeInterval)codexAccountRefreshInterval {
    CodexStatusSnapshot *snapshot = self.codexProvider.snapshot;
    BOOL hasError = snapshot.quotaErrorText.length > 0 || snapshot.accountErrorText.length > 0 || snapshot.usageErrorText.length > 0 || snapshot.recentTasksErrorText.length > 0;
    BOOL quotaLow = (snapshot.fiveHourAvailable && snapshot.fiveHourRemainingPercent <= 15) || (snapshot.weeklyAvailable && snapshot.weeklyRemainingPercent <= 15);
    if (hasError && self.codexProvider.recommendedRetryInterval > 0) return self.codexProvider.recommendedRetryInterval;
    return hasError || (quotaLow && snapshot.activeTaskCount > 0) ? 60.0 : 300.0;
}

- (void)scheduleCodexRefreshTimer {
    [self.codexTimer invalidate]; self.codexTimer = nil;
    if (!self.codexProvider) return;
    BOOL needsActivity = self.showTaskActivity || self.homeShowTaskActivity;
    BOOL needsCostHistory = self.showLocalCost || self.homeShowLocalCost || self.showTokenWindows || self.homeShowTokenWindows;
    BOOL needsAccountData = self.weeklyConsumptionAlertEnabled || self.showTokenWindows || self.homeShowTokenWindows || self.showQuotaDetails || self.homeShowQuotaDetails || self.showQuotaForecast || self.homeShowQuotaForecast || self.showFiveHourQuota || self.showWeeklyQuota || self.showPlan || self.showUsage || self.showModelQuota || self.showRecentTasks || self.showLongestTurn || self.showLongestStreak || self.showPeakDailyTokens || self.homeShowFiveHour || self.homeShowWeekly || self.homeShowPlan || self.homeShowUsage || self.homeShowModelQuota || self.homeShowRecentTasks || self.homeShowLongestTurn || self.homeShowLongestStreak || self.homeShowPeakDailyTokens;
    NSTimeInterval now = NSDate.date.timeIntervalSince1970;
    NSTimeInterval next = DBL_MAX;
    if (needsActivity) {
        NSTimeInterval activityInterval = HUDCodexActivityRefreshInterval(self.codexProvider.snapshot.activeTaskCount);
        next = MIN(next, MAX(now + 1.0, self.lastActivityRefreshRequestAt + activityInterval));
    }
    if (needsCostHistory) next = MIN(next, MAX(now + 1.0, self.lastCostHistoryFetchAt + 300.0));
    if (needsAccountData || needsActivity) next = MIN(next, MAX(now + 1.0, self.lastCodexAccountFetchAt + [self codexAccountRefreshInterval]));
    if (next == DBL_MAX) return;
    NSTimeInterval delay = MAX(1.0, next - now);
    self.codexTimer = [NSTimer scheduledTimerWithTimeInterval:delay target:self selector:@selector(refreshCodexData) userInfo:nil repeats:NO];
    self.codexTimer.tolerance = MIN(2.0, delay * 0.1);
    [[NSRunLoop mainRunLoop] addTimer:self.codexTimer forMode:NSRunLoopCommonModes];
}

- (void)refreshCodexData {
    if (!self.codexProvider) return;
    NSTimeInterval now = NSDate.date.timeIntervalSince1970;
    BOOL needsActivity = self.showTaskActivity || self.homeShowTaskActivity;
    BOOL needsCostHistory = self.showLocalCost || self.homeShowLocalCost || self.showTokenWindows || self.homeShowTokenWindows;
    BOOL needsAccountData = self.weeklyConsumptionAlertEnabled || self.showTokenWindows || self.homeShowTokenWindows || self.showQuotaDetails || self.homeShowQuotaDetails || self.showQuotaForecast || self.homeShowQuotaForecast || self.showFiveHourQuota || self.showWeeklyQuota || self.showPlan || self.showUsage || self.showModelQuota || self.showRecentTasks || self.showLongestTurn || self.showLongestStreak || self.showPeakDailyTokens || self.homeShowFiveHour || self.homeShowWeekly || self.homeShowPlan || self.homeShowUsage || self.homeShowModelQuota || self.homeShowRecentTasks || self.homeShowLongestTurn || self.homeShowLongestStreak || self.homeShowPeakDailyTokens;
    NSTimeInterval activityInterval = HUDCodexActivityRefreshInterval(self.codexProvider.snapshot.activeTaskCount);
    if (needsActivity && now - self.lastActivityRefreshRequestAt >= activityInterval - 0.5) {
        [self.codexProvider refreshActivity]; self.lastActivityRefreshRequestAt = now;
    }
    if (needsCostHistory && now - self.lastCostHistoryFetchAt >= 299.5) {
        [self.codexProvider refreshCostHistory]; self.lastCostHistoryFetchAt = now;
    }
    if ((needsAccountData || needsActivity) && now - self.lastCodexAccountFetchAt >= [self codexAccountRefreshInterval] - 0.5) {
        [self.codexProvider refreshQuotaInBackground]; self.lastCodexAccountFetchAt = now;
    }
    [self scheduleCodexRefreshTimer];
}

- (NSTimeInterval)serviceStatusRefreshInterval {
    HUDOpenAIServiceStatusSnapshot *snapshot = self.serviceStatusProvider.snapshot;
    BOOL degraded = snapshot.errorText.length > 0 || (snapshot.overallIndicator.length > 0 && ![snapshot.overallIndicator isEqualToString:@"none"]) || (snapshot.codexComponentStatus.length > 0 && ![snapshot.codexComponentStatus isEqualToString:@"operational"]);
    return degraded ? 120.0 : 600.0;
}

- (void)scheduleServiceStatusTimer {
    [self.serviceStatusTimer invalidate];
    self.serviceStatusTimer = nil;
    if (!self.serviceStatusProvider) return;
    NSTimeInterval now = NSDate.date.timeIntervalSince1970;
    NSTimeInterval delay = MAX(1.0, self.lastServiceStatusFetchAt + [self serviceStatusRefreshInterval] - now);
    self.serviceStatusTimer = [NSTimer scheduledTimerWithTimeInterval:delay target:self selector:@selector(refreshServiceStatus) userInfo:nil repeats:NO];
    self.serviceStatusTimer.tolerance = MIN(30.0, delay * 0.1);
    [[NSRunLoop mainRunLoop] addTimer:self.serviceStatusTimer forMode:NSRunLoopCommonModes];
}

- (void)refreshServiceStatus {
    if (!self.serviceStatusProvider) return;
    self.lastServiceStatusFetchAt = NSDate.date.timeIntervalSince1970;
    [self.serviceStatusProvider refresh];
    [self scheduleServiceStatusTimer];
}

- (void)startServiceStatusIfNeeded {
    BOOL needed = self.showServiceStatus || self.homeShowServiceStatus;
    if (!needed) {
        [self.serviceStatusTimer invalidate]; self.serviceStatusTimer = nil;
        [self.serviceStatusProvider stop]; self.serviceStatusProvider = nil;
        self.lastServiceStatusFetchAt = 0;
        return;
    }
    if (!self.serviceStatusProvider) {
        self.serviceStatusProvider = [HUDOpenAIServiceStatusProvider new];
        __weak typeof(self) weakSelf = self;
        self.serviceStatusProvider.updateHandler = ^{ [weakSelf updateServiceStatusDisplay]; [weakSelf scheduleServiceStatusTimer]; };
        self.lastServiceStatusFetchAt = 0;
    }
    if (self.lastServiceStatusFetchAt <= 0) [self refreshServiceStatus];
    else [self scheduleServiceStatusTimer];
}

- (void)updateServiceStatusDisplay {
    HUDOpenAIServiceStatusSnapshot *snapshot = self.serviceStatusProvider.snapshot;
    if (!snapshot || !self.hudView) return;
    NSString *value = snapshot.available ? snapshot.headline : (snapshot.errorText ?: @"正在检查官方状态");
    NSString *detail = snapshot.available ? [NSString stringWithFormat:@"%@ · %@", snapshot.detail ?: @"OpenAI整体状态未知", FormatModuleState(snapshot.updatedAt, snapshot.errorText, 900)] : @"未读取账号信息 · 稍后重试";
    BOOL severe = [snapshot.overallIndicator isEqualToString:@"major"] || [snapshot.overallIndicator isEqualToString:@"critical"] || [snapshot.codexComponentStatus isEqualToString:@"major_outage"];
    BOOL degraded = snapshot.errorText.length > 0 || ![snapshot.overallIndicator isEqualToString:@"none"] || (snapshot.codexComponentStatus.length > 0 && ![snapshot.codexComponentStatus isEqualToString:@"operational"]);
    NSColor *color = severe ? NSColor.systemRedColor : (degraded ? NSColor.systemOrangeColor : [self accentColor]);
    self.hudView.serviceStatusCard.valueLabel.stringValue = value;
    self.hudView.serviceStatusCard.subtitleLabel.stringValue = detail;
    self.hudView.serviceStatusCard.valueLabel.textColor = color;
    self.hudView.homeServiceStatusCard.valueLabel.stringValue = value;
    self.hudView.homeServiceStatusCard.subtitleLabel.stringValue = detail;
    self.hudView.homeServiceStatusCard.valueLabel.textColor = color;
    [self updateDetailLabels];
}

- (BOOL)restorePosition {
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    if ([defaults objectForKey:@"originX"] == nil || [defaults objectForKey:@"originY"] == nil) return NO;
    NSSize frameSize = [self frameSizeForContentSize:[self panelSize]];
    NSRect proposed = NSMakeRect([defaults doubleForKey:@"originX"], [defaults doubleForKey:@"originY"], frameSize.width, frameSize.height);
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

- (NSSize)windowWillResize:(NSWindow *)sender toSize:(NSSize)frameSize {
    if (sender != self.panel) return frameSize;
    if (self.collapsed || self.positionLocked) return self.panel.frame.size;
    NSRect proposedFrame = NSMakeRect(0, 0, frameSize.width, frameSize.height);
    NSSize proposedContent = [sender contentRectForFrameRect:proposedFrame].size;
    NSSize baseSize = [self basePanelSize];
    CGFloat scale = HUDUniformScaleForProposedContentSize(proposedContent, baseSize, self.windowScale);
    scale = MIN(scale, [self maximumWindowScaleForBaseSize:baseSize]);
    return [self frameSizeForContentSize:HUDContentSizeForUniformScale(baseSize, scale)];
}

- (void)windowDidResize:(NSNotification *)notification {
    if (notification.object != self.panel) return;
    NSSize base = [self basePanelSize];
    NSSize content = self.hudView.frame.size;
    [self updateHUDGeometryForContentSize:content baseSize:base];
}

- (void)windowDidEndLiveResize:(NSNotification *)notification {
    if (notification.object != self.panel) return;
    [self updateHUDGeometryForContentSize:self.hudView.frame.size baseSize:[self basePanelSize]];
    [NSUserDefaults.standardUserDefaults setDouble:self.windowScale forKey:@"windowScale"];
    [self savePosition];
}

- (void)workspaceDidWake:(NSNotification *)notification {
    [self appendLifecycleEvent:@"wake"];
    self.sampler = [NativeSampler new];
    [self.minuteSamples removeAllObjects];
    [self.cpuHistory removeAllObjects];
    self.minuteStart = 0;
    [self configureSamplingAndTimers];
    self.lastCodexAccountFetchAt = 0;
    self.lastActivityRefreshRequestAt = 0;
    self.lastCostHistoryFetchAt = 0;
    self.lastServiceStatusFetchAt = 0;
    [self startCodexProviderIfNeeded];
    [self startServiceStatusIfNeeded];
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
    self.hudView.cpuCard.subtitleLabel.stringValue = @"整机总算力占用";
    self.hudView.memoryPressureCard.valueLabel.stringValue = [NSString stringWithFormat:@"%.1f / %.0fG", snapshot.systemMemoryUsedGiB, snapshot.totalMemoryGiB];
    self.hudView.memoryPressureCard.subtitleLabel.stringValue = [NSString stringWithFormat:@"已用 %.0f%% · 压力 %@", snapshot.systemMemoryUsedPercent, snapshot.memoryPressureText ?: @"未知"];
    self.hudView.cpuCard.valueLabel.textColor = snapshot.systemCPUPercent >= 75 ? NSColor.systemOrangeColor : [self accentColor];
    self.hudView.memoryPressureCard.valueLabel.textColor = snapshot.memoryPressureLevel > 0 ? NSColor.systemOrangeColor : [self accentColor];
    self.hudView.homeSystemCard.valueLabel.stringValue = [NSString stringWithFormat:@"CPU %.0f%% · 内存 %.0f%%", snapshot.systemCPUPercent, snapshot.systemMemoryUsedPercent];
    self.hudView.homeSystemCard.subtitleLabel.stringValue = [NSString stringWithFormat:@"已用 %.1f / %.0fG · 压力 %@", snapshot.systemMemoryUsedGiB, snapshot.totalMemoryGiB, snapshot.memoryPressureText ?: @"未知"];
    self.hudView.homeSystemCard.valueLabel.textColor = snapshot.systemCPUPercent >= 75 ? NSColor.systemOrangeColor : [self accentColor];
    self.hudView.attributionLabel.stringValue = [NSString stringWithFormat:@"Codex：CPU %.1f%%（整机） · 内存 %.1fG（占总内存 %.0f%%）", snapshot.codexCPUPercent, snapshot.codexMemoryGiB, snapshot.codexMemoryPercent];
    self.hudView.homeAttributionLabel.stringValue = self.hudView.attributionLabel.stringValue;
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

- (NSString *)visibleCodexStatus:(CodexStatusSnapshot *)s fiveHour:(BOOL)fiveHour weekly:(BOOL)weekly plan:(BOOL)plan usage:(BOOL)usage model:(BOOL)model activity:(BOOL)activity recent:(BOOL)recent {
    BOOL quotaVisible = fiveHour || weekly || model;
    if (quotaVisible && s.quotaErrorText.length > 0) return s.quotaErrorText;
    if (plan && s.accountErrorText.length > 0) return s.accountErrorText;
    if (usage && s.usageErrorText.length > 0) return s.usageErrorText;
    if (activity && s.activityErrorText.length > 0) return @"任务活动暂时无法可靠判断";
    if (recent && s.recentTasksErrorText.length > 0) return s.recentTasksErrorText;
    if (quotaVisible && s.ordinaryUsageAllowed && !s.ordinaryUsageAllowed.boolValue && !HUDTimestampIsStale(s.ordinaryUsageUpdatedAt, 900)) return @"官方：普通包含用量暂不可用";
    if (!s.quotaAvailable && !s.accountAvailable && !s.usageAvailable && !(activity && s.activityAvailable) && !(recent && s.recentTasksAvailable)) return s.statusText ?: @"正在连接本机Codex";
    if (fiveHour && !s.fiveHourAvailable) return @"5小时额度暂未返回";
    if (weekly && !s.weeklyAvailable) return @"每周额度暂未返回";
    if (plan && !s.accountAvailable) return @"订阅信息暂未返回";
    if (usage && !s.usageAvailable) return @"Token用量暂未返回";
    if (model && !s.modelQuotaAvailable) return @"模型专属额度暂未返回";
    if (activity && !s.activityAvailable) return @"任务活动暂未返回";
    if (recent && !s.recentTasksAvailable) return @"最近任务暂未返回";
    BOOL accountStale = (quotaVisible && HUDTimestampIsStale(s.quotaUpdatedAt, 900)) || (plan && HUDTimestampIsStale(s.accountUpdatedAt, 900)) || (usage && HUDTimestampIsStale(s.usageUpdatedAt, 900));
    BOOL activityStale = activity && HUDTimestampIsStale(s.activityUpdatedAt, 60);
    BOOL recentStale = recent && HUDTimestampIsStale(s.recentTasksUpdatedAt, 900);
    if (accountStale || activityStale || recentStale) return @"部分数据已过期，显示上次数据";
    return @"Codex数据正常";
}

- (NSString *)freshnessText:(CodexStatusSnapshot *)s fiveHour:(BOOL)fiveHour weekly:(BOOL)weekly plan:(BOOL)plan usage:(BOOL)usage model:(BOOL)model activity:(BOOL)activity recent:(BOOL)recent {
    NSMutableArray<NSString *> *parts = [NSMutableArray array];
    if (fiveHour || weekly || model) {
        NSString *name = fiveHour && weekly ? @"额度" : (fiveHour ? @"5小时" : (weekly ? @"每周" : @"模型额度"));
        [parts addObject:[NSString stringWithFormat:@"%@ %@", name, FormatModuleState(s.quotaUpdatedAt, s.quotaErrorText, 900)]];
    }
    if (plan) [parts addObject:[NSString stringWithFormat:@"订阅 %@", FormatModuleState(s.accountUpdatedAt, s.accountErrorText, 900)]];
    if (usage) [parts addObject:[NSString stringWithFormat:@"用量 %@", FormatModuleState(s.usageUpdatedAt, s.usageErrorText, 900)]];
    if (activity) [parts addObject:[NSString stringWithFormat:@"任务活动 %@", FormatModuleState(s.activityUpdatedAt, s.activityErrorText, 60)]];
    if (recent) [parts addObject:[NSString stringWithFormat:@"最近任务 %@", FormatModuleState(s.recentTasksUpdatedAt, s.recentTasksErrorText, 900)]];
    return parts.count ? [parts componentsJoinedByString:@" · "] : @"未启用Codex显示模块";
}

- (void)updateCodexDisplay {
    [self.hudView setServiceStatusVisible:self.showServiceStatus];
    [self.hudView setHomeServiceStatusVisible:self.homeShowServiceStatus];
    if (self.serviceStatusProvider) [self updateServiceStatusDisplay];
    CodexStatusSnapshot *s = self.codexProvider.snapshot;
    if (!s) { [self updateDetailLabels]; return; }
    NSString *plan = s.accountAvailable ? FormatPlan(s.planType) : nil;
    BOOL pageUsesUsageData = self.showUsage || self.showLongestTurn || self.showLongestStreak || self.showPeakDailyTokens;
    BOOL homeUsesUsageData = self.homeShowUsage || self.homeShowLongestTurn || self.homeShowLongestStreak || self.homeShowPeakDailyTokens;
    NSString *statusText = [self visibleCodexStatus:s fiveHour:self.showFiveHourQuota weekly:self.showWeeklyQuota plan:self.showPlan usage:pageUsesUsageData model:self.showModelQuota activity:self.showTaskActivity recent:self.showRecentTasks];
    BOOL pageHasOfficialData = self.showFiveHourQuota || self.showWeeklyQuota || self.showPlan || pageUsesUsageData || self.showModelQuota || self.showQuotaDetails || self.showTokenWindows || self.showTaskActivity || self.showRecentTasks || self.showQuotaForecast;
    if (self.showLocalCost && !pageHasOfficialData) statusText = s.localCostAvailable ? @"本机Token历史正常" : (s.localCostErrorText ?: @"正在读取本机Token历史");
    else if (self.showLocalCost && s.localCostErrorText.length > 0) statusText = @"本机Token历史读取失败";
    self.hudView.codexStatusLabel.stringValue = self.showPlan && plan ? [NSString stringWithFormat:@"● %@ · %@", plan, statusText] : [NSString stringWithFormat:@"● %@", statusText];
    NSString *homeStatusText = [self visibleCodexStatus:s fiveHour:self.homeShowFiveHour weekly:self.homeShowWeekly plan:self.homeShowPlan usage:homeUsesUsageData model:self.homeShowModelQuota activity:self.homeShowTaskActivity recent:self.homeShowRecentTasks];
    BOOL homeHasOfficialData = self.homeShowFiveHour || self.homeShowWeekly || self.homeShowPlan || homeUsesUsageData || self.homeShowModelQuota || self.homeShowQuotaDetails || self.homeShowTokenWindows || self.homeShowTaskActivity || self.homeShowRecentTasks || self.homeShowQuotaForecast;
    if (self.homeShowLocalCost && !homeHasOfficialData) homeStatusText = s.localCostAvailable ? @"本机Token历史正常" : (s.localCostErrorText ?: @"正在读取本机Token历史");
    else if (self.homeShowLocalCost && s.localCostErrorText.length > 0) homeStatusText = @"本机Token历史读取失败";
    self.hudView.homeCodexStatusLabel.stringValue = self.homeShowPlan && plan ? [NSString stringWithFormat:@"● %@ · %@", plan, homeStatusText] : [NSString stringWithFormat:@"● %@", homeStatusText];
    NSColor *fiveHourColor = HUDQuotaColor(s.fiveHourAvailable, s.fiveHourRemainingPercent, [self accentColor]);
    NSColor *weeklyColor = HUDQuotaColor(s.weeklyAvailable, s.weeklyRemainingPercent, [self accentColor]);
    [self.hudView.fiveHourCard showAvailable:s.fiveHourAvailable remaining:s.fiveHourRemainingPercent reset:FormatReset(s.fiveHourResetAt) accent:fiveHourColor];
    [self.hudView.weeklyCard showAvailable:s.weeklyAvailable remaining:s.weeklyRemainingPercent reset:FormatReset(s.weeklyResetAt) accent:weeklyColor];
    [self.hudView.homeFiveHourCard showAvailable:s.fiveHourAvailable remaining:s.fiveHourRemainingPercent reset:FormatReset(s.fiveHourResetAt) accent:fiveHourColor];
    [self.hudView.homeWeeklyCard showAvailable:s.weeklyAvailable remaining:s.weeklyRemainingPercent reset:FormatReset(s.weeklyResetAt) accent:weeklyColor];
    NSString *fiveState = s.fiveHourDataState ?: ((s.quotaErrorText.length > 0 && s.fiveHourAvailable) ? @"previous" : nil);
    NSString *weeklyState = s.weeklyDataState ?: ((s.quotaErrorText.length > 0 && s.weeklyAvailable) ? @"previous" : nil);
    HUDApplyQuotaDataState(self.hudView.fiveHourCard, fiveState, s.fiveHourAvailable, s.fiveHourRemainingPercent, s.fiveHourResetAt, s.rateLimitReachedType);
    HUDApplyQuotaDataState(self.hudView.weeklyCard, weeklyState, s.weeklyAvailable, s.weeklyRemainingPercent, s.weeklyResetAt, s.rateLimitReachedType);
    HUDApplyQuotaDataState(self.hudView.homeFiveHourCard, fiveState, s.fiveHourAvailable, s.fiveHourRemainingPercent, s.fiveHourResetAt, s.rateLimitReachedType);
    HUDApplyQuotaDataState(self.hudView.homeWeeklyCard, weeklyState, s.weeklyAvailable, s.weeklyRemainingPercent, s.weeklyResetAt, s.rateLimitReachedType);
    [self updateTokenWindowDisplay:s];
    [self updateQuotaDetailsDisplay:s];
    self.hudView.planCard.valueLabel.stringValue = plan ?: @"当前未返回";
    self.hudView.planCard.subtitleLabel.stringValue = s.accountErrorText.length > 0 ? s.accountErrorText : [NSString stringWithFormat:@"不显示邮箱 · %@", FormatAge(s.accountUpdatedAt)];
    self.hudView.homePlanCard.valueLabel.stringValue = self.hudView.planCard.valueLabel.stringValue;
    self.hudView.homePlanCard.subtitleLabel.stringValue = self.hudView.planCard.subtitleLabel.stringValue;
    NSString *activityValue = s.activityErrorText.length > 0 ? @"当前无法可靠判断" : @"当前未返回";
    NSString *activitySubtitle = s.activityErrorText ?: @"本机会话记录 · 活跃5秒 · 空闲20秒";
    if (s.activityAvailable) {
        if (s.activeTaskCount > 0) activityValue = [NSString stringWithFormat:s.activityPartial ? @"%ld个可确认活跃 · 最长%@" : @"%ld个活跃 · 最长%@", (long)s.activeTaskCount, FormatDuration(s.longestActiveTaskSec)];
        else activityValue = s.activityPartial ? @"暂无可确认的活跃任务" : @"当前没有活跃任务";
        if (s.activeTaskNames.count > 0) activitySubtitle = [s.activeTaskNames componentsJoinedByString:@"、"];
        else if (s.activityNoteText.length > 0) activitySubtitle = s.activityNoteText;
        else activitySubtitle = @"本机活动推测 · 活跃5秒 · 空闲20秒";
    }
    self.hudView.taskActivityCard.valueLabel.stringValue = activityValue;
    self.hudView.taskActivityCard.subtitleLabel.stringValue = activitySubtitle;
    self.hudView.homeTaskActivityCard.valueLabel.stringValue = activityValue;
    self.hudView.homeTaskActivityCard.subtitleLabel.stringValue = activitySubtitle;
    NSMutableArray<NSString *> *recentRows = [NSMutableArray array];
    for (NSDictionary<NSString *, id> *task in s.recentTasks ?: @[]) {
        NSString *name = [task[@"name"] isKindOfClass:NSString.class] ? task[@"name"] : @"未命名任务";
        NSTimeInterval updatedAt = [task[@"updatedAt"] doubleValue];
        [recentRows addObject:[NSString stringWithFormat:@"%lu  %@ · %@", (unsigned long)(recentRows.count + 1), name, FormatAge(updatedAt)]];
    }
    NSString *recentFooter = s.recentTasksErrorText.length > 0 ? @"更新失败 · 显示上次列表" : @"官方任务历史 · 不代表正在运行";
    [self.hudView.recentTasksCard updateRows:recentRows footer:recentFooter];
    [self.hudView.homeRecentTasksCard updateRows:recentRows footer:recentFooter];
    if (s.usageAvailable) {
        self.hudView.usageCard.valueLabel.stringValue = s.todayUsageAvailable ? [NSString stringWithFormat:@"今日 %@", FormatTokens(s.todayTokens)] : (s.latestUsageDate.length > 0 ? [NSString stringWithFormat:@"%@ %@", FormatUsageDate(s.latestUsageDate), FormatTokens(s.latestUsageTokens)] : @"今日数据未返回");
        NSString *trend = @"";
        if (s.previousSevenDayTokens > 0) {
            double change = ((double)s.sevenDayTokens / s.previousSevenDayTokens - 1.0) * 100.0;
            trend = [NSString stringWithFormat:@" · %@%.0f%%", change >= 0 ? @"↑" : @"↓", fabs(change)];
        }
        NSString *through = s.todayUsageAvailable ? @"" : (s.latestUsageDate.length > 0 ? @"今日数据未返回 · " : @"");
        self.hudView.usageCard.subtitleLabel.stringValue = s.usageErrorText.length > 0 ? [NSString stringWithFormat:@"%@7天 %@%@ · 更新失败", through, FormatTokens(s.sevenDayTokens), trend] : [NSString stringWithFormat:@"%@7天 %@%@ · 不等于额度", through, FormatTokens(s.sevenDayTokens), trend];
        self.hudView.homeUsageCard.valueLabel.stringValue = self.hudView.usageCard.valueLabel.stringValue;
        self.hudView.homeUsageCard.subtitleLabel.stringValue = self.hudView.usageCard.subtitleLabel.stringValue;
    } else {
        self.hudView.usageCard.valueLabel.stringValue = @"当前未返回";
        self.hudView.usageCard.subtitleLabel.stringValue = s.usageErrorText ?: @"不会换算成额度百分比";
        self.hudView.homeUsageCard.valueLabel.stringValue = self.hudView.usageCard.valueLabel.stringValue;
        self.hudView.homeUsageCard.subtitleLabel.stringValue = self.hudView.usageCard.subtitleLabel.stringValue;
    }
    if (s.localCostAvailable) {
        NSString *costValue = [NSString stringWithFormat:@"安装后近30天 %@ · %@", FormatTokens(s.localThirtyDayTokens), FormatUSD(s.localThirtyDayCostUSD)];
        NSMutableString *costSubtitle = [[NSString stringWithFormat:@"今日 %@ · 近7天 %@ · 本月趋势 %@（估算）", FormatTokens(s.localTodayTokens), FormatTokens(s.localSevenDayTokens), FormatUSD(s.localMonthForecastCostUSD)] mutableCopy];
        if (s.localPricedTokenPercent < 99.5) [costSubtitle appendFormat:@" · 计价覆盖%.0f%%", s.localPricedTokenPercent];
        if (s.localCostScanIncomplete) [costSubtitle appendString:@" · 模型样本更新中"];
        self.hudView.localCostCard.valueLabel.stringValue = costValue;
        self.hudView.localCostCard.subtitleLabel.stringValue = costSubtitle;
        self.hudView.homeLocalCostCard.valueLabel.stringValue = costValue;
        self.hudView.homeLocalCostCard.subtitleLabel.stringValue = costSubtitle;
    } else {
        NSString *costState = s.localCostErrorText.length > 0 ? @"当前不可用" : @"正在读取";
        NSString *costDetail = s.localCostErrorText ?: @"只统计安装软件后的新增记录";
        self.hudView.localCostCard.valueLabel.stringValue = costState;
        self.hudView.localCostCard.subtitleLabel.stringValue = costDetail;
        self.hudView.homeLocalCostCard.valueLabel.stringValue = costState;
        self.hudView.homeLocalCostCard.subtitleLabel.stringValue = costDetail;
    }
    NSMutableArray<NSString *> *forecastHeads = [NSMutableArray array];
    NSMutableArray<NSString *> *forecastDetails = [NSMutableArray array];
    if (s.fiveHourForecastAvailable) {
        [forecastHeads addObject:[NSString stringWithFormat:@"5小时 %@", FormatForecastHeadline(s.fiveHourForecastHeadline)]];
        if (s.fiveHourForecastDetail.length > 0) [forecastDetails addObject:[NSString stringWithFormat:@"5小时 %@", s.fiveHourForecastDetail]];
    }
    if (s.weeklyForecastAvailable) {
        [forecastHeads addObject:[NSString stringWithFormat:@"每周 %@", FormatForecastHeadline(s.weeklyForecastHeadline)]];
        if (s.weeklyForecastDetail.length > 0) [forecastDetails addObject:[NSString stringWithFormat:@"每周 %@", s.weeklyForecastDetail]];
    }
    NSString *forecastValue = forecastHeads.count > 0 ? [forecastHeads componentsJoinedByString:@" · "] : @"正在积累历史";
    NSString *forecastSubtitle = forecastDetails.count > 0 ? [forecastDetails componentsJoinedByString:@" · "] : (s.weeklyForecastDetail.length > 0 ? s.weeklyForecastDetail : (s.fiveHourForecastDetail.length > 0 ? s.fiveHourForecastDetail : @"至少需要15分钟数据"));
    NSColor *forecastColor = [forecastValue containsString:@"提前"] ? NSColor.systemOrangeColor : [self accentColor];
    self.hudView.quotaForecastCard.valueLabel.stringValue = forecastValue;
    self.hudView.quotaForecastCard.subtitleLabel.stringValue = forecastSubtitle;
    self.hudView.quotaForecastCard.valueLabel.textColor = forecastColor;
    self.hudView.homeQuotaForecastCard.valueLabel.stringValue = forecastValue;
    self.hudView.homeQuotaForecastCard.subtitleLabel.stringValue = forecastSubtitle;
    self.hudView.homeQuotaForecastCard.valueLabel.textColor = forecastColor;
    self.hudView.longestTurnCard.valueLabel.stringValue = s.longestRunningTurnAvailable ? FormatDuration(s.longestRunningTurnSec) : @"当前未返回";
    self.hudView.longestTurnCard.subtitleLabel.stringValue = s.usageErrorText.length > 0 ? @"显示上次数据 · 更新失败" : @"账户历史最长";
    self.hudView.homeLongestTurnCard.valueLabel.stringValue = self.hudView.longestTurnCard.valueLabel.stringValue;
    self.hudView.homeLongestTurnCard.subtitleLabel.stringValue = self.hudView.longestTurnCard.subtitleLabel.stringValue;
    self.hudView.longestStreakCard.valueLabel.stringValue = s.longestStreakAvailable ? [NSString stringWithFormat:@"%ld天", (long)s.longestStreakDays] : @"当前未返回";
    self.hudView.longestStreakCard.subtitleLabel.stringValue = s.usageErrorText.length > 0 ? @"显示上次数据 · 更新失败" : @"账户历史最长";
    self.hudView.homeLongestStreakCard.valueLabel.stringValue = self.hudView.longestStreakCard.valueLabel.stringValue;
    self.hudView.homeLongestStreakCard.subtitleLabel.stringValue = self.hudView.longestStreakCard.subtitleLabel.stringValue;
    self.hudView.peakDailyTokensCard.valueLabel.stringValue = s.peakDailyTokensAvailable ? FormatTokens(s.peakDailyTokens) : @"当前未返回";
    self.hudView.peakDailyTokensCard.subtitleLabel.stringValue = s.usageErrorText.length > 0 ? @"显示上次数据 · 更新失败" : @"官方账户历史峰值";
    self.hudView.homePeakDailyTokensCard.valueLabel.stringValue = self.hudView.peakDailyTokensCard.valueLabel.stringValue;
    self.hudView.homePeakDailyTokensCard.subtitleLabel.stringValue = self.hudView.peakDailyTokensCard.subtitleLabel.stringValue;
    if (s.modelQuotaAvailable) {
        self.hudView.modelQuotaCard.valueLabel.stringValue = [NSString stringWithFormat:@"%@ %.0f%%", s.modelQuotaName, s.modelQuotaRemainingPercent];
        self.hudView.modelQuotaCard.subtitleLabel.stringValue = [NSString stringWithFormat:@"%@ · %@", s.modelQuotaWindowLabel ?: @"独立额度", FormatReset(s.modelQuotaResetAt)];
        self.hudView.homeModelQuotaCard.valueLabel.stringValue = self.hudView.modelQuotaCard.valueLabel.stringValue;
        self.hudView.homeModelQuotaCard.subtitleLabel.stringValue = self.hudView.modelQuotaCard.subtitleLabel.stringValue;
    }
    [self.hudView setModelQuotaVisible:self.showModelQuota && s.modelQuotaAvailable];
    [self.hudView setHomeModelQuotaVisible:self.homeShowModelQuota && s.modelQuotaAvailable];
    [self.hudView setLongestTurnVisible:self.showLongestTurn];
    [self.hudView setLongestStreakVisible:self.showLongestStreak];
    [self.hudView setPeakDailyTokensVisible:self.showPeakDailyTokens];
    [self.hudView setHomeLongestTurnVisible:self.homeShowLongestTurn];
    [self.hudView setHomeLongestStreakVisible:self.homeShowLongestStreak];
    [self.hudView setHomePeakDailyTokensVisible:self.homeShowPeakDailyTokens];
    [self.hudView setTaskActivityVisible:self.showTaskActivity];
    [self.hudView setHomeTaskActivityVisible:self.homeShowTaskActivity];
    [self.hudView setRecentTasksVisible:self.showRecentTasks];
    [self.hudView setHomeRecentTasksVisible:self.homeShowRecentTasks];
    [self.hudView setLocalCostVisible:self.showLocalCost];
    [self.hudView setHomeLocalCostVisible:self.homeShowLocalCost];
    [self.hudView setTokenWindowsVisible:self.showTokenWindows];
    [self.hudView setHomeTokenWindowsVisible:self.homeShowTokenWindows];
    [self.hudView setQuotaForecastVisible:self.showQuotaForecast];
    [self.hudView setHomeQuotaForecastVisible:self.homeShowQuotaForecast];
    [self.hudView setQuotaDetailsVisible:self.showQuotaDetails];
    [self.hudView setHomeQuotaDetailsVisible:self.homeShowQuotaDetails];
    [self evaluateWeeklyConsumptionAlert:s];
    BOOL anyQuotaVisible = self.showFiveHourQuota || self.showWeeklyQuota || self.showModelQuota || self.showQuotaDetails || self.homeShowFiveHour || self.homeShowWeekly || self.homeShowModelQuota || self.homeShowQuotaDetails;
    BOOL anyCostVisible = self.showLocalCost || self.homeShowLocalCost || self.showTokenWindows || self.homeShowTokenWindows;
    BOOL anyStale = (anyQuotaVisible && HUDTimestampIsStale(s.quotaUpdatedAt, 900)) || ((self.showPlan || self.homeShowPlan) && HUDTimestampIsStale(s.accountUpdatedAt, 900)) || ((pageUsesUsageData || homeUsesUsageData) && HUDTimestampIsStale(s.usageUpdatedAt, 900)) || ((self.showTaskActivity || self.homeShowTaskActivity) && HUDTimestampIsStale(s.activityUpdatedAt, 60)) || ((self.showRecentTasks || self.homeShowRecentTasks) && HUDTimestampIsStale(s.recentTasksUpdatedAt, 900)) || (anyCostVisible && HUDTimestampIsStale(s.localCostUpdatedAt, 900));
    BOOL anyError = ((self.showFiveHourQuota || self.showWeeklyQuota || self.showModelQuota || self.homeShowFiveHour || self.homeShowWeekly || self.homeShowModelQuota) && s.quotaErrorText.length > 0) || ((self.showPlan || self.homeShowPlan) && s.accountErrorText.length > 0) || ((pageUsesUsageData || homeUsesUsageData) && s.usageErrorText.length > 0) || ((self.showTaskActivity || self.homeShowTaskActivity) && s.activityErrorText.length > 0) || ((self.showRecentTasks || self.homeShowRecentTasks) && s.recentTasksErrorText.length > 0) || (anyCostVisible && s.localCostErrorText.length > 0);
    BOOL ordinaryUsageBlocked = anyQuotaVisible && s.ordinaryUsageAllowed && !s.ordinaryUsageAllowed.boolValue && !HUDTimestampIsStale(s.ordinaryUsageUpdatedAt, 900);
    NSString *pageFreshness = [self freshnessText:s fiveHour:self.showFiveHourQuota weekly:self.showWeeklyQuota plan:self.showPlan usage:pageUsesUsageData model:self.showModelQuota activity:self.showTaskActivity recent:self.showRecentTasks];
    if (self.showLocalCost || self.showTokenWindows) {
        NSString *costFreshness = [NSString stringWithFormat:@"本机Token %@", FormatModuleState(s.localCostUpdatedAt, s.localCostErrorText, 900)];
        pageFreshness = [pageFreshness isEqualToString:@"未启用Codex显示模块"] ? costFreshness : [NSString stringWithFormat:@"%@ · %@", pageFreshness, costFreshness];
    }
    self.hudView.codexFreshnessLabel.stringValue = pageFreshness;
    self.hudView.codexFreshnessLabel.textColor = anyError ? NSColor.systemRedColor : (anyStale ? NSColor.systemOrangeColor : NSColor.tertiaryLabelColor);
    self.hudView.codexStatusLabel.textColor = (anyError || anyStale || ordinaryUsageBlocked) ? NSColor.systemOrangeColor : [self accentColor];
    self.hudView.homeCodexStatusLabel.textColor = self.hudView.codexStatusLabel.textColor;
    NSString *homeCodexFreshness = [self freshnessText:s fiveHour:self.homeShowFiveHour weekly:self.homeShowWeekly plan:self.homeShowPlan usage:homeUsesUsageData model:self.homeShowModelQuota activity:self.homeShowTaskActivity recent:self.homeShowRecentTasks];
    if (self.homeShowLocalCost || self.homeShowTokenWindows) {
        NSString *costFreshness = [NSString stringWithFormat:@"本机Token %@", FormatModuleState(s.localCostUpdatedAt, s.localCostErrorText, 900)];
        homeCodexFreshness = [homeCodexFreshness isEqualToString:@"未启用Codex显示模块"] ? costFreshness : [NSString stringWithFormat:@"%@ · %@", homeCodexFreshness, costFreshness];
    }
    self.hudView.homeFreshnessLabel.stringValue = [homeCodexFreshness isEqualToString:@"未启用Codex显示模块"] ? @"电脑 刚刚" : [NSString stringWithFormat:@"电脑 刚刚 · %@", homeCodexFreshness];
    self.hudView.homeFreshnessLabel.textColor = self.hudView.codexFreshnessLabel.textColor;
    [self updateDetailLabels];
}

- (void)updateTokenWindowDisplay:(CodexStatusSnapshot *)s {
    NSTimeInterval now = NSDate.date.timeIntervalSince1970;
    NSArray<NSDictionary<NSString *, id> *> *buckets = s.localTokenBuckets ?: @[];
    NSTimeInterval trackingStart = s.localTokenBucketsStartedAt;
    NSDictionary *five = nil;
    NSDictionary *week = nil;
    if (s.fiveHourAvailable && s.fiveHourWindowDurationMins > 0 && s.fiveHourResetAt > now) {
        NSTimeInterval start = s.fiveHourResetAt - s.fiveHourWindowDurationMins * 60.0;
        five = CodexTokenWindowSummary(buckets, start, now + 1.0, trackingStart);
        NSString *windowName = FormatQuotaWindow(s.fiveHourWindowDurationMins);
        self.hudView.fiveHourTokensCard.titleLabel.stringValue = [NSString stringWithFormat:@"%@ Token", windowName];
        self.hudView.homeFiveHourTokensCard.titleLabel.stringValue = self.hudView.fiveHourTokensCard.titleLabel.stringValue;
    }
    NSDictionary *day = CodexTokenWindowSummary(buckets, now - 24.0 * 3600.0, now + 1.0, trackingStart);
    if (s.weeklyAvailable && s.weeklyWindowDurationMins > 0 && s.weeklyResetAt > now) {
        NSTimeInterval start = s.weeklyResetAt - s.weeklyWindowDurationMins * 60.0;
        week = CodexTokenWindowSummary(buckets, start, now + 1.0, trackingStart);
    }
    long long weeklyTokens = [week[@"tokens"] longLongValue];
    BOOL weeklyComparisonAvailable = [week[@"complete"] boolValue] && !s.localCostScanIncomplete;
    void (^fill)(HUDMetricCard *, NSDictionary *, NSString *, double, BOOL) = ^(HUDMetricCard *card, NSDictionary *summary, NSString *extra, double officialUsed, BOOL baseline) {
        if (![summary[@"available"] boolValue]) {
            card.valueLabel.stringValue = @"正在积累";
            card.subtitleLabel.stringValue = @"本地统计 · 暂无完整时间点";
            card.valueLabel.textColor = NSColor.tertiaryLabelColor;
            return;
        }
        long long tokens = [summary[@"tokens"] longLongValue];
        BOOL complete = [summary[@"complete"] boolValue] && !s.localCostScanIncomplete;
        card.valueLabel.stringValue = FormatTokens(tokens);
        card.valueLabel.textColor = [self accentColor];
        NSMutableArray<NSString *> *parts = [NSMutableArray array];
        if (baseline) [parts addObject:weeklyComparisonAvailable ? @"周基准100%" : @"周基准待完整"];
        else if (weeklyComparisonAvailable && weeklyTokens > 0) [parts addObject:[NSString stringWithFormat:@"周占%.1f%%", (double)tokens / (double)weeklyTokens * 100.0]];
        else [parts addObject:@"周占待完整"];
        if (officialUsed >= 0) [parts addObject:[NSString stringWithFormat:@"额度%.0f%%", officialUsed]];
        [parts addObject:complete ? @"本地完整" : @"本地局部"];
        if (extra.length > 0) [parts addObject:extra];
        card.subtitleLabel.stringValue = [parts componentsJoinedByString:@" · "];
    };
    double fiveOfficialUsed = s.fiveHourAvailable ? 100.0 - s.fiveHourRemainingPercent : -1;
    double weeklyOfficialUsed = s.weeklyAvailable ? 100.0 - s.weeklyRemainingPercent : -1;
    fill(self.hudView.fiveHourTokensCard, five ?: @{}, @"约5分钟精度", fiveOfficialUsed, NO);
    fill(self.hudView.rollingDayTokensCard, day, @"滚动24h", -1, NO);
    fill(self.hudView.weeklyTokensCard, week ?: @{}, @"官方周期", weeklyOfficialUsed, YES);
    if (!five) self.hudView.fiveHourTokensCard.subtitleLabel.stringValue = @"等待官方短周期 · 本地统计";
    if (!week) self.hudView.weeklyTokensCard.subtitleLabel.stringValue = @"等待官方每周周期 · 本地统计";
    for (NSArray *pair in @[
        @[self.hudView.homeFiveHourTokensCard, self.hudView.fiveHourTokensCard],
        @[self.hudView.homeRollingDayTokensCard, self.hudView.rollingDayTokensCard],
        @[self.hudView.homeWeeklyTokensCard, self.hudView.weeklyTokensCard]
    ]) {
        HUDMetricCard *target = pair[0], *source = pair[1];
        target.titleLabel.stringValue = source.titleLabel.stringValue;
        target.valueLabel.stringValue = source.valueLabel.stringValue;
        target.valueLabel.textColor = source.valueLabel.textColor;
        target.subtitleLabel.stringValue = source.subtitleLabel.stringValue;
    }
}

- (void)updateQuotaDetailsDisplay:(CodexStatusSnapshot *)s {
    NSMutableArray<NSString *> *rows = [NSMutableArray array];
    for (NSDictionary<NSString *, id> *bucket in s.rateLimitBuckets ?: @[]) {
        NSString *name = [bucket[@"name"] isKindOfClass:NSString.class] ? bucket[@"name"] : @"额度";
        double duration = [bucket[@"windowDurationMins"] doubleValue];
        double remaining = [bucket[@"remainingPercent"] doubleValue];
        NSTimeInterval reset = [bucket[@"resetsAt"] doubleValue];
        [rows addObject:[NSString stringWithFormat:@"%@ · %@ · 剩余%.0f%% · %@", name, FormatQuotaWindow(duration), remaining, FormatReset(reset)]];
    }
    NSMutableArray<NSString *> *footer = [NSMutableArray arrayWithObject:@"官方完整额度列表"];
    if (s.rateLimitResetCreditsAvailable) [footer addObject:[NSString stringWithFormat:@"可用恢复次数%ld", (long)s.rateLimitResetCreditsCount]];
    else [footer addObject:@"恢复次数未返回"];
    if (s.rateLimitReachedType.length > 0) [footer addObject:[NSString stringWithFormat:@"官方触顶：%@", s.rateLimitReachedType]];
    if (s.ordinaryUsageAllowed && !HUDTimestampIsStale(s.ordinaryUsageUpdatedAt, 900)) [footer addObject:s.ordinaryUsageAllowed.boolValue ? @"普通包含用量允许" : @"普通包含用量暂不可用"];
    NSString *footerText = [footer componentsJoinedByString:@" · "];
    [self.hudView.quotaDetailsCard updateRows:rows footer:footerText];
    [self.hudView.homeQuotaDetailsCard updateRows:rows footer:footerText];
}

- (void)requestWeeklyNotificationPermissionIfNeeded {
    if (!self.weeklyConsumptionSystemNotificationEnabled) return;
    UNUserNotificationCenter *center = UNUserNotificationCenter.currentNotificationCenter;
    [center getNotificationSettingsWithCompletionHandler:^(UNNotificationSettings *settings) {
        if (settings.authorizationStatus != UNAuthorizationStatusNotDetermined) return;
        [center requestAuthorizationWithOptions:UNAuthorizationOptionAlert completionHandler:^(__unused BOOL granted, __unused NSError *error) {}];
    }];
}

- (void)evaluateWeeklyConsumptionAlert:(CodexStatusSnapshot *)s {
    if (!self.weeklyConsumptionAlertEnabled || !s.weeklyAvailable ||
        ![s.weeklyDataState isEqualToString:@"live"] ||
        s.weeklyResetAt <= NSDate.date.timeIntervalSince1970) return;
    BOOL naturalDay = [self.weeklyConsumptionAlertMode isEqualToString:@"naturalDay"];
    BOOL available = naturalDay ? s.weeklyNaturalDayConsumptionAvailable : s.weeklyRolling24hConsumptionAvailable;
    double consumed = naturalDay ? s.weeklyNaturalDayConsumedPercent : s.weeklyRolling24hConsumedPercent;
    if (!available) return;
    NSString *period = naturalDay ? @"今日" : @"最近24小时";
    NSString *alertText = [NSString stringWithFormat:@"%@已用周额度%.1f%% · 阈值%.0f%%", period, consumed, self.weeklyConsumptionAlertThreshold];
    NSString *baseReset = FormatReset(s.weeklyResetAt);
    self.hudView.weeklyCard.resetLabel.stringValue = [NSString stringWithFormat:@"%@ · %@", baseReset, alertText];
    self.hudView.homeWeeklyCard.resetLabel.stringValue = self.hudView.weeklyCard.resetLabel.stringValue;
    BOOL eligible = HUDWeeklyAlertEligible(self.weeklyConsumptionAlertEnabled,
                                           s.weeklyAvailable,
                                           s.weeklyDataState,
                                           s.weeklyResetAt,
                                           NSDate.date.timeIntervalSince1970,
                                           available,
                                           consumed,
                                           self.weeklyConsumptionAlertThreshold);
    if (!eligible) return;
    self.hudView.weeklyCard.valueLabel.textColor = NSColor.systemOrangeColor;
    self.hudView.homeWeeklyCard.valueLabel.textColor = NSColor.systemOrangeColor;
    if (!self.showWeeklyQuota) {
        self.hudView.codexStatusLabel.stringValue = [NSString stringWithFormat:@"● %@", alertText];
        self.hudView.codexStatusLabel.textColor = NSColor.systemOrangeColor;
    }
    if (!self.homeShowWeekly) {
        self.hudView.homeCodexStatusLabel.stringValue = [NSString stringWithFormat:@"● %@", alertText];
        self.hudView.homeCodexStatusLabel.textColor = NSColor.systemOrangeColor;
    }
    if (!self.weeklyConsumptionSystemNotificationEnabled) return;
    NSString *cycleKey = HUDWeeklyAlertCycleKey(s.weeklyResetAt);
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    if ([[defaults stringForKey:@"weeklyConsumptionLastNotifiedCycle"] isEqualToString:cycleKey]) return;
    UNUserNotificationCenter *center = UNUserNotificationCenter.currentNotificationCenter;
    [center getNotificationSettingsWithCompletionHandler:^(UNNotificationSettings *settings) {
        if (settings.authorizationStatus != UNAuthorizationStatusAuthorized && settings.authorizationStatus != UNAuthorizationStatusProvisional) return;
        UNMutableNotificationContent *content = [UNMutableNotificationContent new];
        content.title = @"Codex周额度提醒";
        content.body = alertText;
        content.threadIdentifier = @"codex-monitor-weekly-quota";
        UNNotificationRequest *request = [UNNotificationRequest requestWithIdentifier:[@"weekly-quota-" stringByAppendingString:cycleKey] content:content trigger:nil];
        [center addNotificationRequest:request withCompletionHandler:^(NSError *error) {
            if (!error) [defaults setObject:cycleKey forKey:@"weeklyConsumptionLastNotifiedCycle"];
        }];
    }];
}

- (void)updateDetailLabels {
    if (!self.hudView || self.compact) return;
    for (NSTextField *label in self.hudView.detailLabels) { label.hidden = YES; label.stringValue = @""; }
    NSInteger row = 0;
    if (self.currentPage == 0) {
        CodexStatusSnapshot *s = self.codexProvider.snapshot;
        if (self.homeShowTaskActivity && row < 5) { self.hudView.detailLabels[row].stringValue = s.activityAvailable ? [NSString stringWithFormat:@"任务活动   %ld个%@ · 最长%@ · 本机推测", (long)s.activeTaskCount, s.activityPartial ? @"可确认活跃" : @"活跃", FormatDuration(s.longestActiveTaskSec)] : @"任务活动   当前无法可靠判断"; self.hudView.detailLabels[row++].hidden = NO; }
        if (self.homeShowRecentTasks && row < 5) { self.hudView.detailLabels[row].stringValue = s.recentTasksAvailable ? [NSString stringWithFormat:@"最近任务   已读取%ld项 · 历史列表", (long)s.recentTaskCount] : @"最近任务   当前未返回"; self.hudView.detailLabels[row++].hidden = NO; }
        if (self.homeShowServiceStatus && row < 5) { HUDOpenAIServiceStatusSnapshot *service = self.serviceStatusProvider.snapshot; self.hudView.detailLabels[row].stringValue = service.available ? [NSString stringWithFormat:@"官方状态   %@ · %@", service.headline, FormatAge(service.updatedAt)] : @"官方状态   正在读取"; self.hudView.detailLabels[row++].hidden = NO; }
        NSMutableArray<NSString *> *quotaParts = [NSMutableArray array];
        if (self.homeShowFiveHour) [quotaParts addObject:s.fiveHourAvailable ? [NSString stringWithFormat:@"5小时 %.0f%%", s.fiveHourRemainingPercent] : @"5小时未返回"];
        if (self.homeShowWeekly) [quotaParts addObject:s.weeklyAvailable ? [NSString stringWithFormat:@"每周 %.0f%%", s.weeklyRemainingPercent] : @"每周未返回"];
        if (quotaParts.count && row < 5) { self.hudView.detailLabels[row].stringValue = [NSString stringWithFormat:@"额度摘要   %@", [quotaParts componentsJoinedByString:@" · "]]; self.hudView.detailLabels[row++].hidden = NO; }
        if (self.homeShowPlan && row < 5) { self.hudView.detailLabels[row].stringValue = [NSString stringWithFormat:@"订阅类型   %@", s.accountAvailable ? FormatPlan(s.planType) : @"当前未返回"]; self.hudView.detailLabels[row++].hidden = NO; }
        if (self.homeShowUsage && row < 5) {
            NSString *today = s.todayUsageAvailable ? [NSString stringWithFormat:@"今日 %@", FormatTokens(s.todayTokens)] : (s.latestUsageDate.length > 0 ? [NSString stringWithFormat:@"最新%@ %@", FormatUsageDate(s.latestUsageDate), FormatTokens(s.latestUsageTokens)] : @"今日未返回");
            self.hudView.detailLabels[row].stringValue = s.usageAvailable ? [NSString stringWithFormat:@"用量摘要   %@ · 7天 %@", today, FormatTokens(s.sevenDayTokens)] : @"用量摘要   当前接口未返回";
            self.hudView.detailLabels[row++].hidden = NO;
        }
        if (self.homeShowLongestTurn && row < 5) { self.hudView.detailLabels[row].stringValue = s.longestRunningTurnAvailable ? [NSString stringWithFormat:@"最长单次   %@", FormatDuration(s.longestRunningTurnSec)] : @"最长单次   当前接口未返回"; self.hudView.detailLabels[row++].hidden = NO; }
        if (self.homeShowLongestStreak && row < 5) { self.hudView.detailLabels[row].stringValue = s.longestStreakAvailable ? [NSString stringWithFormat:@"最长连续   %ld天", (long)s.longestStreakDays] : @"最长连续   当前接口未返回"; self.hudView.detailLabels[row++].hidden = NO; }
        NativeSnapshot *n = self.lastSnapshot;
        if ((self.homeShowDiagnosis || self.homeShowSystem) && row < 5) { self.hudView.detailLabels[row].stringValue = n ? [NSString stringWithFormat:@"电脑摘要   CPU %.0f%% · 内存 %.1f/%.0fG (%.0f%%) · 压力 %@", n.systemCPUPercent, n.systemMemoryUsedGiB, n.totalMemoryGiB, n.systemMemoryUsedPercent, n.memoryPressureText] : @"电脑摘要   正在采样"; self.hudView.detailLabels[row++].hidden = NO; }
        if (self.homeShowAttribution && row < 5) { self.hudView.detailLabels[row].stringValue = n ? [NSString stringWithFormat:@"Codex摘要  CPU %.1f%%（整机） · 内存 %.1fG（总量%.0f%%）", n.codexCPUPercent, n.codexMemoryGiB, n.codexMemoryPercent] : @"Codex摘要  正在采样"; self.hudView.detailLabels[row++].hidden = NO; }
        if (row < 5) { self.hudView.detailLabels[row].stringValue = @"齿轮可一次查看并修改全部显示选项"; self.hudView.detailLabels[row++].hidden = NO; }
    } else if (self.currentPage == 1) {
        CodexStatusSnapshot *s = self.codexProvider.snapshot;
        if (self.showTaskActivity && row < 5) { self.hudView.detailLabels[row].stringValue = s.activityAvailable ? [NSString stringWithFormat:@"任务活动   %ld个%@ · 最长%@ · 本机推测", (long)s.activeTaskCount, s.activityPartial ? @"可确认活跃" : @"活跃", FormatDuration(s.longestActiveTaskSec)] : @"任务活动   当前无法可靠判断"; self.hudView.detailLabels[row++].hidden = NO; }
        if (self.showRecentTasks && row < 5) { self.hudView.detailLabels[row].stringValue = s.recentTasksAvailable ? [NSString stringWithFormat:@"最近任务   已读取%ld项 · 历史列表", (long)s.recentTaskCount] : @"最近任务   当前未返回"; self.hudView.detailLabels[row++].hidden = NO; }
        if (self.showServiceStatus && row < 5) { HUDOpenAIServiceStatusSnapshot *service = self.serviceStatusProvider.snapshot; self.hudView.detailLabels[row].stringValue = service.available ? [NSString stringWithFormat:@"官方状态   %@ · %@", service.headline, FormatAge(service.updatedAt)] : @"官方状态   正在读取"; self.hudView.detailLabels[row++].hidden = NO; }
        if (self.showFiveHourQuota && row < 5) { self.hudView.detailLabels[row].stringValue = s.fiveHourAvailable ? [NSString stringWithFormat:@"5小时额度  剩余 %.0f%% · %@", s.fiveHourRemainingPercent, FormatReset(s.fiveHourResetAt)] : @"5小时额度  当前接口未返回"; self.hudView.detailLabels[row++].hidden = NO; }
        if (self.showWeeklyQuota && row < 5) { self.hudView.detailLabels[row].stringValue = s.weeklyAvailable ? [NSString stringWithFormat:@"每周额度   剩余 %.0f%% · %@", s.weeklyRemainingPercent, FormatReset(s.weeklyResetAt)] : @"每周额度   当前接口未返回"; self.hudView.detailLabels[row++].hidden = NO; }
        if (self.showPlan && row < 5) { self.hudView.detailLabels[row].stringValue = [NSString stringWithFormat:@"订阅类型   %@", s.accountAvailable ? FormatPlan(s.planType) : @"当前未返回"]; self.hudView.detailLabels[row++].hidden = NO; }
        if (self.showUsage && row < 5) {
            NSString *today = s.todayUsageAvailable ? [NSString stringWithFormat:@"今日 %@", FormatTokens(s.todayTokens)] : (s.latestUsageDate.length > 0 ? [NSString stringWithFormat:@"最新%@ %@", FormatUsageDate(s.latestUsageDate), FormatTokens(s.latestUsageTokens)] : @"今日未返回");
            self.hudView.detailLabels[row].stringValue = s.usageAvailable ? [NSString stringWithFormat:@"用量趋势   %@ · 7天 %@ · 连续%ld天", today, FormatTokens(s.sevenDayTokens), (long)s.currentStreakDays] : @"用量趋势   当前接口未返回";
            self.hudView.detailLabels[row++].hidden = NO;
        }
        if (self.showLongestTurn && row < 5) { self.hudView.detailLabels[row].stringValue = s.longestRunningTurnAvailable ? [NSString stringWithFormat:@"最长单次   %@", FormatDuration(s.longestRunningTurnSec)] : @"最长单次   当前接口未返回"; self.hudView.detailLabels[row++].hidden = NO; }
        if (self.showLongestStreak && row < 5) { self.hudView.detailLabels[row].stringValue = s.longestStreakAvailable ? [NSString stringWithFormat:@"最长连续   %ld天", (long)s.longestStreakDays] : @"最长连续   当前接口未返回"; self.hudView.detailLabels[row++].hidden = NO; }
        if (self.showModelQuota && row < 5) { self.hudView.detailLabels[row].stringValue = s.modelQuotaAvailable ? [NSString stringWithFormat:@"模型额度   %@ %.0f%%", s.modelQuotaName, s.modelQuotaRemainingPercent] : @"模型额度   当前未返回"; self.hudView.detailLabels[row++].hidden = NO; }
        if (row < 5) { self.hudView.detailLabels[row].stringValue = @"数据来源   Codex本机接口 · 活跃5秒/空闲20秒 · 账户1–5分钟"; self.hudView.detailLabels[row++].hidden = NO; }
    } else if (self.lastSnapshot) {
        NativeSnapshot *s = self.lastSnapshot;
        if (self.showAttribution && row < 5) { self.hudView.detailLabels[row].stringValue = [NSString stringWithFormat:@"Codex %ld进程 · 渲染%ld · 工具%ld · 最大 %.1fG", (long)s.codexProcessCount, (long)s.codexRendererCount, (long)s.codexHelperCount, s.codexLargestGiB]; self.hudView.detailLabels[row++].hidden = NO; }
        if (self.showAttribution && row < 5) { self.hudView.detailLabels[row].stringValue = [NSString stringWithFormat:@"Codex磁盘  读 %@ · 写 %@", FormatRate(s.codexDiskReadMBps), FormatRate(s.codexDiskWriteMBps)]; self.hudView.detailLabels[row++].hidden = NO; }
        if (self.showSystem && row < 5) { self.hudView.detailLabels[row].stringValue = [NSString stringWithFormat:@"整机内存  %.1f/%.0fG (%.0f%%) · 压缩 %.1fG", s.systemMemoryUsedGiB, s.totalMemoryGiB, s.systemMemoryUsedPercent, s.compressedGiB]; self.hudView.detailLabels[row++].hidden = NO; }
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
    NSRect old = self.panel.frame; NSSize contentSize = [self panelSize]; NSSize baseSize = [self basePanelSize]; NSScreen *screen = self.panel.screen ?: NSScreen.mainScreen;
    [self configureResizeLimitsForBaseSize:baseSize contentSize:contentSize];
    NSSize frameSize = [self frameSizeForContentSize:contentSize];
    CGFloat y = old.origin.y; if (screen && NSMidY(old) >= NSMidY(screen.visibleFrame)) y = NSMaxY(old) - frameSize.height;
    [self.panel setFrame:NSMakeRect(old.origin.x, y, frameSize.width, frameSize.height) display:YES animate:YES];
    [self.hudView setCompact:self.compact]; [self updateDetailLabels]; [self savePosition];
}

- (void)toggleDetail:(id)sender { self.compact = !self.compact; [NSUserDefaults.standardUserDefaults setBool:self.compact forKey:@"compact"]; [self resizePanel]; [self configureSamplingAndTimers]; }
- (void)toggleFiveHourQuota:(id)sender { self.showFiveHourQuota = !self.showFiveHourQuota; [NSUserDefaults.standardUserDefaults setBool:self.showFiveHourQuota forKey:@"showFiveHourQuota"]; [self.hudView setFiveHourQuotaVisible:self.showFiveHourQuota]; [self startCodexProviderIfNeeded]; [self updateCodexDisplay]; [self resizePanel]; }
- (void)toggleWeeklyQuota:(id)sender { self.showWeeklyQuota = !self.showWeeklyQuota; [NSUserDefaults.standardUserDefaults setBool:self.showWeeklyQuota forKey:@"showWeeklyQuota"]; [self.hudView setWeeklyQuotaVisible:self.showWeeklyQuota]; [self startCodexProviderIfNeeded]; [self updateCodexDisplay]; [self resizePanel]; }
- (void)togglePlan:(id)sender { self.showPlan = !self.showPlan; [NSUserDefaults.standardUserDefaults setBool:self.showPlan forKey:@"showPlan"]; [self.hudView setPlanVisible:self.showPlan]; [self startCodexProviderIfNeeded]; [self updateCodexDisplay]; [self resizePanel]; }
- (void)toggleUsage:(id)sender { self.showUsage = !self.showUsage; [NSUserDefaults.standardUserDefaults setBool:self.showUsage forKey:@"showUsage"]; [self.hudView setUsageVisible:self.showUsage]; [self startCodexProviderIfNeeded]; [self updateCodexDisplay]; [self resizePanel]; }
- (void)toggleLocalCost:(id)sender { self.showLocalCost = !self.showLocalCost; [NSUserDefaults.standardUserDefaults setBool:self.showLocalCost forKey:@"showLocalCost"]; [self.hudView setLocalCostVisible:self.showLocalCost]; if (self.showLocalCost) self.lastCostHistoryFetchAt = 0; [self startCodexProviderIfNeeded]; [self updateCodexDisplay]; [self resizePanel]; }
- (void)toggleTokenWindows:(id)sender { self.showTokenWindows = !self.showTokenWindows; [NSUserDefaults.standardUserDefaults setBool:self.showTokenWindows forKey:@"showTokenWindows"]; [self.hudView setTokenWindowsVisible:self.showTokenWindows]; if (self.showTokenWindows) self.lastCostHistoryFetchAt = 0; [self startCodexProviderIfNeeded]; [self updateCodexDisplay]; [self resizePanel]; }
- (void)toggleQuotaForecast:(id)sender { self.showQuotaForecast = !self.showQuotaForecast; [NSUserDefaults.standardUserDefaults setBool:self.showQuotaForecast forKey:@"showQuotaForecast"]; [self.hudView setQuotaForecastVisible:self.showQuotaForecast]; [self startCodexProviderIfNeeded]; [self updateCodexDisplay]; [self resizePanel]; }
- (void)toggleQuotaDetails:(id)sender { self.showQuotaDetails = !self.showQuotaDetails; [NSUserDefaults.standardUserDefaults setBool:self.showQuotaDetails forKey:@"showQuotaDetails"]; [self.hudView setQuotaDetailsVisible:self.showQuotaDetails]; [self startCodexProviderIfNeeded]; [self updateCodexDisplay]; [self resizePanel]; }
- (void)togglePeakDailyTokens:(id)sender { self.showPeakDailyTokens = !self.showPeakDailyTokens; [NSUserDefaults.standardUserDefaults setBool:self.showPeakDailyTokens forKey:@"showPeakDailyTokens"]; [self.hudView setPeakDailyTokensVisible:self.showPeakDailyTokens]; [self startCodexProviderIfNeeded]; [self updateCodexDisplay]; [self resizePanel]; }
- (void)toggleWeeklyConsumptionAlert:(NSButton *)sender { self.weeklyConsumptionAlertEnabled = sender.state == NSControlStateValueOn; [NSUserDefaults.standardUserDefaults setBool:self.weeklyConsumptionAlertEnabled forKey:@"weeklyConsumptionAlertEnabled"]; [self startCodexProviderIfNeeded]; [self updateCodexDisplay]; }
- (void)toggleWeeklyConsumptionSystemNotification:(NSButton *)sender { self.weeklyConsumptionSystemNotificationEnabled = sender.state == NSControlStateValueOn; [NSUserDefaults.standardUserDefaults setBool:self.weeklyConsumptionSystemNotificationEnabled forKey:@"weeklyConsumptionSystemNotificationEnabled"]; [self requestWeeklyNotificationPermissionIfNeeded]; [self updateCodexDisplay]; }
- (void)changeWeeklyConsumptionAlertMode:(NSPopUpButton *)sender { NSString *mode = sender.selectedItem.representedObject; if (![@[@"rolling24h", @"naturalDay"] containsObject:mode]) return; self.weeklyConsumptionAlertMode = mode; [NSUserDefaults.standardUserDefaults setObject:mode forKey:@"weeklyConsumptionAlertMode"]; [self updateCodexDisplay]; }
- (void)changeWeeklyConsumptionAlertThreshold:(NSSlider *)sender { self.weeklyConsumptionAlertThreshold = MAX(1.0, MIN(100.0, round(sender.doubleValue))); sender.doubleValue = self.weeklyConsumptionAlertThreshold; [NSUserDefaults.standardUserDefaults setDouble:self.weeklyConsumptionAlertThreshold forKey:@"weeklyConsumptionAlertThreshold"]; self.weeklyConsumptionThresholdLabel.stringValue = [NSString stringWithFormat:@"提醒阈值：%.0f%%", self.weeklyConsumptionAlertThreshold]; [self updateCodexDisplay]; }
- (void)toggleServiceStatus:(id)sender { self.showServiceStatus = !self.showServiceStatus; [NSUserDefaults.standardUserDefaults setBool:self.showServiceStatus forKey:@"showServiceStatus"]; [self.hudView setServiceStatusVisible:self.showServiceStatus]; [self startServiceStatusIfNeeded]; [self updateServiceStatusDisplay]; [self resizePanel]; }
- (void)toggleModelQuota:(id)sender { self.showModelQuota = !self.showModelQuota; [NSUserDefaults.standardUserDefaults setBool:self.showModelQuota forKey:@"showModelQuota"]; [self.hudView setModelQuotaVisible:self.showModelQuota && self.codexProvider.snapshot.modelQuotaAvailable]; [self startCodexProviderIfNeeded]; [self updateCodexDisplay]; [self resizePanel]; }
- (void)toggleTaskActivity:(id)sender { self.showTaskActivity = !self.showTaskActivity; [NSUserDefaults.standardUserDefaults setBool:self.showTaskActivity forKey:@"showTaskActivity"]; [self.hudView setTaskActivityVisible:self.showTaskActivity]; [self startCodexProviderIfNeeded]; [self updateCodexDisplay]; [self resizePanel]; }
- (void)toggleRecentTasks:(id)sender { self.showRecentTasks = !self.showRecentTasks; [NSUserDefaults.standardUserDefaults setBool:self.showRecentTasks forKey:@"showRecentTasks"]; [self.hudView setRecentTasksVisible:self.showRecentTasks]; [self startCodexProviderIfNeeded]; [self updateCodexDisplay]; [self resizePanel]; }
- (void)toggleLongestTurn:(id)sender { self.showLongestTurn = !self.showLongestTurn; [NSUserDefaults.standardUserDefaults setBool:self.showLongestTurn forKey:@"showLongestTurn"]; [self.hudView setLongestTurnVisible:self.showLongestTurn]; [self startCodexProviderIfNeeded]; [self updateCodexDisplay]; [self resizePanel]; }
- (void)toggleLongestStreak:(id)sender { self.showLongestStreak = !self.showLongestStreak; [NSUserDefaults.standardUserDefaults setBool:self.showLongestStreak forKey:@"showLongestStreak"]; [self.hudView setLongestStreakVisible:self.showLongestStreak]; [self startCodexProviderIfNeeded]; [self updateCodexDisplay]; [self resizePanel]; }
- (void)toggleSystem:(id)sender { self.showSystem = !self.showSystem; [NSUserDefaults.standardUserDefaults setBool:self.showSystem forKey:@"showSystem"]; [self.hudView setSystemVisible:self.showSystem]; [self updateDetailLabels]; [self configureSamplingAndTimers]; }
- (void)toggleAttribution:(id)sender { self.showAttribution = !self.showAttribution; [NSUserDefaults.standardUserDefaults setBool:self.showAttribution forKey:@"showAttribution"]; [self.hudView setAttributionVisible:self.showAttribution]; [self updateDetailLabels]; [self configureSamplingAndTimers]; }
- (void)toggleTrend:(id)sender { self.showTrend = !self.showTrend; [NSUserDefaults.standardUserDefaults setBool:self.showTrend forKey:@"showTrend"]; [self.hudView setTrendVisible:self.showTrend]; [self updateDetailLabels]; [self configureSamplingAndTimers]; }
- (void)toggleMemoryApps:(id)sender { self.showMemoryApps = !self.showMemoryApps; [NSUserDefaults.standardUserDefaults setBool:self.showMemoryApps forKey:@"showMemoryApps"]; [self.hudView setMemoryAppsVisible:self.showMemoryApps]; [self resizePanel]; [self configureSamplingAndTimers]; }
- (void)toggleHomeFiveHour:(id)sender { self.homeShowFiveHour = !self.homeShowFiveHour; [NSUserDefaults.standardUserDefaults setBool:self.homeShowFiveHour forKey:@"homeShowFiveHour"]; [self.hudView setHomeFiveHourVisible:self.homeShowFiveHour]; [self startCodexProviderIfNeeded]; [self updateCodexDisplay]; [self resizePanel]; }
- (void)toggleHomeWeekly:(id)sender { self.homeShowWeekly = !self.homeShowWeekly; [NSUserDefaults.standardUserDefaults setBool:self.homeShowWeekly forKey:@"homeShowWeekly"]; [self.hudView setHomeWeeklyVisible:self.homeShowWeekly]; [self startCodexProviderIfNeeded]; [self updateCodexDisplay]; [self resizePanel]; }
- (void)toggleHomePlan:(id)sender { self.homeShowPlan = !self.homeShowPlan; [NSUserDefaults.standardUserDefaults setBool:self.homeShowPlan forKey:@"homeShowPlan"]; [self.hudView setHomePlanVisible:self.homeShowPlan]; [self startCodexProviderIfNeeded]; [self updateCodexDisplay]; [self resizePanel]; }
- (void)toggleHomeUsage:(id)sender { self.homeShowUsage = !self.homeShowUsage; [NSUserDefaults.standardUserDefaults setBool:self.homeShowUsage forKey:@"homeShowUsage"]; [self.hudView setHomeUsageVisible:self.homeShowUsage]; [self startCodexProviderIfNeeded]; [self updateCodexDisplay]; [self resizePanel]; }
- (void)toggleHomeLocalCost:(id)sender { self.homeShowLocalCost = !self.homeShowLocalCost; [NSUserDefaults.standardUserDefaults setBool:self.homeShowLocalCost forKey:@"homeShowLocalCost"]; [self.hudView setHomeLocalCostVisible:self.homeShowLocalCost]; if (self.homeShowLocalCost) self.lastCostHistoryFetchAt = 0; [self startCodexProviderIfNeeded]; [self updateCodexDisplay]; [self resizePanel]; }
- (void)toggleHomeTokenWindows:(id)sender { self.homeShowTokenWindows = !self.homeShowTokenWindows; [NSUserDefaults.standardUserDefaults setBool:self.homeShowTokenWindows forKey:@"homeShowTokenWindows"]; [self.hudView setHomeTokenWindowsVisible:self.homeShowTokenWindows]; if (self.homeShowTokenWindows) self.lastCostHistoryFetchAt = 0; [self startCodexProviderIfNeeded]; [self updateCodexDisplay]; [self resizePanel]; }
- (void)toggleHomeQuotaForecast:(id)sender { self.homeShowQuotaForecast = !self.homeShowQuotaForecast; [NSUserDefaults.standardUserDefaults setBool:self.homeShowQuotaForecast forKey:@"homeShowQuotaForecast"]; [self.hudView setHomeQuotaForecastVisible:self.homeShowQuotaForecast]; [self startCodexProviderIfNeeded]; [self updateCodexDisplay]; [self resizePanel]; }
- (void)toggleHomeQuotaDetails:(id)sender { self.homeShowQuotaDetails = !self.homeShowQuotaDetails; [NSUserDefaults.standardUserDefaults setBool:self.homeShowQuotaDetails forKey:@"homeShowQuotaDetails"]; [self.hudView setHomeQuotaDetailsVisible:self.homeShowQuotaDetails]; [self startCodexProviderIfNeeded]; [self updateCodexDisplay]; [self resizePanel]; }
- (void)toggleHomePeakDailyTokens:(id)sender { self.homeShowPeakDailyTokens = !self.homeShowPeakDailyTokens; [NSUserDefaults.standardUserDefaults setBool:self.homeShowPeakDailyTokens forKey:@"homeShowPeakDailyTokens"]; [self.hudView setHomePeakDailyTokensVisible:self.homeShowPeakDailyTokens]; [self startCodexProviderIfNeeded]; [self updateCodexDisplay]; [self resizePanel]; }
- (void)toggleHomeServiceStatus:(id)sender { self.homeShowServiceStatus = !self.homeShowServiceStatus; [NSUserDefaults.standardUserDefaults setBool:self.homeShowServiceStatus forKey:@"homeShowServiceStatus"]; [self.hudView setHomeServiceStatusVisible:self.homeShowServiceStatus]; [self startServiceStatusIfNeeded]; [self updateServiceStatusDisplay]; [self resizePanel]; }
- (void)toggleHomeModelQuota:(id)sender { self.homeShowModelQuota = !self.homeShowModelQuota; [NSUserDefaults.standardUserDefaults setBool:self.homeShowModelQuota forKey:@"homeShowModelQuota"]; [self.hudView setHomeModelQuotaVisible:self.homeShowModelQuota && self.codexProvider.snapshot.modelQuotaAvailable]; [self startCodexProviderIfNeeded]; [self updateCodexDisplay]; [self resizePanel]; }
- (void)toggleHomeTaskActivity:(id)sender { self.homeShowTaskActivity = !self.homeShowTaskActivity; [NSUserDefaults.standardUserDefaults setBool:self.homeShowTaskActivity forKey:@"homeShowTaskActivity"]; [self.hudView setHomeTaskActivityVisible:self.homeShowTaskActivity]; [self startCodexProviderIfNeeded]; [self updateCodexDisplay]; [self resizePanel]; }
- (void)toggleHomeRecentTasks:(id)sender { self.homeShowRecentTasks = !self.homeShowRecentTasks; [NSUserDefaults.standardUserDefaults setBool:self.homeShowRecentTasks forKey:@"homeShowRecentTasks"]; [self.hudView setHomeRecentTasksVisible:self.homeShowRecentTasks]; [self startCodexProviderIfNeeded]; [self updateCodexDisplay]; [self resizePanel]; }
- (void)toggleHomeLongestTurn:(id)sender { self.homeShowLongestTurn = !self.homeShowLongestTurn; [NSUserDefaults.standardUserDefaults setBool:self.homeShowLongestTurn forKey:@"homeShowLongestTurn"]; [self.hudView setHomeLongestTurnVisible:self.homeShowLongestTurn]; [self startCodexProviderIfNeeded]; [self updateCodexDisplay]; [self resizePanel]; }
- (void)toggleHomeLongestStreak:(id)sender { self.homeShowLongestStreak = !self.homeShowLongestStreak; [NSUserDefaults.standardUserDefaults setBool:self.homeShowLongestStreak forKey:@"homeShowLongestStreak"]; [self.hudView setHomeLongestStreakVisible:self.homeShowLongestStreak]; [self startCodexProviderIfNeeded]; [self updateCodexDisplay]; [self resizePanel]; }
- (void)toggleHomeDiagnosis:(id)sender { self.homeShowDiagnosis = !self.homeShowDiagnosis; [NSUserDefaults.standardUserDefaults setBool:self.homeShowDiagnosis forKey:@"homeShowDiagnosis"]; [self.hudView setHomeDiagnosisVisible:self.homeShowDiagnosis]; [self resizePanel]; [self configureSamplingAndTimers]; }
- (void)toggleHomeSystem:(id)sender { self.homeShowSystem = !self.homeShowSystem; [NSUserDefaults.standardUserDefaults setBool:self.homeShowSystem forKey:@"homeShowSystem"]; [self.hudView setHomeSystemVisible:self.homeShowSystem]; [self resizePanel]; [self configureSamplingAndTimers]; }
- (void)toggleHomeAttribution:(id)sender { self.homeShowAttribution = !self.homeShowAttribution; [NSUserDefaults.standardUserDefaults setBool:self.homeShowAttribution forKey:@"homeShowAttribution"]; [self.hudView setHomeAttributionVisible:self.homeShowAttribution]; [self resizePanel]; [self configureSamplingAndTimers]; }
- (void)toggleHomeTrend:(id)sender { self.homeShowTrend = !self.homeShowTrend; [NSUserDefaults.standardUserDefaults setBool:self.homeShowTrend forKey:@"homeShowTrend"]; [self.hudView setHomeTrendVisible:self.homeShowTrend]; [self resizePanel]; [self configureSamplingAndTimers]; }
- (void)toggleHomeMemoryApps:(id)sender { self.homeShowMemoryApps = !self.homeShowMemoryApps; [NSUserDefaults.standardUserDefaults setBool:self.homeShowMemoryApps forKey:@"homeShowMemoryApps"]; [self.hudView setHomeMemoryAppsVisible:self.homeShowMemoryApps]; [self resizePanel]; [self configureSamplingAndTimers]; }
- (void)toggleHistory:(id)sender { self.historyEnabled = !self.historyEnabled; [NSUserDefaults.standardUserDefaults setBool:self.historyEnabled forKey:@"historyEnabled"]; [self configureSamplingAndTimers]; }
- (void)toggleAlwaysOnTop:(id)sender { self.alwaysOnTop = !self.alwaysOnTop; [NSUserDefaults.standardUserDefaults setBool:self.alwaysOnTop forKey:@"alwaysOnTop"]; self.panel.level = self.alwaysOnTop ? NSFloatingWindowLevel : NSNormalWindowLevel; [self.hudView setAlwaysOnTop:self.alwaysOnTop]; if (self.alwaysOnTop) [self.panel orderFrontRegardless]; }
- (void)togglePositionLock:(id)sender { self.positionLocked = !self.positionLocked; [NSUserDefaults.standardUserDefaults setBool:self.positionLocked forKey:@"positionLocked"]; [self applyPositionLock]; }
- (void)minimizeToDock:(id)sender { [self.panel miniaturize:nil]; }
- (void)toggleCollapsed:(id)sender { self.collapsed = !self.collapsed; [self.hudView setCollapsed:self.collapsed]; [self resizePanel]; }
- (void)selectPage:(NSMenuItem *)sender { self.currentPage = sender.tag; [NSUserDefaults.standardUserDefaults setInteger:self.currentPage forKey:@"currentPage"]; [self.hudView setPage:self.currentPage]; [self updateDetailLabels]; [self resizePanel]; [self configureSamplingAndTimers]; }
- (void)setOpacity:(NSMenuItem *)sender { self.backgroundOpacity = sender.tag / 100.0; [NSUserDefaults.standardUserDefaults setDouble:self.backgroundOpacity forKey:@"opacity"]; [self.hudView setBackgroundOpacity:self.backgroundOpacity]; }
- (void)setAccent:(NSMenuItem *)sender { self.accentName = sender.representedObject; [NSUserDefaults.standardUserDefaults setObject:self.accentName forKey:@"accentName"]; self.hudView.accentColor = [self accentColor]; self.hudView.codexStatusLabel.textColor = self.hudView.accentColor; if ([self systemDataNeeded]) [self updateDisplay]; [self updateCodexDisplay]; }
- (void)setRefresh:(NSMenuItem *)sender { self.refreshInterval = sender.tag; [NSUserDefaults.standardUserDefaults setDouble:self.refreshInterval forKey:@"refreshInterval"]; [self startSystemTimer]; if ([self systemDataNeeded]) [self updateDisplay]; }
- (void)resetWindowScale:(id)sender {
    if (self.positionLocked) return;
    self.windowScale = 1.0;
    [NSUserDefaults.standardUserDefaults setDouble:self.windowScale forKey:@"windowScale"];
    [self resizePanel];
}

- (void)moveToCorner:(NSString *)corner {
    NSScreen *screen = self.panel.screen ?: NSScreen.mainScreen; if (!screen) return;
    NSRect v = screen.visibleFrame; NSSize s = [self frameSizeForContentSize:[self panelSize]]; CGFloat p = 16;
    BOOL right = [corner containsString:@"Right"], top = [corner containsString:@"top"] || [corner containsString:@"Top"];
    CGFloat x = right ? NSMaxX(v) - s.width - p : NSMinX(v) + p;
    CGFloat y = top ? NSMaxY(v) - s.height - p : NSMinY(v) + p;
    [self.panel setFrame:NSMakeRect(x, y, s.width, s.height) display:YES]; [self savePosition];
}
- (void)moveCornerFromMenu:(NSMenuItem *)sender { if (!self.positionLocked) [self moveToCorner:sender.representedObject]; }
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
        @"KeepAlive": @{ @"SuccessfulExit": @NO },
        @"ThrottleInterval": @10,
        @"LimitLoadToSessionType": @"Aqua",
        @"ProcessType": @"Interactive",
        @"StandardOutPath": [logDirectory stringByAppendingPathComponent:@"hud.stdout.log"],
        @"StandardErrorPath": [logDirectory stringByAppendingPathComponent:@"hud.stderr.log"]
    };
    [plist writeToFile:path atomically:YES];
}

- (void)checkForUpdatesAutomatically {
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    NSTimeInterval now = NSDate.date.timeIntervalSince1970;
    NSTimeInterval lastCheck = [defaults doubleForKey:@"lastAutomaticUpdateCheckAt"];
    if (lastCheck > 0 && now - lastCheck < HUDAutomaticUpdateCheckInterval) return;
    [defaults setDouble:now forKey:@"lastAutomaticUpdateCheckAt"];
    [self performUpdateCheckManual:NO];
}
- (void)checkForUpdatesManually:(id)sender { [self performUpdateCheckManual:YES]; }

- (void)performUpdateCheckManual:(BOOL)manual {
    if (self.updateCheckInProgress || self.updateInstalling) return;
    self.updateCheckInProgress = YES;
    self.updateButton.enabled = NO;
    self.updateStatusLabel.stringValue = @"正在检查 GitHub 新版本…";
    __weak typeof(self) weakSelf = self;
    [self.updateManager checkForUpdates:^(HUDUpdateCheckResult result, HUDReleaseInfo *release, NSString *message) {
        weakSelf.updateCheckInProgress = NO;
        weakSelf.updateButton.enabled = YES;
        weakSelf.updateStatusLabel.stringValue = message;
        if (result == HUDUpdateCheckResultAvailable) {
            NSString *lastPrompted = [NSUserDefaults.standardUserDefaults stringForKey:@"lastPromptedUpdateVersion"];
            NSTimeInterval lastPromptedAt = [NSUserDefaults.standardUserDefaults doubleForKey:@"lastPromptedUpdateAt"];
            BOOL reminderDue = NSDate.date.timeIntervalSince1970 - lastPromptedAt >= 24.0 * 60.0 * 60.0;
            if (manual || ![lastPrompted isEqualToString:release.version] || reminderDue) {
                [NSUserDefaults.standardUserDefaults setObject:release.version forKey:@"lastPromptedUpdateVersion"];
                [NSUserDefaults.standardUserDefaults setDouble:NSDate.date.timeIntervalSince1970 forKey:@"lastPromptedUpdateAt"];
                [weakSelf presentUpdatePrompt:release];
            }
        } else if (manual) {
            NSAlert *alert = [NSAlert new];
            alert.messageText = result == HUDUpdateCheckResultUpToDate ? @"已经是最新版" : @"暂时无法检查更新";
            alert.informativeText = message;
            [alert addButtonWithTitle:@"好"];
            [alert runModal];
        }
    }];
}

- (void)presentUpdatePrompt:(HUDReleaseInfo *)release {
    NSAlert *alert = [NSAlert new];
    alert.messageText = [NSString stringWithFormat:@"发现 Codex Monitor HUD %@", release.version];
    NSString *notes = release.releaseNotes.length ? release.releaseNotes : @"新版本已经可以下载。";
    alert.informativeText = notes.length > 700 ? [[notes substringToIndex:700] stringByAppendingString:@"…"] : notes;
    [alert addButtonWithTitle:@"一键更新"];
    [alert addButtonWithTitle:@"稍后"];
    [alert addButtonWithTitle:@"查看发布页面"];
    NSModalResponse response = [alert runModal];
    if (response == NSAlertFirstButtonReturn) [self installRelease:release];
    else if (response == NSAlertThirdButtonReturn && release.releasePageURL) [[NSWorkspace sharedWorkspace] openURL:release.releasePageURL];
}

- (void)installRelease:(HUDReleaseInfo *)release {
    if (self.updateInstalling) return;
    self.updateInstalling = YES;
    self.updateButton.enabled = NO;
    self.updateStatusLabel.stringValue = @"正在下载并验证更新…";
    __weak typeof(self) weakSelf = self;
    [self.updateManager installRelease:release completion:^(BOOL prepared, NSString *message) {
        weakSelf.updateStatusLabel.stringValue = message;
        if (prepared) {
            [weakSelf appendLifecycleEvent:@"update"];
            [NSApp terminate:nil];
            return;
        }
        weakSelf.updateInstalling = NO;
        weakSelf.updateButton.enabled = YES;
        NSAlert *alert = [NSAlert new];
        alert.messageText = @"一键更新未完成";
        alert.informativeText = message;
        [alert addButtonWithTitle:@"打开发布页面"];
        [alert addButtonWithTitle:@"取消"];
        if ([alert runModal] == NSAlertFirstButtonReturn && release.releasePageURL) [[NSWorkspace sharedWorkspace] openURL:release.releasePageURL];
    }];
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
    if (!self.settingsOrderControllers) self.settingsOrderControllers = [NSMutableArray array];
    if (!self.homeCodexModuleOrder) self.homeCodexModuleOrder = HUDDefaultHomeCodexOrder();
    if (!self.homeComputerModuleOrder) self.homeComputerModuleOrder = HUDDefaultHomeComputerOrder();
    [self.settingsOrderControllers removeAllObjects];
    NSArray<NSView *> *homeControls = @[
        [self settingsCheckbox:@"当前任务活动（本机推测）" action:@selector(toggleHomeTaskActivity:) state:self.homeShowTaskActivity],
        [self settingsCheckbox:@"最近任务（历史）" action:@selector(toggleHomeRecentTasks:) state:self.homeShowRecentTasks],
        [self settingsCheckbox:@"5小时额度" action:@selector(toggleHomeFiveHour:) state:self.homeShowFiveHour],
        [self settingsCheckbox:@"每周额度" action:@selector(toggleHomeWeekly:) state:self.homeShowWeekly],
        [self settingsCheckbox:@"订阅类型" action:@selector(toggleHomePlan:) state:self.homeShowPlan],
        [self settingsCheckbox:@"账户Token统计" action:@selector(toggleHomeUsage:) state:self.homeShowUsage],
        [self settingsCheckbox:@"5小时/24小时/每周Token对比" action:@selector(toggleHomeTokenWindows:) state:self.homeShowTokenWindows],
        [self settingsCheckbox:@"Token用量与费用" action:@selector(toggleHomeLocalCost:) state:self.homeShowLocalCost],
        [self settingsCheckbox:@"额度趋势预测" action:@selector(toggleHomeQuotaForecast:) state:self.homeShowQuotaForecast],
        [self settingsCheckbox:@"OpenAI服务状态" action:@selector(toggleHomeServiceStatus:) state:self.homeShowServiceStatus],
        [self settingsCheckbox:@"完整额度列表与恢复次数" action:@selector(toggleHomeQuotaDetails:) state:self.homeShowQuotaDetails],
        [self settingsCheckbox:@"模型专属额度" action:@selector(toggleHomeModelQuota:) state:self.homeShowModelQuota],
        [self settingsCheckbox:@"最长单次任务时长" action:@selector(toggleHomeLongestTurn:) state:self.homeShowLongestTurn],
        [self settingsCheckbox:@"历史最长连续天数" action:@selector(toggleHomeLongestStreak:) state:self.homeShowLongestStreak],
        [self settingsCheckbox:@"历史单日峰值Token" action:@selector(toggleHomePeakDailyTokens:) state:self.homeShowPeakDailyTokens],
        [self settingsCheckbox:@"瓶颈判断" action:@selector(toggleHomeDiagnosis:) state:self.homeShowDiagnosis],
        [self settingsCheckbox:@"电脑核心状态" action:@selector(toggleHomeSystem:) state:self.homeShowSystem],
        [self settingsCheckbox:@"Codex性能占用" action:@selector(toggleHomeAttribution:) state:self.homeShowAttribution],
        [self settingsCheckbox:@"内存占用排行" action:@selector(toggleHomeMemoryApps:) state:self.homeShowMemoryApps],
        [self settingsCheckbox:@"CPU趋势" action:@selector(toggleHomeTrend:) state:self.homeShowTrend]
    ];
    NSArray<NSView *> *codexControls = @[
        [self settingsCheckbox:@"当前任务活动（本机推测）" action:@selector(toggleTaskActivity:) state:self.showTaskActivity],
        [self settingsCheckbox:@"最近任务（历史）" action:@selector(toggleRecentTasks:) state:self.showRecentTasks],
        [self settingsCheckbox:@"5小时额度" action:@selector(toggleFiveHourQuota:) state:self.showFiveHourQuota],
        [self settingsCheckbox:@"每周额度" action:@selector(toggleWeeklyQuota:) state:self.showWeeklyQuota],
        [self settingsCheckbox:@"订阅类型" action:@selector(togglePlan:) state:self.showPlan],
        [self settingsCheckbox:@"账户Token统计" action:@selector(toggleUsage:) state:self.showUsage],
        [self settingsCheckbox:@"5小时/24小时/每周Token对比" action:@selector(toggleTokenWindows:) state:self.showTokenWindows],
        [self settingsCheckbox:@"Token用量与费用" action:@selector(toggleLocalCost:) state:self.showLocalCost],
        [self settingsCheckbox:@"额度趋势预测" action:@selector(toggleQuotaForecast:) state:self.showQuotaForecast],
        [self settingsCheckbox:@"OpenAI服务状态" action:@selector(toggleServiceStatus:) state:self.showServiceStatus],
        [self settingsCheckbox:@"完整额度列表与恢复次数" action:@selector(toggleQuotaDetails:) state:self.showQuotaDetails],
        [self settingsCheckbox:@"模型专属额度" action:@selector(toggleModelQuota:) state:self.showModelQuota],
        [self settingsCheckbox:@"最长单次任务时长" action:@selector(toggleLongestTurn:) state:self.showLongestTurn],
        [self settingsCheckbox:@"历史最长连续天数" action:@selector(toggleLongestStreak:) state:self.showLongestStreak],
        [self settingsCheckbox:@"历史单日峰值Token" action:@selector(togglePeakDailyTokens:) state:self.showPeakDailyTokens]
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

    __weak typeof(self) weakSelf = self;
    HUDModuleOrderController *codexOrderController = [[HUDModuleOrderController alloc] initWithOrderKey:@"codex" items:HUDOrderItems(self.homeCodexModuleOrder, @{
        @"activity": @"任务活动", @"recent": @"最近任务", @"quota": @"5小时与每周额度",
        @"insights": @"订阅与账户Token统计", @"tokenWindows": @"5小时/24小时/每周Token对比", @"cost": @"Token用量与费用",
        @"forecast": @"额度趋势预测", @"service": @"OpenAI服务状态", @"quotaDetails": @"完整额度列表", @"history": @"历史峰值与连续使用"
    }) changed:^(NSArray<NSString *> *order) {
        weakSelf.homeCodexModuleOrder = order; [NSUserDefaults.standardUserDefaults setObject:order forKey:@"homeCodexModuleOrder"];
        [weakSelf.hudView applyHomeCodexOrder:weakSelf.homeCodexModuleOrder computerOrder:weakSelf.homeComputerModuleOrder];
    }];
    HUDModuleOrderController *computerOrderController = [[HUDModuleOrderController alloc] initWithOrderKey:@"computer" items:HUDOrderItems(self.homeComputerModuleOrder, @{
        @"summary": @"瓶颈与电脑状态", @"attribution": @"Codex性能占用", @"memory": @"内存占用排行", @"trend": @"CPU趋势"
    }) changed:^(NSArray<NSString *> *order) {
        weakSelf.homeComputerModuleOrder = order; [NSUserDefaults.standardUserDefaults setObject:order forKey:@"homeComputerModuleOrder"];
        [weakSelf.hudView applyHomeCodexOrder:weakSelf.homeCodexModuleOrder computerOrder:weakSelf.homeComputerModuleOrder];
    }];
    [self.settingsOrderControllers addObjectsFromArray:@[codexOrderController, computerOrderController]];
    NSTextField *codexOrderHint = [NSTextField labelWithString:@"拖动 ≡ 调整主页中的上下顺序"];
    codexOrderHint.font = [NSFont systemFontOfSize:11]; codexOrderHint.textColor = NSColor.secondaryLabelColor;
    NSTextField *computerOrderHint = [NSTextField labelWithString:@"拖动 ≡ 调整主页中的上下顺序"];
    computerOrderHint.font = [NSFont systemFontOfSize:11]; computerOrderHint.textColor = NSColor.secondaryLabelColor;
    NSBox *codexOrderBox = [self settingsGroup:@"主页 · Codex模块排序" controls:@[codexOrderHint, codexOrderController.scrollView]];
    NSBox *computerOrderBox = [self settingsGroup:@"主页 · 电脑模块排序" controls:@[computerOrderHint, computerOrderController.scrollView]];
    [codexOrderController.scrollView.widthAnchor constraintEqualToAnchor:codexOrderBox.contentView.widthAnchor constant:-24].active = YES;
    [computerOrderController.scrollView.widthAnchor constraintEqualToAnchor:computerOrderBox.contentView.widthAnchor constant:-24].active = YES;
    NSStackView *orderColumns = [NSStackView stackViewWithViews:@[codexOrderBox, computerOrderBox]];
    orderColumns.orientation = NSUserInterfaceLayoutOrientationHorizontal; orderColumns.distribution = NSStackViewDistributionFillEqually; orderColumns.spacing = 12;

    NSButton *weeklyAlertToggle = [self settingsCheckbox:@"启用周额度消耗提醒（默认关闭）" action:@selector(toggleWeeklyConsumptionAlert:) state:self.weeklyConsumptionAlertEnabled];
    NSButton *weeklySystemNotificationToggle = [self settingsCheckbox:@"同时发送静音系统通知（可选）" action:@selector(toggleWeeklyConsumptionSystemNotification:) state:self.weeklyConsumptionSystemNotificationEnabled];
    NSPopUpButton *weeklyAlertMode = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [weeklyAlertMode addItemWithTitle:@"滚动24小时"];
    weeklyAlertMode.lastItem.representedObject = @"rolling24h";
    [weeklyAlertMode addItemWithTitle:@"自然日（当天零点起）"];
    weeklyAlertMode.lastItem.representedObject = @"naturalDay";
    [weeklyAlertMode selectItemAtIndex:[self.weeklyConsumptionAlertMode isEqualToString:@"naturalDay"] ? 1 : 0];
    weeklyAlertMode.target = self; weeklyAlertMode.action = @selector(changeWeeklyConsumptionAlertMode:);
    NSTextField *modeLabel = [NSTextField labelWithString:@"统计方式"];
    modeLabel.font = [NSFont systemFontOfSize:12.5];
    NSStackView *modeRow = [NSStackView stackViewWithViews:@[modeLabel, weeklyAlertMode]];
    modeRow.orientation = NSUserInterfaceLayoutOrientationHorizontal; modeRow.alignment = NSLayoutAttributeCenterY; modeRow.spacing = 10;
    self.weeklyConsumptionThresholdLabel = [NSTextField labelWithString:[NSString stringWithFormat:@"提醒阈值：%.0f%%", self.weeklyConsumptionAlertThreshold]];
    self.weeklyConsumptionThresholdLabel.font = [NSFont systemFontOfSize:12.5];
    NSSlider *weeklyAlertThreshold = [NSSlider sliderWithValue:self.weeklyConsumptionAlertThreshold minValue:1 maxValue:100 target:self action:@selector(changeWeeklyConsumptionAlertThreshold:)];
    weeklyAlertThreshold.continuous = YES;
    [weeklyAlertThreshold.widthAnchor constraintEqualToConstant:280].active = YES;
    NSTextField *alertHint = [NSTextField wrappingLabelWithString:@"阈值可选1%—100%；数据不足不提醒，同一周额度周期只提醒一次。系统通知复用现有额度刷新，不增加轮询。"];
    alertHint.font = [NSFont systemFontOfSize:11]; alertHint.textColor = NSColor.secondaryLabelColor;
    NSBox *weeklyAlertBox = [self settingsGroup:@"周额度消耗提醒" controls:@[weeklyAlertToggle, weeklySystemNotificationToggle, modeRow, self.weeklyConsumptionThresholdLabel, weeklyAlertThreshold, alertHint]];

    NSTextField *sizeLabel = [NSTextField wrappingLabelWithString:@"拖动悬浮窗任意边角连续缩放；最小75%，最大尺寸按当前屏幕自动决定"];
    sizeLabel.font = [NSFont systemFontOfSize:12.5 weight:NSFontWeightRegular];
    NSButton *resetSize = [NSButton buttonWithTitle:@"恢复标准大小" target:self action:@selector(resetWindowScale:)];
    resetSize.bezelStyle = NSBezelStyleRounded;
    resetSize.enabled = !self.positionLocked;
    NSArray<NSView *> *behaviorControls = @[
        [self settingsCheckbox:@"始终置顶" action:@selector(toggleAlwaysOnTop:) state:self.alwaysOnTop],
        [self settingsCheckbox:@"锁定位置和大小" action:@selector(togglePositionLock:) state:self.positionLocked],
        [self settingsCheckbox:@"显示详细信息" action:@selector(toggleDetail:) state:!self.compact],
        [self settingsCheckbox:@"保存每分钟趋势" action:@selector(toggleHistory:) state:self.historyEnabled],
        [self settingsCheckbox:@"登录后自动启动" action:@selector(toggleLaunchAtLogin:) state:[self launchAtLoginEnabled]]
    ];
    NSStackView *sizeStack = [NSStackView stackViewWithViews:@[sizeLabel, resetSize]];
    sizeStack.orientation = NSUserInterfaceLayoutOrientationVertical; sizeStack.alignment = NSLayoutAttributeLeading; sizeStack.spacing = 7;
    NSBox *behaviorBox = [self settingsGroup:@"常驻与显示" controls:behaviorControls];
    NSBox *sizeBox = [self settingsGroup:@"外观" controls:@[sizeStack]];
    NSString *version = self.updateManager.currentVersion ?: ([NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"] ?: @"--");
    self.updateStatusLabel = [NSTextField wrappingLabelWithString:[NSString stringWithFormat:@"当前版本 %@ · 每天自动检查一次", version]];
    self.updateStatusLabel.font = [NSFont systemFontOfSize:12.5 weight:NSFontWeightRegular];
    self.updateButton = [NSButton buttonWithTitle:@"检查更新" target:self action:@selector(checkForUpdatesManually:)];
    self.updateButton.bezelStyle = NSBezelStyleRounded;
    NSStackView *updateStack = [NSStackView stackViewWithViews:@[self.updateStatusLabel, self.updateButton]];
    updateStack.orientation = NSUserInterfaceLayoutOrientationVertical; updateStack.alignment = NSLayoutAttributeLeading; updateStack.spacing = 7;
    NSBox *updateBox = [self settingsGroup:@"软件更新" controls:@[updateStack]];
    NSStackView *bottom = [NSStackView stackViewWithViews:@[behaviorBox, sizeBox, updateBox]];
    bottom.orientation = NSUserInterfaceLayoutOrientationHorizontal; bottom.distribution = NSStackViewDistributionFillEqually; bottom.spacing = 12;

    NSTextField *sourceText = [NSTextField wrappingLabelWithString:@"电脑性能：macOS系统接口。  Codex额度、订阅、账户Token与任务历史：本机Codex官方接口。  5小时/24小时/每周Token与费用：只读取本机会话记录中的时间、模型和Token计数，属于本地统计。  OpenAI服务状态：官方公开状态页，不带账号信息且默认关闭。  任务活动：只读检查近期写入记录。  更新：仅访问本项目GitHub Release。"];
    sourceText.font = [NSFont systemFontOfSize:11.5]; sourceText.textColor = NSColor.labelColor;
    NSTextField *permissionText = [NSTextField wrappingLabelWithString:@"默认不需要屏幕录制、辅助功能、完全磁盘访问、浏览器Cookie或API密钥；费用缓存不保存提示词、回复或工具内容；单任务实时Token速度因无法连接桌面版同一接口实例，当前不会显示。"];
    permissionText.font = [NSFont systemFontOfSize:11.5]; permissionText.textColor = NSColor.secondaryLabelColor;
    NSBox *dataBox = [self settingsGroup:@"数据来源与权限" controls:@[sourceText, permissionText]];

    NSTextField *hint = [NSTextField labelWithString:@"所有勾选状态集中显示；主页扩展和长期历史默认关闭，按需勾选。"];
    hint.font = [NSFont systemFontOfSize:12]; hint.textColor = NSColor.secondaryLabelColor;
    NSStackView *root = [NSStackView stackViewWithViews:@[hint, moduleColumns, orderColumns, weeklyAlertBox, dataBox, bottom]];
    root.orientation = NSUserInterfaceLayoutOrientationVertical; root.alignment = NSLayoutAttributeLeading; root.spacing = 14;
    root.translatesAutoresizingMaskIntoConstraints = NO;
    NSView *content = [[HUDFlippedView alloc] initWithFrame:NSMakeRect(0, 0, 804, 820)]; [content addSubview:root];
    [NSLayoutConstraint activateConstraints:@[
        [root.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:18], [root.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-18],
        [root.topAnchor constraintEqualToAnchor:content.topAnchor constant:16], [root.bottomAnchor constraintLessThanOrEqualToAnchor:content.bottomAnchor constant:-18],
        [moduleColumns.widthAnchor constraintEqualToAnchor:root.widthAnchor], [orderColumns.widthAnchor constraintEqualToAnchor:root.widthAnchor], [weeklyAlertBox.widthAnchor constraintEqualToAnchor:root.widthAnchor], [dataBox.widthAnchor constraintEqualToAnchor:root.widthAnchor], [bottom.widthAnchor constraintEqualToAnchor:root.widthAnchor]
    ]];
    [content layoutSubtreeIfNeeded];
    CGFloat documentHeight = MAX(820, root.fittingSize.height + 34);
    content.frame = NSMakeRect(0, 0, 804, documentHeight);
    NSScrollView *scrollView = [NSScrollView new];
    scrollView.documentView = content; scrollView.hasVerticalScroller = YES; scrollView.drawsBackground = YES; scrollView.backgroundColor = NSColor.windowBackgroundColor; scrollView.autohidesScrollers = YES;
    scrollView.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    return scrollView;
}

- (void)showSettingsWindow:(id)sender {
    if (self.settingsWindow.visible) { [NSApp activateIgnoringOtherApps:YES]; [self.settingsWindow makeKeyAndOrderFront:nil]; return; }
    self.settingsWindow = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 840, 720) styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable) backing:NSBackingStoreBuffered defer:NO];
    self.settingsWindow.title = @"Codex Monitor HUD 设置";
    self.settingsWindow.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
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
    [homeModules addItem:[self item:@"当前任务活动（本机推测）" action:@selector(toggleHomeTaskActivity:) state:self.homeShowTaskActivity]];
    [homeModules addItem:[self item:@"最近任务（历史）" action:@selector(toggleHomeRecentTasks:) state:self.homeShowRecentTasks]];
    [homeModules addItem:[self item:@"5小时额度" action:@selector(toggleHomeFiveHour:) state:self.homeShowFiveHour]];
    [homeModules addItem:[self item:@"每周额度" action:@selector(toggleHomeWeekly:) state:self.homeShowWeekly]];
    [homeModules addItem:[self item:@"订阅类型" action:@selector(toggleHomePlan:) state:self.homeShowPlan]];
    [homeModules addItem:[self item:@"账户Token统计" action:@selector(toggleHomeUsage:) state:self.homeShowUsage]];
    [homeModules addItem:[self item:@"5小时/24小时/每周Token对比" action:@selector(toggleHomeTokenWindows:) state:self.homeShowTokenWindows]];
    [homeModules addItem:[self item:@"Token用量与费用" action:@selector(toggleHomeLocalCost:) state:self.homeShowLocalCost]];
    [homeModules addItem:[self item:@"额度趋势预测" action:@selector(toggleHomeQuotaForecast:) state:self.homeShowQuotaForecast]];
    [homeModules addItem:[self item:@"OpenAI服务状态" action:@selector(toggleHomeServiceStatus:) state:self.homeShowServiceStatus]];
    [homeModules addItem:[self item:@"完整额度列表与恢复次数" action:@selector(toggleHomeQuotaDetails:) state:self.homeShowQuotaDetails]];
    [homeModules addItem:[self item:@"模型专属额度" action:@selector(toggleHomeModelQuota:) state:self.homeShowModelQuota]];
    [homeModules addItem:[self item:@"最长单次任务时长" action:@selector(toggleHomeLongestTurn:) state:self.homeShowLongestTurn]];
    [homeModules addItem:[self item:@"历史最长连续天数" action:@selector(toggleHomeLongestStreak:) state:self.homeShowLongestStreak]];
    [homeModules addItem:[self item:@"历史单日峰值Token" action:@selector(toggleHomePeakDailyTokens:) state:self.homeShowPeakDailyTokens]];
    [homeModules addItem:NSMenuItem.separatorItem];
    [homeModules addItem:[self item:@"瓶颈判断" action:@selector(toggleHomeDiagnosis:) state:self.homeShowDiagnosis]];
    [homeModules addItem:[self item:@"电脑核心状态" action:@selector(toggleHomeSystem:) state:self.homeShowSystem]];
    [homeModules addItem:[self item:@"Codex性能占用" action:@selector(toggleHomeAttribution:) state:self.homeShowAttribution]];
    [homeModules addItem:[self item:@"内存占用排行" action:@selector(toggleHomeMemoryApps:) state:self.homeShowMemoryApps]];
    [homeModules addItem:[self item:@"CPU趋势" action:@selector(toggleHomeTrend:) state:self.homeShowTrend]];
    NSMenuItem *homeRoot = [[NSMenuItem alloc] initWithTitle:@"主页内容" action:nil keyEquivalent:@""]; homeRoot.submenu = homeModules; [menu addItem:homeRoot];
    NSMenu *modules = [NSMenu new];
    [modules addItem:[self item:@"当前任务活动（本机推测）" action:@selector(toggleTaskActivity:) state:self.showTaskActivity]];
    [modules addItem:[self item:@"最近任务（历史）" action:@selector(toggleRecentTasks:) state:self.showRecentTasks]];
    [modules addItem:[self item:@"5小时额度" action:@selector(toggleFiveHourQuota:) state:self.showFiveHourQuota]];
    [modules addItem:[self item:@"每周额度" action:@selector(toggleWeeklyQuota:) state:self.showWeeklyQuota]];
    [modules addItem:[self item:@"订阅类型" action:@selector(togglePlan:) state:self.showPlan]];
    [modules addItem:[self item:@"账户Token统计" action:@selector(toggleUsage:) state:self.showUsage]];
    [modules addItem:[self item:@"5小时/24小时/每周Token对比" action:@selector(toggleTokenWindows:) state:self.showTokenWindows]];
    [modules addItem:[self item:@"Token用量与费用" action:@selector(toggleLocalCost:) state:self.showLocalCost]];
    [modules addItem:[self item:@"额度趋势预测" action:@selector(toggleQuotaForecast:) state:self.showQuotaForecast]];
    [modules addItem:[self item:@"OpenAI服务状态" action:@selector(toggleServiceStatus:) state:self.showServiceStatus]];
    [modules addItem:[self item:@"完整额度列表与恢复次数" action:@selector(toggleQuotaDetails:) state:self.showQuotaDetails]];
    [modules addItem:[self item:@"最长单次任务时长" action:@selector(toggleLongestTurn:) state:self.showLongestTurn]];
    [modules addItem:[self item:@"历史最长连续天数" action:@selector(toggleLongestStreak:) state:self.showLongestStreak]];
    [modules addItem:[self item:@"历史单日峰值Token" action:@selector(togglePeakDailyTokens:) state:self.showPeakDailyTokens]];
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
    NSMenuItem *resetScale = [self item:@"恢复标准大小" action:@selector(resetWindowScale:) state:NO]; resetScale.enabled = !self.positionLocked; [menu addItem:resetScale];
    NSMenu *colors = [NSMenu new];
    for (NSArray *pair in @[@[@"绿色", @"green"], @[@"蓝色", @"blue"], @[@"紫色", @"purple"], @[@"橙色", @"orange"]]) { NSMenuItem *i = [self item:pair[0] action:@selector(setAccent:) state:[self.accentName isEqualToString:pair[1]]]; i.representedObject = pair[1]; [colors addItem:i]; }
    NSMenuItem *colorRoot = [[NSMenuItem alloc] initWithTitle:@"强调颜色" action:nil keyEquivalent:@""]; colorRoot.submenu = colors; [menu addItem:colorRoot];
    NSMenu *refresh = [NSMenu new];
    for (NSNumber *value in @[@5, @10, @20]) { NSMenuItem *i = [self item:[NSString stringWithFormat:@"%@秒", value] action:@selector(setRefresh:) state:fabs(self.refreshInterval - value.doubleValue) < 0.1]; i.tag = value.integerValue; [refresh addItem:i]; }
    NSMenuItem *refreshRoot = [[NSMenuItem alloc] initWithTitle:@"电脑数据刷新" action:nil keyEquivalent:@""]; refreshRoot.submenu = refresh; [menu addItem:refreshRoot];
    NSMenu *positions = [NSMenu new];
    for (NSArray *pair in @[@[@"左下角", @"bottomLeft"], @[@"左上角", @"topLeft"], @[@"右下角", @"bottomRight"], @[@"右上角", @"topRight"]]) { NSMenuItem *i = [self item:pair[0] action:@selector(moveCornerFromMenu:) state:NO]; i.representedObject = pair[1]; i.enabled = !self.positionLocked; [positions addItem:i]; }
    NSMenuItem *positionRoot = [[NSMenuItem alloc] initWithTitle:@"屏幕位置" action:nil keyEquivalent:@""]; positionRoot.submenu = positions; [menu addItem:positionRoot];
    [menu addItem:[self item:@"始终置顶" action:@selector(toggleAlwaysOnTop:) state:self.alwaysOnTop]];
    [menu addItem:[self item:@"锁定位置和大小" action:@selector(togglePositionLock:) state:self.positionLocked]];
    [menu addItem:[self item:@"最小化到程序栏" action:@selector(minimizeToDock:) state:NO]];
    [menu addItem:[self item:(self.collapsed ? @"退出窄栏模式" : @"窄栏模式") action:@selector(toggleCollapsed:) state:self.collapsed]];
    [menu addItem:[self item:@"显示详细信息" action:@selector(toggleDetail:) state:!self.compact]];
    [menu addItem:[self item:@"保存每分钟趋势" action:@selector(toggleHistory:) state:self.historyEnabled]];
    [menu addItem:[self item:@"登录后自动启动" action:@selector(toggleLaunchAtLogin:) state:[self launchAtLoginEnabled]]];
    [menu addItem:[self item:@"检查更新…" action:@selector(checkForUpdatesManually:) state:NO]];
    [menu addItem:NSMenuItem.separatorItem];
    NSMenuItem *open = [[NSMenuItem alloc] initWithTitle:@"打开趋势数据文件夹" action:@selector(openDataFolder:) keyEquivalent:@""]; open.target = self; [menu addItem:open];
    NSMenuItem *quit = [[NSMenuItem alloc] initWithTitle:@"退出悬浮窗" action:@selector(quitHUD:) keyEquivalent:@""]; quit.target = self; [menu addItem:quit];
    return menu;
}

@end

static void CountSettingsControls(NSView *view, NSInteger *checkboxCount, NSInteger *hiddenFiveHourCount, NSInteger *hiddenPlanCount, NSInteger *optionalHistoryOffCount, NSInteger *resetButtonCount) {
    if ([view isKindOfClass:NSButton.class]) {
        NSButton *button = (NSButton *)view;
        if (![button.title isEqualToString:@"恢复标准大小"] && ![button.title isEqualToString:@"检查更新"]) {
            (*checkboxCount)++;
            if ([button.title isEqualToString:@"5小时额度"] && button.state == NSControlStateValueOff) (*hiddenFiveHourCount)++;
            if ([button.title isEqualToString:@"订阅类型"] && button.state == NSControlStateValueOff) (*hiddenPlanCount)++;
            if (([button.title isEqualToString:@"最长单次任务时长"] || [button.title isEqualToString:@"历史最长连续天数"] || [button.title isEqualToString:@"历史单日峰值Token"]) && button.state == NSControlStateValueOff) (*optionalHistoryOffCount)++;
        } else if ([button.title isEqualToString:@"恢复标准大小"]) (*resetButtonCount)++;
    }
    for (NSView *subview in view.subviews) CountSettingsControls(subview, checkboxCount, hiddenFiveHourCount, hiddenPlanCount, optionalHistoryOffCount, resetButtonCount);
}

static int RunUIDiagnostic(void) {
    [NSApplication sharedApplication];
    AppDelegate *delegate = [AppDelegate new];
    delegate.homeShowFiveHour = NO; delegate.showFiveHourQuota = NO;
    delegate.homeShowPlan = NO; delegate.showPlan = NO;
    delegate.homeShowWeekly = YES; delegate.showWeeklyQuota = YES;
    delegate.homeShowUsage = YES; delegate.showUsage = YES;
    delegate.homeShowLongestTurn = NO; delegate.showLongestTurn = NO;
    delegate.homeShowLongestStreak = NO; delegate.showLongestStreak = NO;
    delegate.homeShowTaskActivity = NO; delegate.showTaskActivity = NO;
    delegate.homeShowRecentTasks = NO; delegate.showRecentTasks = NO;
    delegate.windowScale = 1.13; delegate.compact = YES; delegate.currentPage = 0;
    NSView *settings = [delegate settingsContentView];
    NSInteger checkboxCount = 0, hiddenFiveHourCount = 0, hiddenPlanCount = 0, optionalHistoryOffCount = 0, resetButtonCount = 0;
    CountSettingsControls(settings, &checkboxCount, &hiddenFiveHourCount, &hiddenPlanCount, &optionalHistoryOffCount, &resetButtonCount);
    BOOL settingsPass = checkboxCount == 47 && hiddenFiveHourCount == 2 && hiddenPlanCount == 2 && optionalHistoryOffCount == 6 && resetButtonCount == 1 && delegate.settingsOrderControllers.count == 2 && delegate.settingsOrderControllers[0].items.count == 10 && delegate.settingsOrderControllers[1].items.count == 4;
    BOOL scalePass = fabs([delegate panelSize].width - 485.9) < 0.01 && ([delegate panelStyleMask] & NSWindowStyleMaskResizable) != 0;
    delegate.windowScale = 0.75; scalePass = scalePass && fabs([delegate panelSize].width - 322.5) < 0.01;
    delegate.windowScale = 1.0; scalePass = scalePass && fabs([delegate panelSize].width - 430.0) < 0.1;
    delegate.windowScale = 1.5; scalePass = scalePass && fabs([delegate panelSize].width - 645.0) < 0.1;
    CGFloat screenMaximumScale = [delegate maximumWindowScaleForBaseSize:[delegate basePanelSize]];
    delegate.windowScale = screenMaximumScale + 1.0;
    scalePass = scalePass && fabs([delegate panelSize].width - 430.0 * screenMaximumScale) < 0.1;
    NSSize transientBase = NSMakeSize(430, 260);
    NSSize wideResize = HUDContentSizeForUniformScale(transientBase, HUDUniformScaleForProposedContentSize(NSMakeSize(501, 260), transientBase, 1.0));
    NSSize tallResize = HUDContentSizeForUniformScale(transientBase, HUDUniformScaleForProposedContentSize(NSMakeSize(430, 317), transientBase, 1.0));
    NSSize maximumResize = HUDContentSizeForUniformScale(transientBase, HUDUniformScaleForProposedContentSize(NSMakeSize(1000, 260), transientBase, 1.0));
    scalePass = scalePass && fabs(wideResize.width / transientBase.width - wideResize.height / transientBase.height) < 0.0001;
    scalePass = scalePass && fabs(tallResize.width / transientBase.width - tallResize.height / transientBase.height) < 0.0001;
    scalePass = scalePass && fabs(wideResize.width - 501.0) < 0.01 && fabs(tallResize.height - 317.0) < 0.01;
    scalePass = scalePass && maximumResize.width > 645.0 && maximumResize.height > 390.0;
    delegate.hudView = [[HUDView alloc] initWithFrame:NSMakeRect(0, 0, 430, 260)];
    __block BOOL taskCenterRequested = NO;
    delegate.hudView.taskCenterRequested = ^{ taskCenterRequested = YES; };
    [delegate.hudView.taskCenterButton performClick:nil];
    [delegate.hudView layoutSubtreeIfNeeded];
    BOOL taskCenterButtonLayoutPass = delegate.hudView.taskCenterButton.frame.size.width > 0 && delegate.hudView.taskCenterButton.frame.size.height > 0 && NSMaxX(delegate.hudView.taskCenterButton.frame) <= NSMinX(delegate.hudView.settingsButton.frame) + 1.0;
    BOOL taskCenterEntryPass = taskCenterRequested && taskCenterButtonLayoutPass && [delegate.hudView.taskCenterButton.toolTip isEqualToString:@"打开任务中心"] && delegate.hudView.taskCenterButton.image != nil;
    [delegate.hudView setHomeFiveHourVisible:NO]; [delegate.hudView setFiveHourQuotaVisible:NO];
    [delegate.hudView setHomePlanVisible:NO]; [delegate.hudView setPlanVisible:NO];
    [delegate.hudView setHomeWeeklyVisible:YES]; [delegate.hudView setWeeklyQuotaVisible:YES];
    [delegate.hudView setHomeLongestTurnVisible:NO]; [delegate.hudView setLongestTurnVisible:NO];
    [delegate.hudView setHomeLongestStreakVisible:NO]; [delegate.hudView setLongestStreakVisible:NO];
    [delegate.hudView setHomeTaskActivityVisible:NO]; [delegate.hudView setTaskActivityVisible:NO];
    [delegate.hudView setHomeRecentTasksVisible:NO]; [delegate.hudView setRecentTasksVisible:NO];
    [delegate.hudView setHomeLocalCostVisible:NO]; [delegate.hudView setLocalCostVisible:YES];
    [delegate.hudView setHomeQuotaForecastVisible:NO]; [delegate.hudView setQuotaForecastVisible:YES];
    [delegate.hudView setHomeServiceStatusVisible:NO]; [delegate.hudView setServiceStatusVisible:YES];
    [delegate.hudView applyHomeCodexOrder:@[@"recent", @"activity", @"quota", @"insights", @"cost", @"forecast", @"service", @"history"] computerOrder:@[@"trend", @"summary", @"attribution", @"memory"]];
    NSArray<NSView *> *homeArranged = delegate.hudView.homeStack.arrangedSubviews;
    BOOL orderPass = homeArranged.count == 15 && homeArranged[1] == delegate.hudView.homeRecentTasksCard && homeArranged[2] == delegate.hudView.homeTaskActivityCard && homeArranged[5] == delegate.hudView.homeLocalCostCard && homeArranged[6] == delegate.hudView.homeQuotaForecastCard && homeArranged[7] == delegate.hudView.homeServiceStatusCard && homeArranged[10] == delegate.hudView.homeTrendRow;
    BOOL cardVisibilityPass = delegate.hudView.homeTaskActivityCard.hidden && delegate.hudView.taskActivityCard.hidden && delegate.hudView.homeRecentTasksCard.hidden && delegate.hudView.recentTasksCard.hidden && delegate.hudView.homeFiveHourCard.hidden && delegate.hudView.fiveHourCard.hidden && delegate.hudView.homePlanCard.hidden && delegate.hudView.planCard.hidden && !delegate.hudView.homeWeeklyCard.hidden && !delegate.hudView.weeklyCard.hidden && delegate.hudView.homeUsageHistoryRow.hidden && delegate.hudView.usageHistoryRow.hidden && delegate.hudView.homeLocalCostCard.hidden && !delegate.hudView.localCostCard.hidden && delegate.hudView.homeQuotaForecastCard.hidden && !delegate.hudView.quotaForecastCard.hidden && delegate.hudView.homeServiceStatusCard.hidden && !delegate.hudView.serviceStatusCard.hidden;
    HUDView *longTextHUD = [[HUDView alloc] initWithFrame:NSMakeRect(0, 0, 430, 220)];
    [longTextHUD setPage:1]; [longTextHUD setCompact:YES];
    [longTextHUD setTaskActivityVisible:NO]; [longTextHUD setRecentTasksVisible:NO];
    [longTextHUD setFiveHourQuotaVisible:NO]; [longTextHUD setWeeklyQuotaVisible:NO]; [longTextHUD setModelQuotaVisible:NO];
    [longTextHUD setLocalCostVisible:NO]; [longTextHUD setQuotaForecastVisible:NO];
    [longTextHUD setServiceStatusVisible:NO];
    [longTextHUD setLongestTurnVisible:NO]; [longTextHUD setLongestStreakVisible:NO];
    longTextHUD.usageCard.subtitleLabel.stringValue = @"今日数据未返回 · 7天 7900953979 · 上升125% · 不等于额度";
    [longTextHUD layoutSubtreeIfNeeded];
    BOOL longTextLayoutPass = fabs(longTextHUD.planCard.frame.size.width - longTextHUD.usageCard.frame.size.width) <= 1.0;
    AppDelegate *homeDelegate = [AppDelegate new];
    homeDelegate.compact = NO; homeDelegate.currentPage = 0; homeDelegate.windowScale = 1.0;
    homeDelegate.homeShowTaskActivity = YES; homeDelegate.homeShowRecentTasks = YES;
    homeDelegate.homeShowFiveHour = YES; homeDelegate.homeShowWeekly = YES;
    homeDelegate.homeShowPlan = YES; homeDelegate.homeShowUsage = YES; homeDelegate.homeShowModelQuota = YES;
    homeDelegate.homeShowLocalCost = YES; homeDelegate.homeShowQuotaForecast = YES;
    homeDelegate.homeShowServiceStatus = YES;
    homeDelegate.homeShowLongestTurn = YES; homeDelegate.homeShowLongestStreak = YES;
    homeDelegate.homeShowDiagnosis = YES; homeDelegate.homeShowSystem = YES; homeDelegate.homeShowAttribution = YES;
    homeDelegate.homeShowMemoryApps = YES; homeDelegate.homeShowTrend = YES;
    CGFloat fullHomeHeight = [homeDelegate homePanelHeight];
    HUDView *fullHomeHUD = [[HUDView alloc] initWithFrame:NSMakeRect(0, 0, 430, fullHomeHeight)];
    [fullHomeHUD setPage:0]; [fullHomeHUD setCompact:NO];
    [fullHomeHUD setHomeModelQuotaVisible:YES]; [fullHomeHUD setHomeRecentTasksVisible:YES];
    [fullHomeHUD setHomeLocalCostVisible:YES]; [fullHomeHUD setHomeQuotaForecastVisible:YES];
    [fullHomeHUD setHomeServiceStatusVisible:YES];
    [fullHomeHUD setHomeLongestTurnVisible:YES]; [fullHomeHUD setHomeLongestStreakVisible:YES];
    [fullHomeHUD setHomeMemoryAppsVisible:YES];
    [fullHomeHUD applyHomeCodexOrder:HUDDefaultHomeCodexOrder() computerOrder:HUDDefaultHomeComputerOrder()];
    [fullHomeHUD layoutSubtreeIfNeeded];
    NSRect fullHomeBounds = fullHomeHUD.layoutCanvas.bounds;
    NSRect fullHomeRoot = fullHomeHUD.rootStack.frame;
    CGFloat homeWidth = fullHomeHUD.homeStack.frame.size.width;
    BOOL homeWidthPass = fabs(fullHomeHUD.homeTaskActivityCard.frame.size.width - homeWidth) <= 1.0 && fabs(fullHomeHUD.homeRecentTasksCard.frame.size.width - homeWidth) <= 1.0 && fabs(fullHomeHUD.homeQuotaRow.frame.size.width - homeWidth) <= 1.0 && fabs(fullHomeHUD.homeInsightsRow.frame.size.width - homeWidth) <= 1.0 && fabs(fullHomeHUD.homeLocalCostCard.frame.size.width - homeWidth) <= 1.0 && fabs(fullHomeHUD.homeQuotaForecastCard.frame.size.width - homeWidth) <= 1.0 && fabs(fullHomeHUD.homeServiceStatusCard.frame.size.width - homeWidth) <= 1.0 && fabs(fullHomeHUD.homeMemoryAppsCard.frame.size.width - homeWidth) <= 1.0;
    BOOL homeLayoutPass = fullHomeHeight > 650.0 && homeWidthPass && NSMinY(fullHomeRoot) >= NSMinY(fullHomeBounds) - 1.0 && NSMaxY(fullHomeRoot) <= NSMaxY(fullHomeBounds) + 1.0;
    delegate.currentPage = 1; delegate.compact = YES; delegate.collapsed = NO; delegate.windowScale = 1.0;
    [delegate.hudView setPage:1]; [delegate.hudView setCompact:YES];
    delegate.panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, transientBase.width, transientBase.height) styleMask:[delegate panelStyleMask] backing:NSBackingStoreBuffered defer:NO];
    delegate.panel.contentView = delegate.hudView;
    NSSize proposedFrame = [delegate frameSizeForContentSize:NSMakeSize(501, transientBase.height)];
    NSSize constrainedFrame = [delegate windowWillResize:delegate.panel toSize:proposedFrame];
    NSSize constrainedContent = [delegate.panel contentRectForFrameRect:NSMakeRect(0, 0, constrainedFrame.width, constrainedFrame.height)].size;
    scalePass = scalePass && fabs(constrainedContent.width / transientBase.width - constrainedContent.height / transientBase.height) < 0.0001;
    [delegate.panel setContentSize:constrainedContent];
    [delegate windowDidResize:[NSNotification notificationWithName:NSWindowDidResizeNotification object:delegate.panel]];
    NSSize actualFrame = delegate.hudView.frame.size, actualBounds = delegate.hudView.bounds.size;
    scalePass = scalePass && fabs(actualBounds.width - transientBase.width) < 0.0001 && fabs(actualBounds.height - transientBase.height) < 0.0001;
    CGFloat expectedFrameHeight = actualFrame.width * transientBase.height / transientBase.width;
    scalePass = scalePass && fabs(actualFrame.height - expectedFrameHeight) <= 1.0;
    NSStackView *rootStack = delegate.hudView.rootStack;
    NSRect hudBounds = delegate.hudView.layoutCanvas.bounds;
    BOOL layoutInsideBounds = rootStack != nil && NSMinY(rootStack.frame) >= NSMinY(hudBounds) - 1.0 && NSMaxY(rootStack.frame) <= NSMaxY(hudBounds) + 1.0;
    scalePass = scalePass && layoutInsideBounds;
    if (!scalePass) {
        printf("drag_resize_details=wide:%.3fx%.3f tall:%.3fx%.3f constrained:%.3fx%.3f frame:%.3fx%.3f bounds:%.3fx%.3f root:%.3f,%.3f %.3fx%.3f\n",
               wideResize.width, wideResize.height, tallResize.width, tallResize.height,
               constrainedContent.width, constrainedContent.height, actualFrame.width, actualFrame.height,
               actualBounds.width, actualBounds.height, rootStack.frame.origin.x, rootStack.frame.origin.y, rootStack.frame.size.width, rootStack.frame.size.height);
    }
    delegate.positionLocked = YES; [delegate applyPositionLock];
    BOOL positionLockPass = (delegate.panel.styleMask & NSWindowStyleMaskResizable) == 0 && !delegate.panel.movableByWindowBackground && delegate.hudView.positionLocked;
    delegate.positionLocked = NO; [delegate applyPositionLock];
    positionLockPass = positionLockPass && (delegate.panel.styleMask & NSWindowStyleMaskResizable) != 0 && delegate.panel.movableByWindowBackground && !delegate.hudView.positionLocked;
    delegate.historyEnabled = NO; delegate.currentPage = 1; delegate.refreshInterval = 5.0; delegate.sampler = [NativeSampler new]; [delegate configureSamplingAndTimers];
    BOOL hiddenSamplingPass = delegate.systemTimer == nil && !delegate.sampler.collectTopApps && !delegate.sampler.collectSecondaryMetrics && !delegate.sampler.collectThermalMetrics;
    delegate.currentPage = 2; [delegate configureSamplingAndTimers];
    double timerTolerance = delegate.systemTimer.tolerance;
    BOOL timerTolerancePass = delegate.systemTimer != nil && timerTolerance > 0 && timerTolerance <= 0.51;
    delegate.currentPage = 1; delegate.compact = YES;
    delegate.compact = NO; delegate.homeShowUsage = NO; delegate.homeShowDiagnosis = NO; delegate.homeShowSystem = NO; delegate.homeShowAttribution = NO;
    CodexStatusProvider *provider = [CodexStatusProvider new]; delegate.codexProvider = provider;
    provider.snapshot.quotaAvailable = YES; provider.snapshot.weeklyAvailable = YES; provider.snapshot.weeklyRemainingPercent = 42;
    BOOL refreshPolicyPass = HUDCodexActivityRefreshInterval(0) == 20.0 && HUDCodexActivityRefreshInterval(1) == 5.0 && [delegate codexAccountRefreshInterval] == 300.0;
    provider.snapshot.weeklyRemainingPercent = 14; provider.snapshot.activeTaskCount = 1; refreshPolicyPass = refreshPolicyPass && [delegate codexAccountRefreshInterval] == 60.0;
    provider.snapshot.activeTaskCount = 0; refreshPolicyPass = refreshPolicyPass && [delegate codexAccountRefreshInterval] == 300.0; provider.snapshot.weeklyRemainingPercent = 42;
    provider.snapshot.recentTasksErrorText = @"test-error"; refreshPolicyPass = refreshPolicyPass && [delegate codexAccountRefreshInterval] == 60.0; provider.snapshot.recentTasksErrorText = nil;
    provider.snapshot.accountAvailable = YES; provider.snapshot.planType = @"pro"; provider.snapshot.accountErrorText = @"test-error";
    provider.snapshot.quotaErrorText = @"test-error"; provider.snapshot.usageAvailable = YES; provider.snapshot.todayUsageAvailable = NO;
    provider.snapshot.latestUsageDate = @"2026-08-05"; provider.snapshot.latestUsageTokens = 4800000000LL; provider.snapshot.sevenDayTokens = 7600000000LL;
    delegate.showWeeklyQuota = YES; delegate.showUsage = YES; [delegate updateCodexDisplay];
    BOOL stateFallbackPass = [delegate.hudView.weeklyCard.windowLabel.stringValue isEqualToString:@"上次数据"] && [delegate.hudView.usageCard.valueLabel.stringValue containsString:@"8/5"] && ![delegate.hudView.usageCard.valueLabel.stringValue containsString:@"待结算"];
    provider.snapshot.quotaErrorText = nil; provider.snapshot.accountErrorText = nil; provider.snapshot.usageErrorText = nil; provider.snapshot.recentTasksErrorText = nil;
    delegate.showTaskActivity = YES;
    provider.snapshot.activityAvailable = NO; provider.snapshot.activityErrorText = @"近期会话记录已迁移、压缩或暂不可读，无法可靠判断活动";
    [delegate updateCodexDisplay];
    NSString *activityStatus = [delegate visibleCodexStatus:provider.snapshot fiveHour:NO weekly:NO plan:NO usage:NO model:NO activity:YES recent:NO];
    BOOL migrationPresentationPass = [delegate.hudView.taskActivityCard.valueLabel.stringValue isEqualToString:@"当前无法可靠判断"] && [activityStatus isEqualToString:@"任务活动暂时无法可靠判断"];
    provider.snapshot.activityAvailable = YES; provider.snapshot.activityErrorText = nil; provider.snapshot.activityPartial = YES; provider.snapshot.activityNoteText = @"部分近期会话记录已迁移、压缩或暂不可读"; provider.snapshot.activeTaskCount = 0;
    [delegate updateCodexDisplay];
    migrationPresentationPass = migrationPresentationPass && [delegate.hudView.taskActivityCard.valueLabel.stringValue isEqualToString:@"暂无可确认的活跃任务"] && [delegate.hudView.taskActivityCard.subtitleLabel.stringValue containsString:@"部分近期会话记录"];
    provider.snapshot.activityPartial = NO; provider.snapshot.activityNoteText = nil;
    [delegate updateDetailLabels];
    NSMutableArray<NSString *> *visibleDetails = [NSMutableArray array];
    for (NSTextField *label in delegate.hudView.detailLabels) if (!label.hidden) [visibleDetails addObject:label.stringValue];
    NSString *details = [visibleDetails componentsJoinedByString:@" | "];
    NSString *status = [delegate visibleCodexStatus:provider.snapshot fiveHour:NO weekly:YES plan:NO usage:NO model:NO activity:NO recent:NO];
    BOOL hiddenContentPass = [details containsString:@"每周"] && ![details containsString:@"5小时"] && ![details containsString:@"订阅"] && [status isEqualToString:@"Codex数据正常"];
    provider.snapshot.ordinaryUsageAllowed = @NO; provider.snapshot.ordinaryUsageUpdatedAt = NSDate.date.timeIntervalSince1970;
    NSString *allowanceVisible = [delegate visibleCodexStatus:provider.snapshot fiveHour:NO weekly:YES plan:NO usage:NO model:NO activity:NO recent:NO];
    NSString *allowanceHidden = [delegate visibleCodexStatus:provider.snapshot fiveHour:NO weekly:NO plan:NO usage:NO model:NO activity:NO recent:NO];
    BOOL allowancePass = [allowanceVisible containsString:@"普通包含用量暂不可用"] && ![allowanceHidden containsString:@"普通包含用量"];
    provider.snapshot.ordinaryUsageUpdatedAt = 1;
    allowancePass = allowancePass && ![[delegate visibleCodexStatus:provider.snapshot fiveHour:NO weekly:YES plan:NO usage:NO model:NO activity:NO recent:NO] containsString:@"普通包含用量"];
    provider.snapshot.ordinaryUsageAllowed = nil;
    allowancePass = allowancePass && ![[delegate visibleCodexStatus:provider.snapshot fiveHour:NO weekly:YES plan:NO usage:NO model:NO activity:NO recent:NO] containsString:@"普通包含用量"];
    delegate.showServiceStatus = NO; delegate.homeShowServiceStatus = NO;
    delegate.serviceStatusProvider = [HUDOpenAIServiceStatusProvider new];
    [delegate startServiceStatusIfNeeded];
    BOOL hiddenServiceStatusPass = delegate.serviceStatusProvider == nil && delegate.serviceStatusTimer == nil && delegate.lastServiceStatusFetchAt == 0;
    printf("settings_visibility_test=%s\n", settingsPass ? "pass" : "fail");
    if (!settingsPass) printf("settings_visibility_details=checkboxes:%ld five_hour_off:%ld plan_off:%ld optional_history_off:%ld reset_buttons:%ld codex_order:%ld computer_order:%ld\n", (long)checkboxCount, (long)hiddenFiveHourCount, (long)hiddenPlanCount, (long)optionalHistoryOffCount, (long)resetButtonCount, (long)delegate.settingsOrderControllers[0].items.count, (long)delegate.settingsOrderControllers[1].items.count);
    printf("home_module_order_test=%s\n", orderPass ? "pass" : "fail");
    printf("task_center_entry_test=%s\n", taskCenterEntryPass ? "pass" : "fail");
    printf("home_full_layout_test=%s\n", homeLayoutPass ? "pass" : "fail");
    printf("long_text_card_layout_test=%s\n", longTextLayoutPass ? "pass" : "fail");
    printf("adaptive_refresh_test=%s\n", refreshPolicyPass ? "pass" : "fail");
    printf("codex_state_fallback_test=%s\n", stateFallbackPass ? "pass" : "fail");
    printf("codex_migration_state_presentation_test=%s\n", migrationPresentationPass ? "pass" : "fail");
    printf("drag_resize_test=%s\n", scalePass ? "pass" : "fail");
    printf("position_size_lock_test=%s\n", positionLockPass ? "pass" : "fail");
    printf("hidden_sampling_test=%s\n", hiddenSamplingPass ? "pass" : "fail");
    printf("hidden_service_status_test=%s\n", hiddenServiceStatusPass ? "pass" : "fail");
    printf("timer_tolerance_test=%s\n", timerTolerancePass ? "pass" : "fail");
    printf("timer_tolerance_seconds=%.2f\n", timerTolerance);
    printf("hidden_content_test=%s\n", cardVisibilityPass && hiddenContentPass ? "pass" : "fail");
    printf("official_allowance_visibility_test=%s\n", allowancePass ? "pass" : "fail");
    return settingsPass && orderPass && taskCenterEntryPass && homeLayoutPass && longTextLayoutPass && refreshPolicyPass && stateFallbackPass && migrationPresentationPass && scalePass && positionLockPass && hiddenSamplingPass && hiddenServiceStatusPass && timerTolerancePass && cardVisibilityPass && hiddenContentPass && allowancePass ? 0 : 5;
}

static int RunDiagnostic(void) {
    NativeSampler *sampler = [NativeSampler new]; [sampler sample];
    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:5.1]];
    NativeSnapshot *s = [sampler sample];
    printf("system_cpu=%.2f\n", s.systemCPUPercent); printf("codex_cpu=%.2f\n", s.codexCPUPercent); printf("codex_cpu_cores=%.2f\n", s.codexCPUCores);
    printf("system_memory_used_gib=%.3f\n", s.systemMemoryUsedGiB); printf("system_memory_used_percent=%.2f\n", s.systemMemoryUsedPercent); printf("total_memory_gib=%.3f\n", s.totalMemoryGiB);
    printf("codex_memory_gib=%.3f\n", s.codexMemoryGiB); printf("codex_memory_percent=%.2f\n", s.codexMemoryPercent);
    printf("codex_processes=%ld\n", (long)s.codexProcessCount); printf("memory_pressure=%ld\n", (long)s.memoryPressureLevel); printf("swap_gib=%.3f\n", s.swapUsedGiB); printf("thermal=%ld\n", (long)s.thermalLevel);
    return s.codexProcessCount > 0 && s.totalMemoryGiB > 0 && s.systemMemoryUsedGiB > 0 && s.systemMemoryUsedGiB <= s.totalMemoryGiB ? 0 : 2;
}

static int RunCodexDiagnostic(void) {
    CodexStatusProvider *provider = [CodexStatusProvider new];
    [provider start];
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:10.0];
    while (!(provider.snapshot.quotaAvailable && provider.snapshot.accountAvailable && provider.snapshot.usageAvailable && provider.snapshot.activityUpdatedAt > 0 && provider.snapshot.recentTasksUpdatedAt > 0) && deadline.timeIntervalSinceNow > 0) {
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
    }
    CodexStatusSnapshot *s = provider.snapshot;
    printf("quota_available=%s\n", s.quotaAvailable ? "true" : "false");
    printf("ordinary_usage_allowed=%s\n", s.ordinaryUsageAllowed ? (s.ordinaryUsageAllowed.boolValue ? "true" : "false") : "unavailable");
    if (s.quotaErrorText.length > 0) printf("quota_error=%s\n", s.quotaErrorText.UTF8String);
    printf("five_hour_available=%s\n", s.fiveHourAvailable ? "true" : "false");
    if (s.fiveHourAvailable) printf("five_hour_remaining=%.0f\n", s.fiveHourRemainingPercent);
    printf("weekly_available=%s\n", s.weeklyAvailable ? "true" : "false");
    if (s.weeklyAvailable) {
        printf("weekly_remaining=%.0f\n", s.weeklyRemainingPercent);
        printf("weekly_data_state=%s\n", s.weeklyDataState.UTF8String ?: "unknown");
        printf("weekly_window_minutes=%.0f\n", s.weeklyWindowDurationMins);
    }
    printf("quota_bucket_count=%ld\n", (long)s.rateLimitBuckets.count);
    printf("reset_credits_available=%s\n", s.rateLimitResetCreditsAvailable ? "true" : "false");
    if (s.rateLimitResetCreditsAvailable) printf("reset_credits_count=%ld\n", (long)s.rateLimitResetCreditsCount);
    if (s.rateLimitReachedType.length > 0) printf("rate_limit_reached_type=%s\n", s.rateLimitReachedType.UTF8String);
    printf("account_available=%s\n", s.accountAvailable ? "true" : "false");
    if (s.accountErrorText.length > 0) printf("account_error=%s\n", s.accountErrorText.UTF8String);
    if (s.accountAvailable) printf("plan_type=%s\n", s.planType.UTF8String ?: "unknown");
    printf("usage_available=%s\n", s.usageAvailable ? "true" : "false");
    if (s.usageErrorText.length > 0) printf("usage_error=%s\n", s.usageErrorText.UTF8String);
    if (s.usageAvailable) {
        printf("today_usage_available=%s\n", s.todayUsageAvailable ? "true" : "false");
        printf("usage_through_date=%s\n", s.latestUsageDate.UTF8String ?: "none");
        printf("seven_day_tokens=%lld\n", s.sevenDayTokens);
        printf("thirty_day_tokens=%lld\n", s.thirtyDayTokens);
        printf("month_to_date_tokens=%lld\n", s.monthToDateTokens);
        printf("month_forecast_tokens=%lld\n", s.monthForecastTokens);
        printf("peak_daily_tokens_available=%s\n", s.peakDailyTokensAvailable ? "true" : "false");
        if (s.peakDailyTokensAvailable) printf("peak_daily_tokens=%lld\n", s.peakDailyTokens);
        printf("longest_turn_available=%s\n", s.longestRunningTurnAvailable ? "true" : "false");
        if (s.longestRunningTurnAvailable) printf("longest_turn_seconds=%ld\n", (long)s.longestRunningTurnSec);
        printf("longest_streak_available=%s\n", s.longestStreakAvailable ? "true" : "false");
        if (s.longestStreakAvailable) printf("longest_streak_days=%ld\n", (long)s.longestStreakDays);
    }
    printf("model_quota_available=%s\n", s.modelQuotaAvailable ? "true" : "false");
    if (s.modelQuotaAvailable) printf("model_quota_name=%s\n", s.modelQuotaName.UTF8String ?: "unknown");
    printf("activity_available=%s\n", s.activityAvailable ? "true" : "false");
    if (s.activityErrorText.length > 0) printf("activity_error=%s\n", s.activityErrorText.UTF8String);
    printf("activity_partial=%s\n", s.activityPartial ? "true" : "false");
    printf("activity_unresolved_recent_tasks=%ld\n", (long)s.unresolvedRecentTaskCount);
    printf("active_task_count=%ld\n", (long)s.activeTaskCount);
    printf("longest_active_task_seconds=%ld\n", (long)s.longestActiveTaskSec);
    if (s.activeTaskNames.count > 0) printf("active_task_names=%s\n", [[s.activeTaskNames componentsJoinedByString:@" | "] UTF8String]);
    printf("recent_tasks_available=%s\n", s.recentTasksAvailable ? "true" : "false");
    if (s.recentTasksErrorText.length > 0) printf("recent_tasks_error=%s\n", s.recentTasksErrorText.UTF8String);
    printf("recent_task_count=%ld\n", (long)s.recentTaskCount);
    printf("recent_task_project_count=%ld\n", (long)s.recentTaskProjectCount);
    if (s.recentTasks.count > 0) {
        NSMutableArray<NSString *> *names = [NSMutableArray array]; for (NSDictionary *task in s.recentTasks) if ([task[@"name"] isKindOfClass:NSString.class]) [names addObject:task[@"name"]];
        printf("recent_task_names=%s\n", [[names componentsJoinedByString:@" | "] UTF8String]);
    }
    printf("live_per_task_usage_available=%s\n", s.livePerTaskUsageAvailable ? "true" : "false");
    [provider stop];
    return s.quotaAvailable && s.accountAvailable && s.usageAvailable ? 0 : 3;
}

static int RunCostDiagnostic(void) {
    CodexStatusProvider *provider = [CodexStatusProvider new];
    provider.accountDataEnabled = NO;
    provider.costHistoryEnabled = YES;
    [provider refreshCostHistory];
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:30.0];
    while (provider.snapshot.localCostUpdatedAt <= 0 && deadline.timeIntervalSinceNow > 0) {
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
    }
    CodexStatusSnapshot *s = provider.snapshot;
    printf("local_cost_available=%s\n", s.localCostAvailable ? "true" : "false");
    printf("local_cost_scan_incomplete=%s\n", s.localCostScanIncomplete ? "true" : "false");
    printf("local_today_tokens=%lld\n", s.localTodayTokens);
    printf("local_7day_tokens=%lld\n", s.localSevenDayTokens);
    printf("local_30day_tokens=%lld\n", s.localThirtyDayTokens);
    printf("local_30day_cost_usd=%.6f\n", s.localThirtyDayCostUSD);
    printf("local_month_forecast_cost_usd=%.6f\n", s.localMonthForecastCostUSD);
    printf("local_priced_token_percent=%.2f\n", s.localPricedTokenPercent);
    if (s.localTopModel.length > 0) printf("local_top_model=%s\n", s.localTopModel.UTF8String);
    if (s.localCostErrorText.length > 0) printf("local_cost_error=%s\n", s.localCostErrorText.UTF8String);
    return s.localCostUpdatedAt > 0 && s.localCostAvailable ? 0 : 12;
}

static int RunUpdateDiagnostic(void) {
    NSString *digest = @"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    NSDictionary *releaseJSON = @{
        @"tag_name": @"v1.2.3",
        @"html_url": @"https://github.com/Ryuaaa/codex-monitor-hud/releases/tag/v1.2.3",
        @"body": @"test",
        @"assets": @[@{
            @"name": @"Codex-Monitor-HUD.app.zip",
            @"browser_download_url": @"https://github.com/Ryuaaa/codex-monitor-hud/releases/download/v1.2.3/Codex-Monitor-HUD.app.zip",
            @"digest": [@"sha256:" stringByAppendingString:digest]
        }]
    };
    NSError *parseError = nil;
    HUDReleaseInfo *release = HUDReleaseInfoFromDictionary(releaseJSON, &parseError);
    NSDictionary *windowsJSON = @{
        @"tag_name": @"windows-v9.9.9", @"html_url": @"https://github.com/Ryuaaa/codex-monitor-hud/releases/tag/windows-v9.9.9",
        @"assets": @[@{ @"name": @"CodexMonitorHUD-windows-x64-9.9.9.msi", @"browser_download_url": @"https://github.com/Ryuaaa/codex-monitor-hud/releases/download/windows-v9.9.9/CodexMonitorHUD-windows-x64-9.9.9.msi", @"digest": [@"sha256:" stringByAppendingString:digest] }]
    };
    NSError *channelError = nil;
    NSMutableDictionary *taskCenterJSON = [releaseJSON mutableCopy];
    taskCenterJSON[@"tag_name"] = @"task-center-v9.9.9";
    HUDReleaseInfo *selectedMac = HUDLatestMacReleaseInfoFromArray(@[taskCenterJSON, windowsJSON, releaseJSON], &channelError);
    NSURL *file = [NSURL fileURLWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:[NSString stringWithFormat:@"hud-update-test-%@", NSUUID.UUID.UUIDString]]];
    [@"abc" writeToURL:file atomically:YES encoding:NSUTF8StringEncoding error:nil];
    NSString *actualDigest = HUDSHA256ForFile(file);
    [NSFileManager.defaultManager removeItemAtURL:file error:nil];
    BOOL versionPass = HUDCompareVersions(@"1.10.0", @"1.9.9") == NSOrderedDescending && HUDCompareVersions(@"v1.2.3", @"1.2.3") == NSOrderedSame;
    BOOL metadataPass = release && !parseError && [release.version isEqualToString:@"1.2.3"] && [release.assetDigest isEqualToString:digest];
    BOOL channelPass = selectedMac && !channelError && [selectedMac.version isEqualToString:@"1.2.3"];
    BOOL checksumPass = [actualDigest isEqualToString:digest];
    BOOL frequencyPass = fabs(HUDAutomaticUpdateCheckInterval - 86400.0) < 0.1;
    NSString *helperScript = HUDInstallHelperScript();
    BOOL launchAgentHandoffPass = [helperScript containsString:@"launchctl kickstart"] && [helperScript containsString:@"launchctl bootstrap"] && [helperScript containsString:@"/usr/bin/open"] && [helperScript containsString:@"TeamIdentifier=L8K9749GM7"] && [helperScript containsString:@"spctl --assess"];
    printf("update_version_test=%s\n", versionPass ? "pass" : "fail");
    printf("update_metadata_test=%s\n", metadataPass ? "pass" : "fail");
    printf("update_channel_test=%s\n", channelPass ? "pass" : "fail");
    printf("update_checksum_test=%s\n", checksumPass ? "pass" : "fail");
    printf("update_daily_frequency_test=%s\n", frequencyPass ? "pass" : "fail");
    printf("update_launch_agent_handoff_test=%s\n", launchAgentHandoffPass ? "pass" : "fail");
    return versionPass && metadataPass && channelPass && checksumPass && frequencyPass && launchAgentHandoffPass ? 0 : 6;
}

static int RunUpdateHandoffDiagnostic(void) {
    NSString *label = @"com.codexmonitorhud.app";
    NSString *launchAgent = [NSHomeDirectory() stringByAppendingPathComponent:[NSString stringWithFormat:@"Library/LaunchAgents/%@.plist", label]];
    if (![NSFileManager.defaultManager fileExistsAtPath:launchAgent]) {
        printf("update_launch_agent_integration_test=fail\n");
        printf("update_launch_agent_reason=launch_agent_missing\n");
        return 7;
    }
    NSURL *workURL = [NSURL fileURLWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:[@"hud-handoff-test-" stringByAppendingString:NSUUID.UUID.UUIDString]] isDirectory:YES];
    NSURL *sourceURL = [workURL URLByAppendingPathComponent:@"source/Codex Monitor HUD.app" isDirectory:YES];
    NSURL *targetURL = [workURL URLByAppendingPathComponent:@"target/Codex Monitor HUD.app" isDirectory:YES];
    NSURL *helperURL = [workURL URLByAppendingPathComponent:@"install-update.zsh"];
    NSError *error = nil;
    [NSFileManager.defaultManager createDirectoryAtURL:sourceURL.URLByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:&error];
    [NSFileManager.defaultManager createDirectoryAtURL:targetURL.URLByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:&error];
    BOOL prepared = !error && [NSFileManager.defaultManager copyItemAtURL:NSBundle.mainBundle.bundleURL toURL:sourceURL error:&error] && [NSFileManager.defaultManager copyItemAtURL:NSBundle.mainBundle.bundleURL toURL:targetURL error:&error];
    prepared = prepared && [HUDInstallHelperScript() writeToURL:helperURL atomically:YES encoding:NSUTF8StringEncoding error:&error];
    prepared = prepared && [NSFileManager.defaultManager setAttributes:@{NSFilePosixPermissions: @0700} ofItemAtPath:helperURL.path error:&error];
    if (!prepared) {
        [NSFileManager.defaultManager removeItemAtURL:workURL error:nil];
        printf("update_launch_agent_integration_test=fail\n");
        printf("update_launch_agent_reason=prepare_failed\n");
        return 7;
    }
    NSTask *helper = [NSTask new];
    helper.executableURL = helperURL;
    helper.arguments = @[@"2147483647", sourceURL.path, targetURL.path, workURL.path];
    NSError *launchError = nil;
    if (![helper launchAndReturnError:&launchError]) {
        [NSFileManager.defaultManager removeItemAtURL:workURL error:nil];
        printf("update_launch_agent_integration_test=fail\n");
        printf("update_launch_agent_reason=helper_failed\n");
        return 7;
    }
    [helper waitUntilExit];
    NSTask *status = [NSTask new];
    status.executableURL = [NSURL fileURLWithPath:@"/bin/launchctl"];
    status.arguments = @[@"print", [NSString stringWithFormat:@"gui/%u/%@", getuid(), label]];
    NSPipe *output = [NSPipe pipe]; status.standardOutput = output; status.standardError = [NSPipe pipe];
    BOOL statusStarted = [status launchAndReturnError:nil];
    if (statusStarted) [status waitUntilExit];
    NSData *data = statusStarted ? [output.fileHandleForReading readDataToEndOfFile] : nil;
    NSString *description = data ? [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] : @"";
    BOOL pass = helper.terminationStatus == 0 && statusStarted && status.terminationStatus == 0 && [description containsString:@"state = running"];
    [NSFileManager.defaultManager removeItemAtURL:workURL error:nil];
    printf("update_launch_agent_integration_test=%s\n", pass ? "pass" : "fail");
    return pass ? 0 : 7;
}

static int RunLogicDiagnostic(void) {
    BOOL cpuTimebasePass = fabs(NativeRawCPUPercentFromAbsoluteTime(24000000, 1.0, 125, 3) - 100.0) < 0.001;
    double expectedUsedGiB = 205.0 * 4096.0 / 1073741824.0;
    BOOL memoryFormulaPass = fabs(NativeUsedMemoryGiBFromPageCounts(100, 50, 10, 40, 30, 5, 20, 4096) - expectedUsedGiB) < 0.0000001;
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
    BOOL currentPass = [usage[@"todayAvailable"] boolValue] && [usage[@"today"] longLongValue] == 100 && [usage[@"recent"] longLongValue] == 175 && [usage[@"previous"] longLongValue] == 40 && [usage[@"thirty"] longLongValue] == 215 && [usage[@"latestDate"] isEqualToString:todayKey];
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
    NSDate *activityNow = [NSDate dateWithTimeIntervalSince1970:1767225720];
    NSString *startLine = @"{\"timestamp\":\"2026-01-01T00:01:00.000Z\",\"type\":\"response_item\",\"payload\":{\"type\":\"message\",\"role\":\"user\"}}";
    NSString *finishLine = @"{\"timestamp\":\"2026-01-01T00:01:30.000Z\",\"type\":\"response_item\",\"payload\":{\"type\":\"message\",\"role\":\"assistant\",\"phase\":\"final_answer\"}}";
    NSDictionary *activeState = CodexActivityStateFromJSONLLines(@[startLine], [activityNow dateByAddingTimeInterval:-10], activityNow);
    NSDictionary *completedState = CodexActivityStateFromJSONLLines(@[startLine, finishLine], [activityNow dateByAddingTimeInterval:-10], activityNow);
    NSDictionary *staleState = CodexActivityStateFromJSONLLines(@[startLine], [activityNow dateByAddingTimeInterval:-121], activityNow);
    NSDate *fallbackStart = [activityNow dateByAddingTimeInterval:-90];
    NSDictionary *fallbackState = CodexActivityStateFromJSONLLinesWithFallback(@[], [activityNow dateByAddingTimeInterval:-10], activityNow, fallbackStart);
    NSDictionary *fallbackCompletedState = CodexActivityStateFromJSONLLinesWithFallback(@[finishLine], [activityNow dateByAddingTimeInterval:-10], activityNow, fallbackStart);
    BOOL activityPass = [activeState[@"active"] boolValue] && [activeState[@"durationSec"] integerValue] == 60 && ![completedState[@"active"] boolValue] && ![staleState[@"active"] boolValue] && [fallbackState[@"active"] boolValue] && [fallbackState[@"durationSec"] integerValue] == 90 && ![fallbackCompletedState[@"active"] boolValue];
    NSString *threadID = @"01900000-0000-7000-8000-000000000001";
    NSDictionary *normalizedThread = CodexNormalizedThreadMetadata(@{ @"id": threadID, @"name": @"测试任务", @"recencyAt": @(activityNow.timeIntervalSince1970), @"path": @"/private/tmp/old-rollout.jsonl", @"cwd": @"/private/tmp/current-project" });
    BOOL persistedCwdPass = [normalizedThread[@"cwd"] isEqualToString:@"/private/tmp/current-project"] && [normalizedThread[@"path"] isEqualToString:@"/private/tmp/old-rollout.jsonl"];
    NSString *missingRollout = [@"/private/tmp" stringByAppendingPathComponent:[NSString stringWithFormat:@"rollout-test-%@-%@.jsonl.zst", NSUUID.UUID.UUIDString, threadID]];
    NSURL *missingRoot = [NSURL fileURLWithPath:[@"/private/tmp" stringByAppendingPathComponent:NSUUID.UUID.UUIDString] isDirectory:YES];
    NSDictionary *migrationState = CodexScanRecentActivityAtRoot(@{ threadID: @{ @"name": @"迁移测试", @"recencyAt": @(activityNow.timeIntervalSince1970), @"path": missingRollout, @"cwd": @"/private/tmp/current-project" } }, activityNow, missingRoot);
    BOOL migrationFallbackPass = ![migrationState[@"available"] boolValue] && [migrationState[@"unresolvedRecent"] integerValue] == 1 && [migrationState[@"error"] containsString:@"无法可靠判断"];
    NSDate *costNow = [NSDate dateWithTimeIntervalSince1970:1770000000];
    NSString *modelLine = @"{\"timestamp\":\"2026-02-02T02:00:00Z\",\"type\":\"turn_context\",\"payload\":{\"model\":\"gpt-5.6-terra\"}}";
    NSString *tokenLine = @"{\"timestamp\":\"2026-02-02T02:01:00Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"token_count\",\"info\":{\"last_token_usage\":{\"input_tokens\":100000,\"cached_input_tokens\":20000,\"output_tokens\":10000}}}}";
    NSDictionary *parsedCost = CodexCostEventsFromJSONLLines(@[modelLine, tokenLine], costNow, 30);
    NSArray<NSDictionary<NSString *, id> *> *costEvents = parsedCost[@"events"];
    NSDictionary *costAggregate = CodexAggregateCostEvents(costEvents, costNow, NO);
    NSDictionary *shortCost = CodexCostEstimateForTokens(@"gpt-5.6-terra", 100000, 20000, 0, 10000);
    NSDictionary *longCost = CodexCostEstimateForTokens(@"gpt-5.6-sol", 300000, 100000, 0, 10000);
    NSDictionary *unknownCost = CodexCostEstimateForTokens(@"future-unknown-model", 1000, 0, 0, 100);
    NSDictionary *secondParsedCost = CodexCostEventsFromJSONLLines(@[modelLine, tokenLine], costNow, 30);
    NSArray *duplicatedCostEvents = [costEvents arrayByAddingObjectsFromArray:secondParsedCost[@"events"]];
    NSDictionary *deduplicatedAggregate = CodexAggregateCostEvents(duplicatedCostEvents, costNow, NO);
    NSString *watermarkLine1 = @"{\"timestamp\":\"2026-02-02T03:00:00Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"token_count\",\"info\":{\"last_token_usage\":{\"input_tokens\":100},\"total_token_usage\":{\"input_tokens\":100}}}}";
    NSString *watermarkLine2 = @"{\"timestamp\":\"2026-02-02T03:01:00Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"token_count\",\"info\":{\"last_token_usage\":{\"input_tokens\":100},\"total_token_usage\":{\"input_tokens\":100}}}}";
    NSString *watermarkLine3 = @"{\"timestamp\":\"2026-02-02T03:02:00Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"token_count\",\"info\":{\"last_token_usage\":{\"input_tokens\":50},\"total_token_usage\":{\"input_tokens\":50}}}}";
    NSString *watermarkLine4 = @"{\"timestamp\":\"2026-02-02T03:03:00Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"token_count\",\"info\":{\"last_token_usage\":{\"input_tokens\":70},\"total_token_usage\":{\"input_tokens\":120}}}}";
    NSDictionary *watermarkParsed = CodexCostEventsFromJSONLLines(@[modelLine, watermarkLine1, watermarkLine2, watermarkLine3, watermarkLine4], costNow, 30);
    NSDictionary *watermarkAggregate = CodexAggregateCostEvents(watermarkParsed[@"events"], costNow, NO);
    BOOL costParserPass = costEvents.count == 1 && [costAggregate[@"thirtyDayTokens"] longLongValue] == 110000;
    BOOL costPricingPass = [shortCost[@"available"] boolValue] && fabs([shortCost[@"cost"] doubleValue] - 0.284) < 0.000001 && [longCost[@"longContext"] boolValue] && fabs([longCost[@"cost"] doubleValue] - 2.55) < 0.000001 && ![unknownCost[@"available"] boolValue];
    BOOL costDedupPass = [deduplicatedAggregate[@"eventCount"] integerValue] == 1 && [deduplicatedAggregate[@"thirtyDayTokens"] longLongValue] == 110000;
    BOOL costWatermarkPass = [watermarkAggregate[@"thirtyDayTokens"] longLongValue] == 120;
    NSTimeInterval quotaNowValue = 1770000000;
    NSTimeInterval quotaReset = quotaNowValue + 4 * 3600;
    NSArray *quotaSamples = @[
        @{ @"t": @(quotaNowValue - 1800), @"f": @60, @"fr": @(quotaReset) },
        @{ @"t": @(quotaNowValue - 900), @"f": @50, @"fr": @(quotaReset) },
        @{ @"t": @(quotaNowValue), @"f": @40, @"fr": @(quotaReset) }
    ];
    NSDictionary *quotaForecast = CodexQuotaForecastFromSamples(quotaSamples, @"f", @"fr", 40, quotaReset, [NSDate dateWithTimeIntervalSince1970:quotaNowValue]);
    BOOL quotaForecastPass = [quotaForecast[@"available"] boolValue] && [quotaForecast[@"headline"] isEqualToString:@"可能提前用完"];
    NSDate *weeklyAlertNow = [NSDate dateWithTimeIntervalSince1970:quotaNowValue];
    NSTimeInterval naturalDayStart = [NSCalendar.currentCalendar startOfDayForDate:weeklyAlertNow].timeIntervalSince1970;
    NSArray *weeklyConsumptionSamples = @[
        @{ @"t": @(quotaNowValue - 24 * 3600), @"w": @85, @"wr": @(quotaReset) },
        @{ @"t": @(naturalDayStart - 60), @"w": @90, @"wr": @(quotaReset) },
        @{ @"t": @(naturalDayStart + 60), @"w": @80, @"wr": @(quotaReset) }
    ];
    NSDictionary *rollingConsumption = CodexWeeklyConsumptionFromSamples(weeklyConsumptionSamples, 70, quotaReset, @"rolling24h", weeklyAlertNow);
    NSDictionary *naturalConsumption = CodexWeeklyConsumptionFromSamples(weeklyConsumptionSamples, 70, quotaReset, @"naturalDay", weeklyAlertNow);
    NSDictionary *insufficientConsumption = CodexWeeklyConsumptionFromSamples(@[], 70, quotaReset, @"rolling24h", weeklyAlertNow);
    BOOL weeklyConsumptionPass = [rollingConsumption[@"available"] boolValue] && fabs([rollingConsumption[@"consumedPercent"] doubleValue] - 15.0) < 0.001 &&
                                 [naturalConsumption[@"available"] boolValue] && fabs([naturalConsumption[@"consumedPercent"] doubleValue] - 10.0) < 0.001 &&
                                 ![insufficientConsumption[@"available"] boolValue];
    NSArray *tokenWindowBuckets = @[
        @{ @"t": @(quotaNowValue - 4 * 3600), @"tokens": @100LL },
        @{ @"t": @(quotaNowValue - 3600), @"tokens": @200LL },
        @{ @"t": @(quotaNowValue - 30 * 3600), @"tokens": @400LL }
    ];
    NSDictionary *completeTokenWindow = CodexTokenWindowSummary(tokenWindowBuckets, quotaNowValue - 5 * 3600, quotaNowValue + 1, quotaNowValue - 6 * 3600);
    NSDictionary *partialTokenWindow = CodexTokenWindowSummary(tokenWindowBuckets, quotaNowValue - 24 * 3600, quotaNowValue + 1, quotaNowValue - 2 * 3600);
    BOOL tokenWindowPass = [completeTokenWindow[@"available"] boolValue] && [completeTokenWindow[@"complete"] boolValue] && [completeTokenWindow[@"tokens"] longLongValue] == 300 && [partialTokenWindow[@"available"] boolValue] && ![partialTokenWindow[@"complete"] boolValue] && [partialTokenWindow[@"tokens"] longLongValue] == 300;
    BOOL weeklyAlertPass = HUDWeeklyAlertEligible(YES, YES, @"live", quotaReset, quotaNowValue, YES, 15.0, 15.0) &&
                           !HUDWeeklyAlertEligible(YES, YES, @"previous", quotaReset, quotaNowValue, YES, 20.0, 15.0) &&
                           !HUDWeeklyAlertEligible(YES, YES, @"live", quotaReset, quotaNowValue, NO, 20.0, 15.0) &&
                           !HUDWeeklyAlertEligible(NO, YES, @"live", quotaReset, quotaNowValue, YES, 20.0, 15.0) &&
                           [HUDWeeklyAlertCycleKey(quotaReset) isEqualToString:HUDWeeklyAlertCycleKey(quotaReset)] &&
                           ![HUDWeeklyAlertCycleKey(quotaReset) isEqualToString:HUDWeeklyAlertCycleKey(quotaReset + 7 * 24 * 3600)];
    NSData *serviceNormalData = [@"{\"status\":{\"indicator\":\"none\"},\"components\":[{\"name\":\"Codex in ChatGPT Desktop\",\"status\":\"operational\"}]}" dataUsingEncoding:NSUTF8StringEncoding];
    NSData *serviceDegradedData = [@"{\"status\":{\"indicator\":\"minor\"},\"components\":[{\"name\":\"Codex in ChatGPT Desktop\",\"status\":\"degraded_performance\"}]}" dataUsingEncoding:NSUTF8StringEncoding];
    NSDictionary *serviceNormal = HUDOpenAIServiceStatusFromJSONData(serviceNormalData);
    NSDictionary *serviceDegraded = HUDOpenAIServiceStatusFromJSONData(serviceDegradedData);
    BOOL serviceStatusPass = [serviceNormal[@"headline"] isEqualToString:@"Codex服务正常"] && [serviceNormal[@"overallIndicator"] isEqualToString:@"none"] && [serviceDegraded[@"headline"] isEqualToString:@"Codex服务性能下降"] && [serviceDegraded[@"overallIndicator"] isEqualToString:@"minor"] && HUDOpenAIServiceStatusFromJSONData([@"{}" dataUsingEncoding:NSUTF8StringEncoding]).count == 0;
    NSURL *scanRoot = [NSURL fileURLWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:[@"codex-cost-scan-" stringByAppendingString:NSUUID.UUID.UUIDString]] isDirectory:YES];
    NSURL *scanDay = [scanRoot URLByAppendingPathComponent:@"sessions/2026/02/02" isDirectory:YES];
    NSURL *scanFile = [scanDay URLByAppendingPathComponent:@"rollout-01900000-0000-7000-8000-000000000002.jsonl"];
    NSURL *scanCache = [scanRoot URLByAppendingPathComponent:@"cache/history.json"];
    NSURL *scanStart = [scanRoot URLByAppendingPathComponent:@"cache/tracking-start.json"];
    [NSFileManager.defaultManager createDirectoryAtURL:scanDay withIntermediateDirectories:YES attributes:nil error:nil];
    NSString *largeContext = [@"x" stringByPaddingToLength:1500000 withString:@"x" startingAtIndex:0];
    NSString *largeModelLine = [NSString stringWithFormat:@"{\"timestamp\":\"2026-02-02T02:00:00Z\",\"type\":\"turn_context\",\"payload\":{\"model\":\"gpt-5.6-terra\",\"context\":\"%@\"}}", largeContext];
    NSString *initialScanText = [@[largeModelLine, tokenLine, @""] componentsJoinedByString:@"\n"];
    [initialScanText writeToURL:scanFile atomically:YES encoding:NSUTF8StringEncoding error:nil];
    NSDictionary *baselineScan = CodexScanCostHistoryAtHome(scanRoot, scanCache, scanStart, costNow);
    NSDictionary *firstScan = CodexScanCostHistoryAtHome(scanRoot, scanCache, scanStart, costNow);
    NSString *secondTokenLine = @"{\"timestamp\":\"2026-02-02T02:41:00Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"token_count\",\"info\":{\"last_token_usage\":{\"input_tokens\":200000,\"cached_input_tokens\":20000,\"output_tokens\":10000}}}}";
    NSFileHandle *scanHandle = [NSFileHandle fileHandleForWritingToURL:scanFile error:nil];
    [scanHandle seekToEndOfFile];
    [scanHandle writeData:[[secondTokenLine stringByAppendingString:@"\n"] dataUsingEncoding:NSUTF8StringEncoding]];
    [scanHandle closeFile];
    NSDictionary *secondScan = CodexScanCostHistoryAtHome(scanRoot, scanCache, scanStart, costNow);
    scanHandle = [NSFileHandle fileHandleForWritingToURL:scanFile error:nil];
    [scanHandle seekToEndOfFile];
    [scanHandle writeData:[@"{\"timestamp\":\"2026-02-02T02:42:00Z\"" dataUsingEncoding:NSUTF8StringEncoding]];
    [scanHandle closeFile];
    NSDictionary *partialLineScan = CodexScanCostHistoryAtHome(scanRoot, scanCache, scanStart, costNow);
    NSData *compactedCacheData = [NSData dataWithContentsOfURL:scanCache];
    NSDictionary *compactedCache = compactedCacheData ? [NSJSONSerialization JSONObjectWithData:compactedCacheData options:0 error:nil] : nil;
    NSDictionary *compactedEntry = [[compactedCache[@"files"] allValues] firstObject];
    NSString *compactedFileKey = [[compactedCache[@"files"] allKeys] firstObject];
    NSDictionary *compactedEvent = [compactedEntry[@"events"] firstObject];
    BOOL cacheCompactionPass = [compactedCache[@"version"] integerValue] == 5 && compactedFileKey.length == 64 && [compactedEntry[@"events"] count] == 1 && compactedEntry[@"state"][@"occurrences"] == nil && [compactedEvent[@"t"] doubleValue] > 0 && fabs([compactedEvent[@"x"] doubleValue] - 0.484) < 0.000001;
    BOOL incrementalScanPass = ![baselineScan[@"available"] boolValue] && [firstScan[@"thirtyDayTokens"] longLongValue] == 0 && [secondScan[@"thirtyDayTokens"] longLongValue] == 210000 && fabs([secondScan[@"thirtyDayCost"] doubleValue] - 0.484) < 0.000001 && [secondScan[@"eventCount"] integerValue] == 1 && ![partialLineScan[@"scanIncomplete"] boolValue] && [partialLineScan[@"thirtyDayTokens"] longLongValue] == 210000;
    NSMutableDictionary *legacyCache = compactedCacheData ? [NSJSONSerialization JSONObjectWithData:compactedCacheData options:NSJSONReadingMutableContainers error:nil] : nil;
    legacyCache[@"version"] = @4;
    [legacyCache removeObjectForKey:@"tokenBucketsStartedAt"];
    for (NSMutableDictionary *entry in [legacyCache[@"files"] allValues]) {
        for (NSMutableDictionary *event in entry[@"events"]) [event removeObjectForKey:@"t"];
    }
    NSData *legacyData = legacyCache ? [NSJSONSerialization dataWithJSONObject:legacyCache options:0 error:nil] : nil;
    [legacyData writeToURL:scanCache atomically:YES];
    NSString *thirdTokenLine = @"{\"timestamp\":\"2026-02-02T02:43:00Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"token_count\",\"info\":{\"last_token_usage\":{\"input_tokens\":100000,\"cached_input_tokens\":0,\"output_tokens\":10000}}}}";
    scanHandle = [NSFileHandle fileHandleForWritingToURL:scanFile error:nil];
    [scanHandle seekToEndOfFile];
    [scanHandle writeData:[[NSString stringWithFormat:@"\n%@\n", thirdTokenLine] dataUsingEncoding:NSUTF8StringEncoding]];
    [scanHandle closeFile];
    NSDictionary *migratedScan = CodexScanCostHistoryAtHome(scanRoot, scanCache, scanStart, costNow);
    NSDictionary *migratedCache = [NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfURL:scanCache] options:0 error:nil];
    long long migratedWindowTokens = 0;
    for (NSDictionary *bucket in migratedScan[@"tokenBuckets"] ?: @[]) migratedWindowTokens += [bucket[@"tokens"] longLongValue];
    BOOL cacheMigrationPass = [migratedCache[@"version"] integerValue] == 5 &&
                              [migratedScan[@"thirtyDayTokens"] longLongValue] == 320000 &&
                              [migratedScan[@"tokenBucketsStartedAt"] doubleValue] == costNow.timeIntervalSince1970 &&
                              migratedWindowTokens == 110000;
    [NSFileManager.defaultManager removeItemAtURL:scanRoot error:nil];
    BOOL calendarPass = currentPass && delayedPass && emptyPass;
    BOOL pass = calendarPass && cpuTimebasePass && memoryFormulaPass && activityPass && persistedCwdPass && migrationFallbackPass && costParserPass && costPricingPass && costDedupPass && costWatermarkPass && quotaForecastPass && weeklyConsumptionPass && tokenWindowPass && weeklyAlertPass && serviceStatusPass && incrementalScanPass && cacheCompactionPass && cacheMigrationPass;
    printf("calendar_usage_test=%s\n", calendarPass ? "pass" : "fail");
    printf("cpu_timebase_test=%s\n", cpuTimebasePass ? "pass" : "fail");
    printf("memory_used_formula_test=%s\n", memoryFormulaPass ? "pass" : "fail");
    printf("codex_activity_inference_test=%s\n", activityPass ? "pass" : "fail");
    printf("codex_persisted_cwd_test=%s\n", persistedCwdPass ? "pass" : "fail");
    printf("codex_rollout_migration_fallback_test=%s\n", migrationFallbackPass ? "pass" : "fail");
    printf("codex_cost_parser_test=%s\n", costParserPass ? "pass" : "fail");
    printf("codex_cost_pricing_test=%s\n", costPricingPass ? "pass" : "fail");
    printf("codex_cost_dedup_test=%s\n", costDedupPass ? "pass" : "fail");
    printf("codex_cost_watermark_test=%s\n", costWatermarkPass ? "pass" : "fail");
    printf("codex_quota_forecast_test=%s\n", quotaForecastPass ? "pass" : "fail");
    printf("codex_weekly_consumption_test=%s\n", weeklyConsumptionPass ? "pass" : "fail");
    printf("codex_token_window_test=%s\n", tokenWindowPass ? "pass" : "fail");
    printf("codex_weekly_alert_test=%s\n", weeklyAlertPass ? "pass" : "fail");
    printf("openai_service_status_parser_test=%s\n", serviceStatusPass ? "pass" : "fail");
    printf("codex_cost_incremental_scan_test=%s\n", incrementalScanPass ? "pass" : "fail");
    printf("codex_cost_cache_compaction_test=%s\n", cacheCompactionPass ? "pass" : "fail");
    printf("codex_cost_cache_v4_migration_test=%s\n", cacheMigrationPass ? "pass" : "fail");
    return pass ? 0 : 4;
}

static int RunUISnapshot(void) {
    [NSApplication sharedApplication];
    AppDelegate *delegate = [AppDelegate new];
    delegate.homeShowFiveHour = YES; delegate.homeShowWeekly = YES; delegate.homeShowUsage = YES; delegate.homeShowTaskActivity = YES;
    delegate.showFiveHourQuota = YES; delegate.showWeeklyQuota = YES; delegate.showUsage = YES; delegate.showTaskActivity = YES; delegate.showRecentTasks = YES;
    delegate.homeShowLocalCost = NO; delegate.homeShowTokenWindows = NO; delegate.homeShowQuotaForecast = NO; delegate.homeShowServiceStatus = NO; delegate.showLocalCost = YES; delegate.showTokenWindows = YES; delegate.showQuotaForecast = YES; delegate.showServiceStatus = NO;
    delegate.homeCodexModuleOrder = HUDDefaultHomeCodexOrder(); delegate.homeComputerModuleOrder = HUDDefaultHomeComputerOrder();
    NSView *view = [delegate settingsContentView]; view.frame = NSMakeRect(0, 0, 840, 720); [view layoutSubtreeIfNeeded];
    NSBitmapImageRep *bitmap = [view bitmapImageRepForCachingDisplayInRect:view.bounds];
    if (!bitmap) return 8;
    [view cacheDisplayInRect:view.bounds toBitmapImageRep:bitmap];
    NSData *png = [bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    NSString *path = @"/private/tmp/codex-monitor-hud-settings.png";
    BOOL written = [png writeToFile:path atomically:YES];
    HUDView *hud = [[HUDView alloc] initWithFrame:NSMakeRect(0, 0, 430, 684)]; hud.appearance = [NSAppearance appearanceNamed:NSAppearanceNameVibrantDark];
    [hud setPage:1]; [hud setCompact:YES]; [hud setTaskActivityVisible:YES]; [hud setRecentTasksVisible:YES]; [hud setLongestTurnVisible:NO]; [hud setLongestStreakVisible:NO]; [hud setPeakDailyTokensVisible:NO]; [hud setModelQuotaVisible:NO]; [hud setTokenWindowsVisible:YES]; [hud setLocalCostVisible:YES]; [hud setQuotaForecastVisible:YES]; [hud setServiceStatusVisible:NO]; [hud setQuotaDetailsVisible:NO];
    hud.codexStatusLabel.stringValue = @"● Pro · Codex数据正常";
    hud.taskActivityCard.valueLabel.stringValue = @"2个活跃 · 最长8分钟"; hud.taskActivityCard.subtitleLabel.stringValue = @"任务活动为本机趋势推测";
    [hud.recentTasksCard updateRows:@[@"1  电脑监控工具 · 刚刚", @"2  GitHub发布准备 · 12分钟前", @"3  Mac选购研究 · 1小时前"] footer:@"官方任务历史 · 不代表正在运行"];
    [hud.fiveHourCard showAvailable:YES remaining:62 reset:@"2小时后恢复" accent:NSColor.systemGreenColor];
    [hud.weeklyCard showAvailable:YES remaining:14 reset:@"3天后恢复" accent:NSColor.systemGreenColor];
    hud.planCard.valueLabel.stringValue = @"Pro"; hud.planCard.subtitleLabel.stringValue = @"不显示邮箱 · 刚刚";
    hud.usageCard.valueLabel.stringValue = @"8/5 4.8B"; hud.usageCard.subtitleLabel.stringValue = @"今日数据未返回 · 7天7.6B";
    hud.fiveHourTokensCard.valueLabel.stringValue = @"860K"; hud.fiveHourTokensCard.subtitleLabel.stringValue = @"占当前周2.6% · 官方额度已用32%";
    hud.rollingDayTokensCard.valueLabel.stringValue = @"4.92M"; hud.rollingDayTokensCard.subtitleLabel.stringValue = @"占当前周14.6% · 滚动24小时";
    hud.weeklyTokensCard.valueLabel.stringValue = @"33.7M"; hud.weeklyTokensCard.subtitleLabel.stringValue = @"当前周基准100% · 官方额度已用58%";
    hud.localCostCard.valueLabel.stringValue = @"安装后近30天 4.56B · $3,374.59"; hud.localCostCard.subtitleLabel.stringValue = @"今日 2.90B · 近7天 3.86B · 本月趋势 $11,480.57（估算）";
    hud.quotaForecastCard.valueLabel.stringValue = @"每周 可撑到重置"; hud.quotaForecastCard.subtitleLabel.stringValue = @"预计重置时剩余12% · 中可信";
    hud.codexFreshnessLabel.stringValue = @"额度 刚刚 · 用量 刚刚 · 本机Token 刚刚";
    [hud layoutSubtreeIfNeeded];
    NSBitmapImageRep *hudBitmap = [hud bitmapImageRepForCachingDisplayInRect:hud.bounds]; [hud cacheDisplayInRect:hud.bounds toBitmapImageRep:hudBitmap];
    NSData *hudPNG = [hudBitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    NSString *hudPath = @"/private/tmp/codex-monitor-hud-codex-page.png"; BOOL hudWritten = [hudPNG writeToFile:hudPath atomically:YES];
    printf("ui_snapshot=%s\n", written ? path.UTF8String : "failed");
    printf("hud_snapshot=%s\n", hudWritten ? hudPath.UTF8String : "failed");
    return written && hudWritten ? 0 : 8;
}

static int RunSingletonLockProbe(NSString *path) {
    int lockFD = -1;
    HUDSingletonLockResult result = HUDTryAcquireSingletonLockAtPath(path, &lockFD);
    if (lockFD >= 0) close(lockFD);
    if (result == HUDSingletonLockResultAcquired) return 0;
    if (result == HUDSingletonLockResultAlreadyRunning) return 10;
    return 11;
}

static int HUDRunSingletonProbeProcess(NSString *path) {
    NSTask *task = [NSTask new];
    task.executableURL = [NSURL fileURLWithPath:NSBundle.mainBundle.executablePath];
    task.arguments = @[@"--singleton-lock-probe", path];
    task.standardOutput = [NSFileHandle fileHandleWithNullDevice];
    task.standardError = [NSFileHandle fileHandleWithNullDevice];
    if (![task launchAndReturnError:nil]) return -1;
    [task waitUntilExit];
    return task.terminationStatus;
}

static int RunSingletonDiagnostic(void) {
    NSString *path = [NSTemporaryDirectory() stringByAppendingPathComponent:[NSString stringWithFormat:@"codex-monitor-hud-singleton-%@.lock", NSUUID.UUID.UUIDString]];
    int parentLockFD = -1;
    HUDSingletonLockResult acquired = HUDTryAcquireSingletonLockAtPath(path, &parentLockFD);
    int blockedStatus = acquired == HUDSingletonLockResultAcquired ? HUDRunSingletonProbeProcess(path) : -1;
    if (parentLockFD >= 0) close(parentLockFD);
    int releasedStatus = HUDRunSingletonProbeProcess(path);
    [NSFileManager.defaultManager removeItemAtPath:path error:nil];
    BOOL pass = acquired == HUDSingletonLockResultAcquired && blockedStatus == 10 && releasedStatus == 0;
    printf("single_instance_lock_test=%s\n", pass ? "pass" : "fail");
    return pass ? 0 : 9;
}

static int RunServiceStatusDiagnostic(void) {
    HUDOpenAIServiceStatusProvider *provider = [HUDOpenAIServiceStatusProvider new];
    __block BOOL done = NO;
    provider.updateHandler = ^{ done = YES; };
    [provider refresh];
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:10.0];
    while (!done && deadline.timeIntervalSinceNow > 0) {
        [NSRunLoop.currentRunLoop runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
    }
    HUDOpenAIServiceStatusSnapshot *snapshot = provider.snapshot;
    [provider stop];
    printf("openai_service_status_available=%s\n", snapshot.available ? "true" : "false");
    if (snapshot.headline.length > 0) printf("openai_service_status=%s\n", snapshot.headline.UTF8String);
    if (snapshot.detail.length > 0) printf("openai_service_detail=%s\n", snapshot.detail.UTF8String);
    if (snapshot.errorText.length > 0) printf("openai_service_error=%s\n", snapshot.errorText.UTF8String);
    return snapshot.available ? 0 : 13;
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        if (argc > 2 && strcmp(argv[1], "--singleton-lock-probe") == 0) return RunSingletonLockProbe([NSString stringWithUTF8String:argv[2]]);
        if (argc > 1 && strcmp(argv[1], "--singleton-diagnostic") == 0) return RunSingletonDiagnostic();
        if (argc > 1 && strcmp(argv[1], "--diagnostic") == 0) return RunDiagnostic();
        if (argc > 1 && strcmp(argv[1], "--codex-diagnostic") == 0) return RunCodexDiagnostic();
        if (argc > 1 && strcmp(argv[1], "--cost-diagnostic") == 0) return RunCostDiagnostic();
        if (argc > 1 && strcmp(argv[1], "--logic-diagnostic") == 0) return RunLogicDiagnostic();
        if (argc > 1 && strcmp(argv[1], "--ui-diagnostic") == 0) {
            @try { return RunUIDiagnostic(); }
            @catch (NSException *exception) { fprintf(stderr, "ui_diagnostic_exception=%s\n", exception.reason.UTF8String ?: "unknown"); return 14; }
        }
        if (argc > 1 && strcmp(argv[1], "--update-diagnostic") == 0) return RunUpdateDiagnostic();
        if (argc > 1 && strcmp(argv[1], "--update-handoff-diagnostic") == 0) return RunUpdateHandoffDiagnostic();
        if (argc > 1 && strcmp(argv[1], "--service-status-diagnostic") == 0) return RunServiceStatusDiagnostic();
        if (argc > 1 && strcmp(argv[1], "--ui-snapshot") == 0) {
            @try { return RunUISnapshot(); }
            @catch (NSException *exception) { fprintf(stderr, "ui_snapshot_exception=%s\n", exception.reason.UTF8String ?: "unknown"); return 14; }
        }
        if (!HUDAcquireApplicationSingletonLock()) return 0;
        NSApplication *application = NSApplication.sharedApplication; AppDelegate *delegate = [AppDelegate new]; application.delegate = delegate; [application run];
    }
    return 0;
}
