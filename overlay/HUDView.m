#import "HUDView.h"

@implementation HUDProgressView
- (instancetype)initWithFrame:(NSRect)frameRect {
    self = [super initWithFrame:frameRect];
    if (self) { _progress = 0; _accentColor = NSColor.systemGreenColor; self.wantsLayer = YES; }
    return self;
}
- (NSSize)intrinsicContentSize { return NSMakeSize(NSViewNoIntrinsicMetric, 4); }
- (void)setProgress:(CGFloat)progress { _progress = MAX(0, MIN(1, progress)); self.needsDisplay = YES; }
- (void)setAccentColor:(NSColor *)accentColor { _accentColor = accentColor ?: NSColor.systemGreenColor; self.needsDisplay = YES; }
- (void)drawRect:(NSRect)dirtyRect {
    NSRect bounds = NSInsetRect(self.bounds, 0, 0.5);
    NSBezierPath *track = [NSBezierPath bezierPathWithRoundedRect:bounds xRadius:2 yRadius:2];
    [[NSColor colorWithWhite:1 alpha:0.08] setFill]; [track fill];
    if (self.progress <= 0) return;
    NSRect fillRect = bounds; fillRect.size.width *= self.progress;
    NSBezierPath *fill = [NSBezierPath bezierPathWithRoundedRect:fillRect xRadius:2 yRadius:2];
    [self.accentColor setFill]; [fill fill];
}
@end

@implementation HUDQuotaCard
- (NSTextField *)text:(NSString *)text size:(CGFloat)size color:(NSColor *)color weight:(NSFontWeight)weight {
    NSTextField *label = [NSTextField labelWithString:text];
    label.font = [NSFont monospacedDigitSystemFontOfSize:size weight:weight];
    label.textColor = color; label.lineBreakMode = NSLineBreakByTruncatingTail; label.maximumNumberOfLines = 1;
    return label;
}
- (instancetype)initWithTitle:(NSString *)title {
    self = [super initWithFrame:NSZeroRect];
    if (!self) return nil;
    self.boxType = NSBoxCustom; self.titlePosition = NSNoTitle;
    self.fillColor = [NSColor colorWithWhite:1 alpha:0.038];
    self.borderColor = [NSColor colorWithWhite:1 alpha:0.08];
    self.borderWidth = 1; self.cornerRadius = 10; self.contentViewMargins = NSMakeSize(10, 8);
    _titleLabel = [self text:title size:11 color:NSColor.secondaryLabelColor weight:NSFontWeightMedium];
    _windowLabel = [self text:@"剩余" size:10.5 color:NSColor.tertiaryLabelColor weight:NSFontWeightRegular];
    NSStackView *head = [NSStackView stackViewWithViews:@[_titleLabel, _windowLabel]];
    head.orientation = NSUserInterfaceLayoutOrientationHorizontal; head.distribution = NSStackViewDistributionFill;
    [_titleLabel setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_windowLabel setContentHuggingPriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationHorizontal];
    _valueLabel = [self text:@"--" size:19 color:NSColor.systemGreenColor weight:NSFontWeightBold];
    _resetLabel = [self text:@"等待接口" size:10.5 color:NSColor.tertiaryLabelColor weight:NSFontWeightRegular];
    _progressView = [HUDProgressView new];
    NSStackView *stack = [NSStackView stackViewWithViews:@[head, _valueLabel, _resetLabel, _progressView]];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical; stack.alignment = NSLayoutAttributeLeading; stack.spacing = 3;
    stack.translatesAutoresizingMaskIntoConstraints = NO; [self.contentView addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[
        [stack.leadingAnchor constraintEqualToAnchor:self.contentView.leadingAnchor], [stack.trailingAnchor constraintEqualToAnchor:self.contentView.trailingAnchor],
        [stack.topAnchor constraintEqualToAnchor:self.contentView.topAnchor], [stack.bottomAnchor constraintEqualToAnchor:self.contentView.bottomAnchor],
        [head.widthAnchor constraintEqualToAnchor:stack.widthAnchor], [_progressView.widthAnchor constraintEqualToAnchor:stack.widthAnchor]
    ]];
    return self;
}
- (void)showAvailable:(BOOL)available remaining:(double)remaining reset:(NSString *)reset accent:(NSColor *)accent {
    self.windowLabel.stringValue = available ? @"剩余" : @"等待接口";
    self.valueLabel.stringValue = available ? [NSString stringWithFormat:@"%.0f%%", remaining] : @"当前未返回";
    self.valueLabel.font = [NSFont monospacedDigitSystemFontOfSize:(available ? 19 : 14) weight:NSFontWeightBold];
    self.valueLabel.textColor = available ? accent : NSColor.tertiaryLabelColor;
    self.resetLabel.stringValue = available ? reset : @"已预留显示位置";
    self.progressView.accentColor = accent; self.progressView.progress = available ? remaining / 100.0 : 0;
}
@end

@implementation HUDMetricCard
- (instancetype)initWithTitle:(NSString *)title value:(NSString *)value subtitle:(NSString *)subtitle {
    self = [super initWithFrame:NSZeroRect];
    if (!self) return nil;
    self.boxType = NSBoxCustom; self.titlePosition = NSNoTitle;
    self.fillColor = [NSColor colorWithWhite:1 alpha:0.032]; self.borderColor = [NSColor colorWithWhite:1 alpha:0.075];
    self.borderWidth = 1; self.cornerRadius = 9; self.contentViewMargins = NSMakeSize(9, 7);
    _titleLabel = [NSTextField labelWithString:title]; _titleLabel.font = [NSFont systemFontOfSize:10.5 weight:NSFontWeightMedium]; _titleLabel.textColor = NSColor.tertiaryLabelColor;
    _valueLabel = [NSTextField labelWithString:value]; _valueLabel.font = [NSFont monospacedDigitSystemFontOfSize:14 weight:NSFontWeightSemibold]; _valueLabel.textColor = NSColor.labelColor;
    _subtitleLabel = [NSTextField labelWithString:subtitle ?: @""]; _subtitleLabel.font = [NSFont systemFontOfSize:10]; _subtitleLabel.textColor = NSColor.tertiaryLabelColor;
    for (NSTextField *label in @[_titleLabel, _valueLabel, _subtitleLabel]) {
        label.lineBreakMode = NSLineBreakByTruncatingTail;
        label.maximumNumberOfLines = 1;
        [label setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
        [label setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    }
    NSStackView *stack = [NSStackView stackViewWithViews:@[_titleLabel, _valueLabel, _subtitleLabel]];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical; stack.alignment = NSLayoutAttributeLeading; stack.spacing = 2;
    stack.translatesAutoresizingMaskIntoConstraints = NO; [self.contentView addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[
        [stack.leadingAnchor constraintEqualToAnchor:self.contentView.leadingAnchor], [stack.trailingAnchor constraintEqualToAnchor:self.contentView.trailingAnchor],
        [stack.topAnchor constraintEqualToAnchor:self.contentView.topAnchor], [stack.bottomAnchor constraintEqualToAnchor:self.contentView.bottomAnchor]
    ]];
    return self;
}
@end

@interface HUDMemoryListCard ()
@property(nonatomic, strong) NSArray<NSTextField *> *rows;
@end

@implementation HUDMemoryListCard
- (instancetype)init {
    self = [super initWithFrame:NSZeroRect];
    if (!self) return nil;
    self.boxType = NSBoxCustom; self.titlePosition = NSNoTitle;
    self.fillColor = [NSColor colorWithWhite:1 alpha:0.032]; self.borderColor = [NSColor colorWithWhite:1 alpha:0.075];
    self.borderWidth = 1; self.cornerRadius = 9; self.contentViewMargins = NSMakeSize(9, 7);
    NSTextField *title = [NSTextField labelWithString:@"内存占用最高的软件"];
    title.font = [NSFont systemFontOfSize:10.5 weight:NSFontWeightMedium]; title.textColor = NSColor.tertiaryLabelColor;
    NSMutableArray<NSTextField *> *rows = [NSMutableArray array];
    for (NSInteger index = 0; index < 5; index++) {
        NSTextField *row = [NSTextField labelWithString:@"--"];
        row.font = [NSFont monospacedDigitSystemFontOfSize:11 weight:NSFontWeightMedium]; row.textColor = NSColor.labelColor;
        row.lineBreakMode = NSLineBreakByTruncatingMiddle; row.maximumNumberOfLines = 1;
        [rows addObject:row];
    }
    _rows = rows;
    NSMutableArray<NSView *> *views = [NSMutableArray arrayWithObject:title]; [views addObjectsFromArray:rows];
    NSStackView *stack = [NSStackView stackViewWithViews:views];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical; stack.alignment = NSLayoutAttributeLeading; stack.spacing = 3;
    stack.translatesAutoresizingMaskIntoConstraints = NO; [self.contentView addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[
        [stack.leadingAnchor constraintEqualToAnchor:self.contentView.leadingAnchor], [stack.trailingAnchor constraintEqualToAnchor:self.contentView.trailingAnchor],
        [stack.topAnchor constraintEqualToAnchor:self.contentView.topAnchor], [stack.bottomAnchor constraintEqualToAnchor:self.contentView.bottomAnchor]
    ]];
    return self;
}
- (void)updateApps:(NSArray<NativeTopApp *> *)apps totalMemoryGiB:(double)totalMemoryGiB {
    for (NSUInteger index = 0; index < self.rows.count; index++) {
        NSTextField *row = self.rows[index];
        if (index < apps.count) {
            NativeTopApp *app = apps[index];
            double percent = totalMemoryGiB > 0 ? app.memoryGiB / totalMemoryGiB * 100.0 : 0;
            row.stringValue = [NSString stringWithFormat:@"%lu  %@  %.1fG · %.0f%%", (unsigned long)(index + 1), app.name, app.memoryGiB, percent];
        } else row.stringValue = @"--";
    }
}
@end

@interface HUDRecentTasksCard ()
@property(nonatomic, strong) NSArray<NSTextField *> *rows;
@property(nonatomic, strong) NSTextField *footerLabel;
@end

@implementation HUDRecentTasksCard
- (instancetype)init {
    self = [super initWithFrame:NSZeroRect];
    if (!self) return nil;
    self.boxType = NSBoxCustom; self.titlePosition = NSNoTitle;
    self.fillColor = [NSColor colorWithWhite:1 alpha:0.032]; self.borderColor = [NSColor colorWithWhite:1 alpha:0.075];
    self.borderWidth = 1; self.cornerRadius = 9; self.contentViewMargins = NSMakeSize(9, 7);
    NSTextField *title = [NSTextField labelWithString:@"最近任务（历史记录）"];
    title.font = [NSFont systemFontOfSize:10.5 weight:NSFontWeightMedium]; title.textColor = NSColor.tertiaryLabelColor;
    NSMutableArray<NSTextField *> *rows = [NSMutableArray array];
    for (NSInteger index = 0; index < 3; index++) {
        NSTextField *row = [NSTextField labelWithString:@"--"];
        row.font = [NSFont systemFontOfSize:11 weight:NSFontWeightMedium]; row.textColor = NSColor.labelColor;
        row.lineBreakMode = NSLineBreakByTruncatingMiddle; row.maximumNumberOfLines = 1;
        [rows addObject:row];
    }
    _rows = rows;
    _footerLabel = [NSTextField labelWithString:@"官方任务列表 · 不代表正在运行"];
    _footerLabel.font = [NSFont systemFontOfSize:9.5]; _footerLabel.textColor = NSColor.tertiaryLabelColor;
    NSMutableArray<NSView *> *views = [NSMutableArray arrayWithObject:title]; [views addObjectsFromArray:rows]; [views addObject:_footerLabel];
    NSStackView *stack = [NSStackView stackViewWithViews:views];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical; stack.alignment = NSLayoutAttributeLeading; stack.spacing = 3;
    stack.translatesAutoresizingMaskIntoConstraints = NO; [self.contentView addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[
        [stack.leadingAnchor constraintEqualToAnchor:self.contentView.leadingAnchor], [stack.trailingAnchor constraintEqualToAnchor:self.contentView.trailingAnchor],
        [stack.topAnchor constraintEqualToAnchor:self.contentView.topAnchor], [stack.bottomAnchor constraintEqualToAnchor:self.contentView.bottomAnchor]
    ]];
    return self;
}
- (void)updateRows:(NSArray<NSString *> *)rows footer:(NSString *)footer {
    for (NSUInteger index = 0; index < self.rows.count; index++) {
        self.rows[index].stringValue = index < rows.count ? rows[index] : (index == 0 ? @"暂无任务记录" : @"");
    }
    self.footerLabel.stringValue = footer.length > 0 ? footer : @"官方任务列表 · 不代表正在运行";
}
@end

@implementation HUDSparklineView
- (instancetype)initWithFrame:(NSRect)frameRect { self = [super initWithFrame:frameRect]; if (self) { _values = @[]; _accentColor = NSColor.systemGreenColor; } return self; }
- (NSSize)intrinsicContentSize { return NSMakeSize(68, 22); }
- (void)setValues:(NSArray<NSNumber *> *)values { _values = [values copy] ?: @[]; self.needsDisplay = YES; }
- (void)setAccentColor:(NSColor *)accentColor { _accentColor = accentColor ?: NSColor.systemGreenColor; self.needsDisplay = YES; }
- (void)drawRect:(NSRect)dirtyRect {
    if (self.values.count == 0) return;
    NSUInteger visibleCount = MIN((NSUInteger)18, self.values.count); CGFloat gap = 2;
    CGFloat barWidth = MAX(2, (self.bounds.size.width - gap * (visibleCount - 1)) / visibleCount);
    NSUInteger start = self.values.count - visibleCount;
    for (NSUInteger index = 0; index < visibleCount; index++) {
        CGFloat value = MAX(0.05, MIN(1, self.values[start + index].doubleValue / 100.0));
        NSRect bar = NSMakeRect(index * (barWidth + gap), 0, barWidth, self.bounds.size.height * value);
        NSBezierPath *path = [NSBezierPath bezierPathWithRoundedRect:bar xRadius:1.5 yRadius:1.5];
        [[self.accentColor colorWithAlphaComponent:0.78] setFill]; [path fill];
    }
}
@end

@interface HUDTintView : NSView
@property(nonatomic) CGFloat opacity;
@end

@implementation HUDTintView
- (NSView *)hitTest:(NSPoint)point { return nil; }
- (void)setOpacity:(CGFloat)opacity { _opacity = opacity; self.needsDisplay = YES; }
- (void)drawRect:(NSRect)dirtyRect {
    [[NSColor colorWithRed:23.0/255.0 green:28.0/255.0 blue:34.0/255.0 alpha:self.opacity] setFill];
    NSRectFillUsingOperation(self.bounds, NSCompositingOperationSourceOver);
    NSGradient *highlight = [[NSGradient alloc] initWithStartingColor:[NSColor colorWithWhite:1 alpha:0.07] endingColor:[NSColor colorWithWhite:1 alpha:0.0]];
    [highlight drawInRect:self.bounds angle:-35];
}
@end

@interface HUDView ()
@property(nonatomic, strong) HUDTintView *tintView;
@property(nonatomic, strong) NSStackView *diagnosisRow;
@property(nonatomic, strong) NSStackView *trendRow;
@property(nonatomic) BOOL compactMode;
@end

@implementation HUDView

- (NSTextField *)label:(NSString *)text size:(CGFloat)size color:(NSColor *)color {
    NSTextField *label = [NSTextField labelWithString:text];
    label.font = [NSFont monospacedDigitSystemFontOfSize:size weight:NSFontWeightRegular];
    label.textColor = color;
    label.lineBreakMode = NSLineBreakByTruncatingTail;
    label.maximumNumberOfLines = 1;
    return label;
}

- (instancetype)initWithFrame:(NSRect)frameRect {
    self = [super initWithFrame:frameRect];
    if (!self) return nil;
    self.material = NSVisualEffectMaterialHUDWindow;
    self.blendingMode = NSVisualEffectBlendingModeBehindWindow;
    self.state = NSVisualEffectStateActive;
    self.appearance = [NSAppearance appearanceNamed:NSAppearanceNameVibrantDark];
    self.wantsLayer = YES;
    self.layer.cornerRadius = 16.0;
    self.layer.masksToBounds = YES;
    self.layer.borderWidth = 1.0;
    self.layer.borderColor = [NSColor colorWithWhite:1 alpha:0.15].CGColor;
    _layoutCanvas = [[NSView alloc] initWithFrame:self.bounds];
    _layoutCanvas.autoresizingMask = NSViewNotSizable;
    [self addSubview:_layoutCanvas];
    _tintView = [HUDTintView new];
    _tintView.translatesAutoresizingMaskIntoConstraints = NO;
    [_layoutCanvas addSubview:_tintView];
    [NSLayoutConstraint activateConstraints:@[
        [_tintView.leadingAnchor constraintEqualToAnchor:_layoutCanvas.leadingAnchor],
        [_tintView.trailingAnchor constraintEqualToAnchor:_layoutCanvas.trailingAnchor],
        [_tintView.topAnchor constraintEqualToAnchor:_layoutCanvas.topAnchor],
        [_tintView.bottomAnchor constraintEqualToAnchor:_layoutCanvas.bottomAnchor]
    ]];
    self.accentColor = NSColor.systemGreenColor;

    _tabs = [NSSegmentedControl segmentedControlWithLabels:@[@"主页", @"Codex", @"电脑性能"] trackingMode:NSSegmentSwitchTrackingSelectOne target:self action:@selector(tabSelected:)];
    _tabs.selectedSegment = 0;
    _tabs.controlSize = NSControlSizeSmall;
    _tabs.segmentStyle = NSSegmentStyleRounded;
    _tabs.font = [NSFont systemFontOfSize:11.5 weight:NSFontWeightSemibold];
    _minimizeButton = [NSButton buttonWithImage:[NSImage imageWithSystemSymbolName:@"minus" accessibilityDescription:@"最小化到程序栏"] target:self action:@selector(toggleMinimize:)];
    _minimizeButton.bezelStyle = NSBezelStyleInline; _minimizeButton.bordered = NO;
    _minimizeButton.contentTintColor = [NSColor colorWithWhite:1 alpha:0.68]; _minimizeButton.toolTip = @"最小化到程序栏";
    _pinButton = [NSButton buttonWithImage:[NSImage imageWithSystemSymbolName:@"pin.fill" accessibilityDescription:@"取消置顶"] target:self action:@selector(togglePin:)];
    _pinButton.bezelStyle = NSBezelStyleInline;
    _pinButton.bordered = NO;
    _lockButton = [NSButton buttonWithImage:[NSImage imageWithSystemSymbolName:@"lock.open" accessibilityDescription:@"锁定位置和大小"] target:self action:@selector(togglePositionLock:)];
    _lockButton.bezelStyle = NSBezelStyleInline;
    _lockButton.bordered = NO;
    _settingsButton = [NSButton buttonWithImage:[NSImage imageWithSystemSymbolName:@"gearshape" accessibilityDescription:@"设置"] target:self action:@selector(showSettings:)];
    _settingsButton.bezelStyle = NSBezelStyleInline;
    _settingsButton.bordered = NO;
    _settingsButton.contentTintColor = [NSColor colorWithWhite:1 alpha:0.76];
    _settingsButton.toolTip = @"悬浮窗设置";
    NSStackView *header = [NSStackView stackViewWithViews:@[_tabs, _minimizeButton, _pinButton, _lockButton, _settingsButton]];
    header.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    header.alignment = NSLayoutAttributeCenterY;
    header.distribution = NSStackViewDistributionFill;
    [_tabs setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_minimizeButton setContentHuggingPriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_pinButton setContentHuggingPriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_lockButton setContentHuggingPriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_settingsButton setContentHuggingPriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationHorizontal];
    [self setAlwaysOnTop:YES];
    [self setPositionLocked:NO];
    NSBox *headerSeparator = [[NSBox alloc] initWithFrame:NSZeroRect];
    headerSeparator.boxType = NSBoxCustom;
    headerSeparator.titlePosition = NSNoTitle;
    headerSeparator.fillColor = [NSColor colorWithWhite:1 alpha:0.085];
    headerSeparator.borderWidth = 0;

    _codexStatusLabel = [self label:@"● 正在连接本机Codex" size:13 color:self.accentColor];
    _codexStatusLabel.font = [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold];
    _taskActivityCard = [[HUDMetricCard alloc] initWithTitle:@"任务活动（本机推测）" value:@"正在读取" subtitle:@"活跃5秒 · 空闲20秒"];
    _recentTasksCard = [HUDRecentTasksCard new];
    _fiveHourCard = [[HUDQuotaCard alloc] initWithTitle:@"5小时额度"];
    _weeklyCard = [[HUDQuotaCard alloc] initWithTitle:@"每周额度"];
    _quotaRow = [NSStackView stackViewWithViews:@[_fiveHourCard, _weeklyCard]];
    _quotaRow.orientation = NSUserInterfaceLayoutOrientationHorizontal; _quotaRow.distribution = NSStackViewDistributionFillEqually; _quotaRow.spacing = 8;
    _planCard = [[HUDMetricCard alloc] initWithTitle:@"订阅" value:@"等待接口" subtitle:@"不显示邮箱"];
    _usageCard = [[HUDMetricCard alloc] initWithTitle:@"账户Token统计" value:@"今日 --" subtitle:@"7天 -- · 不等于额度"];
    _modelQuotaCard = [[HUDMetricCard alloc] initWithTitle:@"模型专属额度" value:@"当前未返回" subtitle:@"高级显示"];
    _codexInsightsRow = [NSStackView stackViewWithViews:@[_planCard, _usageCard, _modelQuotaCard]];
    _codexInsightsRow.orientation = NSUserInterfaceLayoutOrientationHorizontal; _codexInsightsRow.distribution = NSStackViewDistributionFillEqually; _codexInsightsRow.spacing = 7;
    _localCostCard = [[HUDMetricCard alloc] initWithTitle:@"Token用量与费用" value:@"正在读取" subtitle:@"今日 -- · 7天 -- · 本月预计 --"];
    _quotaForecastCard = [[HUDMetricCard alloc] initWithTitle:@"额度趋势预测" value:@"正在积累历史" subtitle:@"至少需要15分钟数据"];
    _serviceStatusCard = [[HUDMetricCard alloc] initWithTitle:@"OpenAI服务状态" value:@"正在检查" subtitle:@"官方状态页 · 低频刷新"];
    _longestTurnCard = [[HUDMetricCard alloc] initWithTitle:@"最长单次任务" value:@"当前未返回" subtitle:@"账户历史记录"];
    _longestStreakCard = [[HUDMetricCard alloc] initWithTitle:@"最长连续使用" value:@"当前未返回" subtitle:@"账户历史记录"];
    _usageHistoryRow = [NSStackView stackViewWithViews:@[_longestTurnCard, _longestStreakCard]];
    _usageHistoryRow.orientation = NSUserInterfaceLayoutOrientationHorizontal; _usageHistoryRow.distribution = NSStackViewDistributionFillEqually; _usageHistoryRow.spacing = 7;
    _codexFreshnessLabel = [self label:@"来源  Codex本机接口" size:10.5 color:NSColor.tertiaryLabelColor];
    _codexStack = [NSStackView stackViewWithViews:@[_codexStatusLabel, _taskActivityCard, _recentTasksCard, _quotaRow, _codexInsightsRow, _localCostCard, _quotaForecastCard, _serviceStatusCard, _usageHistoryRow, _codexFreshnessLabel]];
    _codexStack.orientation = NSUserInterfaceLayoutOrientationVertical;
    _codexStack.alignment = NSLayoutAttributeLeading;
    _codexStack.spacing = 7;

    _homeCodexStatusLabel = [self label:@"● 正在连接本机Codex" size:13 color:self.accentColor];
    _homeCodexStatusLabel.font = [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold];
    _homeTaskActivityCard = [[HUDMetricCard alloc] initWithTitle:@"任务活动（本机推测）" value:@"正在读取" subtitle:@"活跃5秒 · 空闲20秒"];
    _homeRecentTasksCard = [HUDRecentTasksCard new];
    _homeFiveHourCard = [[HUDQuotaCard alloc] initWithTitle:@"5小时额度"];
    _homeWeeklyCard = [[HUDQuotaCard alloc] initWithTitle:@"每周额度"];
    _homeQuotaRow = [NSStackView stackViewWithViews:@[_homeFiveHourCard, _homeWeeklyCard]];
    _homeQuotaRow.orientation = NSUserInterfaceLayoutOrientationHorizontal; _homeQuotaRow.distribution = NSStackViewDistributionFillEqually; _homeQuotaRow.spacing = 8;
    _homePlanCard = [[HUDMetricCard alloc] initWithTitle:@"订阅" value:@"等待接口" subtitle:@"不显示邮箱"];
    _homeUsageCard = [[HUDMetricCard alloc] initWithTitle:@"账户Token统计" value:@"今日 --" subtitle:@"7天 -- · 不等于额度"];
    _homeModelQuotaCard = [[HUDMetricCard alloc] initWithTitle:@"模型专属额度" value:@"当前未返回" subtitle:@"高级显示"];
    _homeInsightsRow = [NSStackView stackViewWithViews:@[_homePlanCard, _homeUsageCard, _homeModelQuotaCard]];
    _homeInsightsRow.orientation = NSUserInterfaceLayoutOrientationHorizontal; _homeInsightsRow.distribution = NSStackViewDistributionFillEqually; _homeInsightsRow.spacing = 7;
    _homeLocalCostCard = [[HUDMetricCard alloc] initWithTitle:@"Token用量与费用" value:@"正在读取" subtitle:@"今日 -- · 7天 -- · 本月预计 --"];
    _homeQuotaForecastCard = [[HUDMetricCard alloc] initWithTitle:@"额度趋势预测" value:@"正在积累历史" subtitle:@"至少需要15分钟数据"];
    _homeServiceStatusCard = [[HUDMetricCard alloc] initWithTitle:@"OpenAI服务状态" value:@"正在检查" subtitle:@"官方状态页 · 低频刷新"];
    _homeLongestTurnCard = [[HUDMetricCard alloc] initWithTitle:@"最长单次任务" value:@"当前未返回" subtitle:@"账户历史记录"];
    _homeLongestStreakCard = [[HUDMetricCard alloc] initWithTitle:@"最长连续使用" value:@"当前未返回" subtitle:@"账户历史记录"];
    _homeUsageHistoryRow = [NSStackView stackViewWithViews:@[_homeLongestTurnCard, _homeLongestStreakCard]];
    _homeUsageHistoryRow.orientation = NSUserInterfaceLayoutOrientationHorizontal; _homeUsageHistoryRow.distribution = NSStackViewDistributionFillEqually; _homeUsageHistoryRow.spacing = 7;
    _homeComputerStatusLabel = [self label:@"● 正在读取电脑状态" size:13 color:NSColor.systemGreenColor];
    _homeComputerStatusLabel.font = [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold];
    _homeBottleneckCard = [[HUDMetricCard alloc] initWithTitle:@"当前瓶颈" value:@"无" subtitle:@"Codex影响 --"];
    _homeSystemCard = [[HUDMetricCard alloc] initWithTitle:@"电脑状态" value:@"CPU -- · 内存 --" subtitle:@"已用 -- / --G · 压力 --"];
    _homeComputerCardsRow = [NSStackView stackViewWithViews:@[_homeBottleneckCard, _homeSystemCard]];
    _homeComputerCardsRow.orientation = NSUserInterfaceLayoutOrientationHorizontal; _homeComputerCardsRow.distribution = NSStackViewDistributionFillEqually; _homeComputerCardsRow.spacing = 7;
    _homeAttributionLabel = [self label:@"Codex：CPU --（整机） · 内存 --G（占总内存 --%）" size:12 color:NSColor.labelColor];
    _homeMemoryAppsCard = [HUDMemoryListCard new];
    _homeSparkline = [HUDSparklineView new];
    _homeTrendTitleLabel = [self label:@"CPU趋势" size:10.5 color:NSColor.secondaryLabelColor];
    _homeTrendLabel = [self label:@"平均 -- · 峰值 --" size:10.5 color:NSColor.labelColor];
    _homeTrendRow = [NSStackView stackViewWithViews:@[_homeTrendTitleLabel, _homeSparkline, _homeTrendLabel]];
    _homeTrendRow.orientation = NSUserInterfaceLayoutOrientationHorizontal; _homeTrendRow.alignment = NSLayoutAttributeCenterY; _homeTrendRow.spacing = 8;
    _homeFreshnessLabel = [self label:@"电脑5秒起 · Codex智能刷新" size:10.5 color:NSColor.tertiaryLabelColor];
    _homeStack = [NSStackView stackViewWithViews:@[_homeCodexStatusLabel, _homeTaskActivityCard, _homeRecentTasksCard, _homeQuotaRow, _homeInsightsRow, _homeLocalCostCard, _homeQuotaForecastCard, _homeServiceStatusCard, _homeUsageHistoryRow, _homeComputerStatusLabel, _homeComputerCardsRow, _homeAttributionLabel, _homeMemoryAppsCard, _homeTrendRow, _homeFreshnessLabel]];
    _homeStack.orientation = NSUserInterfaceLayoutOrientationVertical; _homeStack.alignment = NSLayoutAttributeLeading; _homeStack.spacing = 7;

    _computerStatusLabel = [self label:@"● 正在读取电脑状态" size:13 color:NSColor.systemGreenColor];
    _computerStatusLabel.font = [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold];
    _bottleneckCard = [[HUDMetricCard alloc] initWithTitle:@"当前瓶颈" value:@"无" subtitle:@""];
    _impactCard = [[HUDMetricCard alloc] initWithTitle:@"Codex影响" value:@"低" subtitle:@""];
    _diagnosisRow = [NSStackView stackViewWithViews:@[_bottleneckCard, _impactCard]];
    _diagnosisRow.orientation = NSUserInterfaceLayoutOrientationHorizontal; _diagnosisRow.distribution = NSStackViewDistributionFillEqually; _diagnosisRow.spacing = 7;
    _cpuCard = [[HUDMetricCard alloc] initWithTitle:@"整机CPU" value:@"--" subtitle:@""];
    _memoryPressureCard = [[HUDMetricCard alloc] initWithTitle:@"整机内存" value:@"-- / --G" subtitle:@"已用 --% · 压力 --"];
    _cpuCard.valueLabel.font = [NSFont monospacedDigitSystemFontOfSize:18 weight:NSFontWeightBold];
    _memoryPressureCard.valueLabel.font = [NSFont systemFontOfSize:18 weight:NSFontWeightBold];
    _healthCardsRow = [NSStackView stackViewWithViews:@[_cpuCard, _memoryPressureCard]];
    _healthCardsRow.orientation = NSUserInterfaceLayoutOrientationHorizontal; _healthCardsRow.distribution = NSStackViewDistributionFillEqually; _healthCardsRow.spacing = 8;
    _attributionLabel = [self label:@"Codex：CPU --（整机） · 内存 --G（占总内存 --%）" size:13 color:NSColor.labelColor];
    _healthLabel = [self label:@"系统  Swap --  ·  热状态 --" size:12 color:NSColor.secondaryLabelColor];
    _memoryAppsCard = [HUDMemoryListCard new];
    _sparkline = [HUDSparklineView new];
    _trendTitleLabel = [self label:@"CPU趋势" size:10.5 color:NSColor.secondaryLabelColor];
    _trendLabel = [self label:@"平均 -- · 峰值 --" size:10.5 color:NSColor.labelColor];
    _trendRow = [NSStackView stackViewWithViews:@[_trendTitleLabel, _sparkline, _trendLabel]];
    _trendRow.orientation = NSUserInterfaceLayoutOrientationHorizontal; _trendRow.alignment = NSLayoutAttributeCenterY; _trendRow.spacing = 8;
    _computerFreshnessLabel = [self label:@"来源  macOS系统接口" size:10.5 color:NSColor.tertiaryLabelColor];
    _computerStack = [NSStackView stackViewWithViews:@[_computerStatusLabel, _diagnosisRow, _healthCardsRow, _attributionLabel, _healthLabel, _memoryAppsCard, _trendRow, _computerFreshnessLabel]];
    _computerStack.orientation = NSUserInterfaceLayoutOrientationVertical;
    _computerStack.alignment = NSLayoutAttributeLeading;
    _computerStack.spacing = 6;

    NSMutableArray<NSTextField *> *details = [NSMutableArray array];
    for (NSInteger index = 0; index < 5; index++) [details addObject:[self label:@"" size:11.5 color:NSColor.secondaryLabelColor]];
    _detailLabels = details;
    _detailStack = [NSStackView stackViewWithViews:details];
    _detailStack.orientation = NSUserInterfaceLayoutOrientationVertical;
    _detailStack.alignment = NSLayoutAttributeLeading;
    _detailStack.spacing = 3;
    _detailStack.hidden = YES;

    NSStackView *root = [NSStackView stackViewWithViews:@[header, headerSeparator, _homeStack, _codexStack, _computerStack, _detailStack]];
    root.orientation = NSUserInterfaceLayoutOrientationVertical;
    root.alignment = NSLayoutAttributeLeading;
    root.spacing = 8;
    root.translatesAutoresizingMaskIntoConstraints = NO;
    _rootStack = root;
    [_layoutCanvas addSubview:root];
    [NSLayoutConstraint activateConstraints:@[
        [root.leadingAnchor constraintEqualToAnchor:_layoutCanvas.leadingAnchor constant:14],
        [root.trailingAnchor constraintEqualToAnchor:_layoutCanvas.trailingAnchor constant:-14],
        [root.topAnchor constraintEqualToAnchor:_layoutCanvas.topAnchor constant:10],
        [root.bottomAnchor constraintLessThanOrEqualToAnchor:_layoutCanvas.bottomAnchor constant:-10],
        [header.widthAnchor constraintEqualToAnchor:root.widthAnchor],
        [headerSeparator.widthAnchor constraintEqualToAnchor:root.widthAnchor],
        [headerSeparator.heightAnchor constraintEqualToConstant:1],
        [_homeStack.widthAnchor constraintEqualToAnchor:root.widthAnchor],
        [_codexStack.widthAnchor constraintEqualToAnchor:root.widthAnchor],
        [_computerStack.widthAnchor constraintEqualToAnchor:root.widthAnchor],
        [_detailStack.widthAnchor constraintEqualToAnchor:root.widthAnchor],
        [_quotaRow.widthAnchor constraintEqualToAnchor:_codexStack.widthAnchor],
        [_taskActivityCard.widthAnchor constraintEqualToAnchor:_codexStack.widthAnchor],
        [_recentTasksCard.widthAnchor constraintEqualToAnchor:_codexStack.widthAnchor],
        [_codexInsightsRow.widthAnchor constraintEqualToAnchor:_codexStack.widthAnchor],
        [_localCostCard.widthAnchor constraintEqualToAnchor:_codexStack.widthAnchor],
        [_quotaForecastCard.widthAnchor constraintEqualToAnchor:_codexStack.widthAnchor],
        [_serviceStatusCard.widthAnchor constraintEqualToAnchor:_codexStack.widthAnchor],
        [_usageHistoryRow.widthAnchor constraintEqualToAnchor:_codexStack.widthAnchor],
        [_homeQuotaRow.widthAnchor constraintEqualToAnchor:_homeStack.widthAnchor],
        [_homeTaskActivityCard.widthAnchor constraintEqualToAnchor:_homeStack.widthAnchor],
        [_homeRecentTasksCard.widthAnchor constraintEqualToAnchor:_homeStack.widthAnchor],
        [_homeInsightsRow.widthAnchor constraintEqualToAnchor:_homeStack.widthAnchor],
        [_homeLocalCostCard.widthAnchor constraintEqualToAnchor:_homeStack.widthAnchor],
        [_homeQuotaForecastCard.widthAnchor constraintEqualToAnchor:_homeStack.widthAnchor],
        [_homeServiceStatusCard.widthAnchor constraintEqualToAnchor:_homeStack.widthAnchor],
        [_homeUsageHistoryRow.widthAnchor constraintEqualToAnchor:_homeStack.widthAnchor],
        [_homeComputerCardsRow.widthAnchor constraintEqualToAnchor:_homeStack.widthAnchor],
        [_homeMemoryAppsCard.widthAnchor constraintEqualToAnchor:_homeStack.widthAnchor],
        [_diagnosisRow.widthAnchor constraintEqualToAnchor:_computerStack.widthAnchor],
        [_healthCardsRow.widthAnchor constraintEqualToAnchor:_computerStack.widthAnchor],
        [_memoryAppsCard.widthAnchor constraintEqualToAnchor:_computerStack.widthAnchor]
    ]];
    [self setAccentColor:self.accentColor];
    [self setPage:0];
    return self;
}

- (void)tabSelected:(NSSegmentedControl *)sender {
    [self setPage:sender.selectedSegment];
    if (self.pageChanged) self.pageChanged(sender.selectedSegment);
}

- (void)showSettings:(NSButton *)sender {
    if (self.settingsRequested) { self.settingsRequested(); return; }
    NSMenu *menu = self.menuProvider ? self.menuProvider() : nil;
    if (menu) [menu popUpMenuPositioningItem:nil atLocation:NSMakePoint(NSMinX(sender.frame), NSMinY(sender.frame) - 4) inView:self];
}

- (void)togglePin:(NSButton *)sender {
    [self setAlwaysOnTop:!self.alwaysOnTop];
    if (self.topmostChanged) self.topmostChanged(self.alwaysOnTop);
}

- (void)togglePositionLock:(NSButton *)sender {
    [self setPositionLocked:!self.positionLocked];
    if (self.positionLockChanged) self.positionLockChanged(self.positionLocked);
}

- (void)toggleMinimize:(NSButton *)sender {
    if (self.minimizeRequested) self.minimizeRequested();
}

- (void)setCollapsed:(BOOL)collapsed {
    _collapsed = collapsed;
    [self setPage:self.tabs.selectedSegment];
    self.detailStack.hidden = collapsed || self.compactMode;
}

- (void)setAlwaysOnTop:(BOOL)enabled {
    _alwaysOnTop = enabled;
    NSString *symbol = enabled ? @"pin.fill" : @"pin";
    NSString *description = enabled ? @"取消置顶" : @"开启置顶";
    self.pinButton.image = [NSImage imageWithSystemSymbolName:symbol accessibilityDescription:description];
    self.pinButton.toolTip = enabled ? @"当前置顶，点击取消置顶" : @"当前不置顶，点击置顶";
    self.pinButton.contentTintColor = enabled ? self.accentColor : [NSColor colorWithWhite:1 alpha:0.52];
}

- (void)setPositionLocked:(BOOL)enabled {
    _positionLocked = enabled;
    NSString *symbol = enabled ? @"lock.fill" : @"lock.open";
    NSString *description = enabled ? @"解锁位置和大小" : @"锁定位置和大小";
    self.lockButton.image = [NSImage imageWithSystemSymbolName:symbol accessibilityDescription:description];
    self.lockButton.toolTip = enabled ? @"位置和大小已锁定，点击解锁" : @"锁定位置和大小，防止误拖动";
    self.lockButton.contentTintColor = enabled ? self.accentColor : [NSColor colorWithWhite:1 alpha:0.52];
}

- (void)setPage:(NSInteger)page {
    NSInteger safePage = MAX(0, MIN(2, page));
    self.tabs.selectedSegment = safePage;
    self.homeStack.hidden = self.collapsed || safePage != 0;
    self.codexStack.hidden = self.collapsed || safePage != 1;
    self.computerStack.hidden = self.collapsed || safePage != 2;
}

- (void)setCompact:(BOOL)compact { self.compactMode = compact; self.detailStack.hidden = compact || self.collapsed; }
- (void)refreshQuotaRowVisibility { self.quotaRow.hidden = self.fiveHourCard.hidden && self.weeklyCard.hidden; }
- (void)refreshCodexInsightsRow { self.codexInsightsRow.hidden = self.planCard.hidden && self.usageCard.hidden && self.modelQuotaCard.hidden; }
- (void)refreshUsageHistoryRow { self.usageHistoryRow.hidden = self.longestTurnCard.hidden && self.longestStreakCard.hidden; }
- (void)setFiveHourQuotaVisible:(BOOL)visible { self.fiveHourCard.hidden = !visible; [self refreshQuotaRowVisibility]; }
- (void)setWeeklyQuotaVisible:(BOOL)visible { self.weeklyCard.hidden = !visible; [self refreshQuotaRowVisibility]; }
- (void)setPlanVisible:(BOOL)visible { self.planCard.hidden = !visible; [self refreshCodexInsightsRow]; }
- (void)setUsageVisible:(BOOL)visible { self.usageCard.hidden = !visible; [self refreshCodexInsightsRow]; }
- (void)setModelQuotaVisible:(BOOL)visible { self.modelQuotaCard.hidden = !visible; [self refreshCodexInsightsRow]; }
- (void)setLocalCostVisible:(BOOL)visible { self.localCostCard.hidden = !visible; }
- (void)setQuotaForecastVisible:(BOOL)visible { self.quotaForecastCard.hidden = !visible; }
- (void)setServiceStatusVisible:(BOOL)visible { self.serviceStatusCard.hidden = !visible; }
- (void)setTaskActivityVisible:(BOOL)visible { self.taskActivityCard.hidden = !visible; }
- (void)setRecentTasksVisible:(BOOL)visible { self.recentTasksCard.hidden = !visible; }
- (void)setLongestTurnVisible:(BOOL)visible { self.longestTurnCard.hidden = !visible; [self refreshUsageHistoryRow]; }
- (void)setLongestStreakVisible:(BOOL)visible { self.longestStreakCard.hidden = !visible; [self refreshUsageHistoryRow]; }
- (void)setSystemVisible:(BOOL)visible { self.healthCardsRow.hidden = !visible; self.healthLabel.hidden = !visible; }
- (void)setAttributionVisible:(BOOL)visible { self.attributionLabel.hidden = !visible; }
- (void)setTrendVisible:(BOOL)visible { self.trendRow.hidden = !visible; }
- (void)setMemoryAppsVisible:(BOOL)visible { self.memoryAppsCard.hidden = !visible; }
- (void)refreshHomeCodexSection { self.homeCodexStatusLabel.hidden = self.homeTaskActivityCard.hidden && self.homeRecentTasksCard.hidden && self.homeQuotaRow.hidden && self.homeInsightsRow.hidden && self.homeLocalCostCard.hidden && self.homeQuotaForecastCard.hidden && self.homeServiceStatusCard.hidden && self.homeUsageHistoryRow.hidden; }
- (void)refreshHomeComputerSection { self.homeComputerStatusLabel.hidden = self.homeComputerCardsRow.hidden && self.homeAttributionLabel.hidden && self.homeMemoryAppsCard.hidden && self.homeTrendRow.hidden; }
- (void)refreshHomeQuotaRow { self.homeQuotaRow.hidden = self.homeFiveHourCard.hidden && self.homeWeeklyCard.hidden; [self refreshHomeCodexSection]; }
- (void)refreshHomeInsightsRow { self.homeInsightsRow.hidden = self.homePlanCard.hidden && self.homeUsageCard.hidden && self.homeModelQuotaCard.hidden; [self refreshHomeCodexSection]; }
- (void)refreshHomeUsageHistoryRow { self.homeUsageHistoryRow.hidden = self.homeLongestTurnCard.hidden && self.homeLongestStreakCard.hidden; [self refreshHomeCodexSection]; }
- (void)refreshHomeComputerCardsRow { self.homeComputerCardsRow.hidden = self.homeBottleneckCard.hidden && self.homeSystemCard.hidden; [self refreshHomeComputerSection]; }
- (void)setHomeFiveHourVisible:(BOOL)visible { self.homeFiveHourCard.hidden = !visible; [self refreshHomeQuotaRow]; }
- (void)setHomeWeeklyVisible:(BOOL)visible { self.homeWeeklyCard.hidden = !visible; [self refreshHomeQuotaRow]; }
- (void)setHomePlanVisible:(BOOL)visible { self.homePlanCard.hidden = !visible; [self refreshHomeInsightsRow]; }
- (void)setHomeUsageVisible:(BOOL)visible { self.homeUsageCard.hidden = !visible; [self refreshHomeInsightsRow]; }
- (void)setHomeModelQuotaVisible:(BOOL)visible { self.homeModelQuotaCard.hidden = !visible; [self refreshHomeInsightsRow]; }
- (void)setHomeLocalCostVisible:(BOOL)visible { self.homeLocalCostCard.hidden = !visible; [self refreshHomeCodexSection]; }
- (void)setHomeQuotaForecastVisible:(BOOL)visible { self.homeQuotaForecastCard.hidden = !visible; [self refreshHomeCodexSection]; }
- (void)setHomeServiceStatusVisible:(BOOL)visible { self.homeServiceStatusCard.hidden = !visible; [self refreshHomeCodexSection]; }
- (void)setHomeTaskActivityVisible:(BOOL)visible { self.homeTaskActivityCard.hidden = !visible; [self refreshHomeCodexSection]; }
- (void)setHomeRecentTasksVisible:(BOOL)visible { self.homeRecentTasksCard.hidden = !visible; [self refreshHomeCodexSection]; }
- (void)setHomeLongestTurnVisible:(BOOL)visible { self.homeLongestTurnCard.hidden = !visible; [self refreshHomeUsageHistoryRow]; }
- (void)setHomeLongestStreakVisible:(BOOL)visible { self.homeLongestStreakCard.hidden = !visible; [self refreshHomeUsageHistoryRow]; }
- (void)setHomeDiagnosisVisible:(BOOL)visible { self.homeBottleneckCard.hidden = !visible; [self refreshHomeComputerCardsRow]; }
- (void)setHomeSystemVisible:(BOOL)visible { self.homeSystemCard.hidden = !visible; [self refreshHomeComputerCardsRow]; }
- (void)setHomeAttributionVisible:(BOOL)visible { self.homeAttributionLabel.hidden = !visible; [self refreshHomeComputerSection]; }
- (void)setHomeTrendVisible:(BOOL)visible { self.homeTrendRow.hidden = !visible; [self refreshHomeComputerSection]; }
- (void)setHomeMemoryAppsVisible:(BOOL)visible { self.homeMemoryAppsCard.hidden = !visible; [self refreshHomeComputerSection]; }
- (void)applyHomeCodexOrder:(NSArray<NSString *> *)codexOrder computerOrder:(NSArray<NSString *> *)computerOrder {
    NSDictionary<NSString *, NSView *> *codexViews = @{
        @"activity": self.homeTaskActivityCard, @"recent": self.homeRecentTasksCard, @"quota": self.homeQuotaRow,
        @"insights": self.homeInsightsRow, @"cost": self.homeLocalCostCard,
        @"forecast": self.homeQuotaForecastCard, @"service": self.homeServiceStatusCard, @"history": self.homeUsageHistoryRow
    };
    NSDictionary<NSString *, NSView *> *computerViews = @{
        @"summary": self.homeComputerCardsRow, @"attribution": self.homeAttributionLabel,
        @"memory": self.homeMemoryAppsCard, @"trend": self.homeTrendRow
    };
    NSMutableArray<NSView *> *ordered = [NSMutableArray arrayWithObject:self.homeCodexStatusLabel];
    for (NSString *key in codexOrder) if (codexViews[key]) [ordered addObject:codexViews[key]];
    [ordered addObject:self.homeComputerStatusLabel];
    for (NSString *key in computerOrder) if (computerViews[key]) [ordered addObject:computerViews[key]];
    [ordered addObject:self.homeFreshnessLabel];
    for (NSView *view in [self.homeStack.arrangedSubviews copy]) [self.homeStack removeArrangedSubview:view];
    for (NSView *view in ordered) [self.homeStack addArrangedSubview:view];
}
- (void)setAccentColor:(NSColor *)accentColor {
    _accentColor = accentColor ?: NSColor.systemGreenColor;
    self.codexStatusLabel.textColor = _accentColor;
    self.homeCodexStatusLabel.textColor = _accentColor; self.homeComputerStatusLabel.textColor = _accentColor;
    self.fiveHourCard.progressView.accentColor = _accentColor; self.weeklyCard.progressView.accentColor = _accentColor;
    self.homeFiveHourCard.progressView.accentColor = _accentColor; self.homeWeeklyCard.progressView.accentColor = _accentColor;
    self.cpuCard.valueLabel.textColor = _accentColor; self.memoryPressureCard.valueLabel.textColor = _accentColor;
    self.bottleneckCard.valueLabel.textColor = _accentColor; self.impactCard.valueLabel.textColor = _accentColor;
    self.sparkline.accentColor = _accentColor;
    self.homeSystemCard.valueLabel.textColor = _accentColor; self.homeBottleneckCard.valueLabel.textColor = _accentColor; self.homeSparkline.accentColor = _accentColor;
    [self setAlwaysOnTop:self.alwaysOnTop];
    [self setPositionLocked:self.positionLocked];
}
- (void)setBackgroundOpacity:(CGFloat)opacity {
    CGFloat clamped = MAX(0.55, MIN(1.0, opacity));
    self.tintView.opacity = clamped;
    self.layer.backgroundColor = NSColor.clearColor.CGColor;
}
- (BOOL)mouseDownCanMoveWindow { return !self.positionLocked; }
- (void)rightMouseDown:(NSEvent *)event {
    NSMenu *menu = self.menuProvider ? self.menuProvider() : nil;
    if (menu) [NSMenu popUpContextMenu:menu withEvent:event forView:self];
}

@end
