#import <Cocoa/Cocoa.h>
#import "NativeSampler.h"

@interface HUDProgressView : NSView
@property(nonatomic) CGFloat progress;
@property(nonatomic, strong) NSColor *accentColor;
@end

@interface HUDQuotaCard : NSBox
@property(nonatomic, strong) NSTextField *titleLabel;
@property(nonatomic, strong) NSTextField *windowLabel;
@property(nonatomic, strong) NSTextField *valueLabel;
@property(nonatomic, strong) NSTextField *resetLabel;
@property(nonatomic, strong) HUDProgressView *progressView;
- (instancetype)initWithTitle:(NSString *)title;
- (void)showAvailable:(BOOL)available remaining:(double)remaining reset:(NSString *)reset accent:(NSColor *)accent;
@end

@interface HUDMetricCard : NSBox
@property(nonatomic, strong) NSTextField *titleLabel;
@property(nonatomic, strong) NSTextField *valueLabel;
@property(nonatomic, strong) NSTextField *subtitleLabel;
- (instancetype)initWithTitle:(NSString *)title value:(NSString *)value subtitle:(NSString *)subtitle;
@end

@interface HUDMemoryListCard : NSBox
- (void)updateApps:(NSArray<NativeTopApp *> *)apps totalMemoryGiB:(double)totalMemoryGiB;
@end

@interface HUDRecentTasksCard : NSBox
- (void)updateRows:(NSArray<NSString *> *)rows footer:(NSString *)footer;
@end

@interface HUDQuotaDetailsCard : NSBox
- (void)updateRows:(NSArray<NSString *> *)rows footer:(NSString *)footer;
@end

@interface HUDSparklineView : NSView
@property(nonatomic, copy) NSArray<NSNumber *> *values;
@property(nonatomic, strong) NSColor *accentColor;
@end

@interface HUDView : NSVisualEffectView
@property(nonatomic, strong) NSView *layoutCanvas;
@property(nonatomic, strong) NSStackView *rootStack;
@property(nonatomic, strong) NSSegmentedControl *tabs;
@property(nonatomic, strong) NSButton *minimizeButton;
@property(nonatomic, strong) NSButton *pinButton;
@property(nonatomic, strong) NSButton *lockButton;
@property(nonatomic, strong) NSButton *taskCenterButton;
@property(nonatomic, strong) NSButton *settingsButton;
@property(nonatomic, strong) NSStackView *homeStack;
@property(nonatomic, strong) NSTextField *homeCodexStatusLabel;
@property(nonatomic, strong) HUDMetricCard *homeTaskActivityCard;
@property(nonatomic, strong) HUDRecentTasksCard *homeRecentTasksCard;
@property(nonatomic, strong) NSStackView *homeQuotaRow;
@property(nonatomic, strong) HUDQuotaCard *homeFiveHourCard;
@property(nonatomic, strong) HUDQuotaCard *homeWeeklyCard;
@property(nonatomic, strong) NSStackView *homeInsightsRow;
@property(nonatomic, strong) HUDMetricCard *homePlanCard;
@property(nonatomic, strong) HUDMetricCard *homeUsageCard;
@property(nonatomic, strong) HUDMetricCard *homeModelQuotaCard;
@property(nonatomic, strong) HUDMetricCard *homeLocalCostCard;
@property(nonatomic, strong) NSStackView *homeTokenWindowRow;
@property(nonatomic, strong) HUDMetricCard *homeFiveHourTokensCard;
@property(nonatomic, strong) HUDMetricCard *homeRollingDayTokensCard;
@property(nonatomic, strong) HUDMetricCard *homeWeeklyTokensCard;
@property(nonatomic, strong) HUDMetricCard *homeQuotaForecastCard;
@property(nonatomic, strong) HUDMetricCard *homeServiceStatusCard;
@property(nonatomic, strong) HUDQuotaDetailsCard *homeQuotaDetailsCard;
@property(nonatomic, strong) NSStackView *homeUsageHistoryRow;
@property(nonatomic, strong) HUDMetricCard *homeLongestTurnCard;
@property(nonatomic, strong) HUDMetricCard *homeLongestStreakCard;
@property(nonatomic, strong) HUDMetricCard *homePeakDailyTokensCard;
@property(nonatomic, strong) NSTextField *homeComputerStatusLabel;
@property(nonatomic, strong) NSStackView *homeComputerCardsRow;
@property(nonatomic, strong) HUDMetricCard *homeBottleneckCard;
@property(nonatomic, strong) HUDMetricCard *homeSystemCard;
@property(nonatomic, strong) NSTextField *homeAttributionLabel;
@property(nonatomic, strong) HUDMemoryListCard *homeMemoryAppsCard;
@property(nonatomic, strong) NSStackView *homeTrendRow;
@property(nonatomic, strong) HUDSparklineView *homeSparkline;
@property(nonatomic, strong) NSTextField *homeTrendTitleLabel;
@property(nonatomic, strong) NSTextField *homeTrendLabel;
@property(nonatomic, strong) NSTextField *homeFreshnessLabel;
@property(nonatomic, strong) NSStackView *codexStack;
@property(nonatomic, strong) NSStackView *computerStack;
@property(nonatomic, strong) NSStackView *detailStack;
@property(nonatomic, strong) NSTextField *codexStatusLabel;
@property(nonatomic, strong) HUDMetricCard *taskActivityCard;
@property(nonatomic, strong) HUDRecentTasksCard *recentTasksCard;
@property(nonatomic, strong) NSStackView *quotaRow;
@property(nonatomic, strong) HUDQuotaCard *fiveHourCard;
@property(nonatomic, strong) HUDQuotaCard *weeklyCard;
@property(nonatomic, strong) NSStackView *codexInsightsRow;
@property(nonatomic, strong) HUDMetricCard *planCard;
@property(nonatomic, strong) HUDMetricCard *usageCard;
@property(nonatomic, strong) HUDMetricCard *modelQuotaCard;
@property(nonatomic, strong) HUDMetricCard *localCostCard;
@property(nonatomic, strong) NSStackView *tokenWindowRow;
@property(nonatomic, strong) HUDMetricCard *fiveHourTokensCard;
@property(nonatomic, strong) HUDMetricCard *rollingDayTokensCard;
@property(nonatomic, strong) HUDMetricCard *weeklyTokensCard;
@property(nonatomic, strong) HUDMetricCard *quotaForecastCard;
@property(nonatomic, strong) HUDMetricCard *serviceStatusCard;
@property(nonatomic, strong) HUDQuotaDetailsCard *quotaDetailsCard;
@property(nonatomic, strong) NSStackView *usageHistoryRow;
@property(nonatomic, strong) HUDMetricCard *longestTurnCard;
@property(nonatomic, strong) HUDMetricCard *longestStreakCard;
@property(nonatomic, strong) HUDMetricCard *peakDailyTokensCard;
@property(nonatomic, strong) NSTextField *codexFreshnessLabel;
@property(nonatomic, strong) NSTextField *computerStatusLabel;
@property(nonatomic, strong) HUDMetricCard *bottleneckCard;
@property(nonatomic, strong) HUDMetricCard *impactCard;
@property(nonatomic, strong) NSStackView *healthCardsRow;
@property(nonatomic, strong) HUDMetricCard *cpuCard;
@property(nonatomic, strong) HUDMetricCard *memoryPressureCard;
@property(nonatomic, strong) NSTextField *attributionLabel;
@property(nonatomic, strong) NSTextField *healthLabel;
@property(nonatomic, strong) HUDMemoryListCard *memoryAppsCard;
@property(nonatomic, strong) HUDSparklineView *sparkline;
@property(nonatomic, strong) NSTextField *trendTitleLabel;
@property(nonatomic, strong) NSTextField *trendLabel;
@property(nonatomic, strong) NSTextField *computerFreshnessLabel;
@property(nonatomic, strong) NSArray<NSTextField *> *detailLabels;
@property(nonatomic, copy) NSMenu *(^menuProvider)(void);
@property(nonatomic, copy) void (^settingsRequested)(void);
@property(nonatomic, copy) void (^taskCenterRequested)(void);
@property(nonatomic, copy) void (^pageChanged)(NSInteger page);
@property(nonatomic, copy) void (^topmostChanged)(BOOL enabled);
@property(nonatomic, copy) void (^positionLockChanged)(BOOL enabled);
@property(nonatomic, copy) void (^minimizeRequested)(void);
@property(nonatomic, strong) NSColor *accentColor;
@property(nonatomic) BOOL alwaysOnTop;
@property(nonatomic) BOOL positionLocked;
@property(nonatomic) BOOL collapsed;
- (void)setPage:(NSInteger)page;
- (void)setCompact:(BOOL)compact;
- (void)setFiveHourQuotaVisible:(BOOL)visible;
- (void)setWeeklyQuotaVisible:(BOOL)visible;
- (void)setPlanVisible:(BOOL)visible;
- (void)setUsageVisible:(BOOL)visible;
- (void)setModelQuotaVisible:(BOOL)visible;
- (void)setLocalCostVisible:(BOOL)visible;
- (void)setTokenWindowsVisible:(BOOL)visible;
- (void)setQuotaForecastVisible:(BOOL)visible;
- (void)setServiceStatusVisible:(BOOL)visible;
- (void)setQuotaDetailsVisible:(BOOL)visible;
- (void)setTaskActivityVisible:(BOOL)visible;
- (void)setRecentTasksVisible:(BOOL)visible;
- (void)setLongestTurnVisible:(BOOL)visible;
- (void)setLongestStreakVisible:(BOOL)visible;
- (void)setPeakDailyTokensVisible:(BOOL)visible;
- (void)setSystemVisible:(BOOL)visible;
- (void)setAttributionVisible:(BOOL)visible;
- (void)setTrendVisible:(BOOL)visible;
- (void)setMemoryAppsVisible:(BOOL)visible;
- (void)setHomeFiveHourVisible:(BOOL)visible;
- (void)setHomeWeeklyVisible:(BOOL)visible;
- (void)setHomePlanVisible:(BOOL)visible;
- (void)setHomeUsageVisible:(BOOL)visible;
- (void)setHomeModelQuotaVisible:(BOOL)visible;
- (void)setHomeLocalCostVisible:(BOOL)visible;
- (void)setHomeTokenWindowsVisible:(BOOL)visible;
- (void)setHomeQuotaForecastVisible:(BOOL)visible;
- (void)setHomeServiceStatusVisible:(BOOL)visible;
- (void)setHomeQuotaDetailsVisible:(BOOL)visible;
- (void)setHomeTaskActivityVisible:(BOOL)visible;
- (void)setHomeRecentTasksVisible:(BOOL)visible;
- (void)setHomeLongestTurnVisible:(BOOL)visible;
- (void)setHomeLongestStreakVisible:(BOOL)visible;
- (void)setHomePeakDailyTokensVisible:(BOOL)visible;
- (void)setHomeDiagnosisVisible:(BOOL)visible;
- (void)setHomeSystemVisible:(BOOL)visible;
- (void)setHomeAttributionVisible:(BOOL)visible;
- (void)setHomeTrendVisible:(BOOL)visible;
- (void)setHomeMemoryAppsVisible:(BOOL)visible;
- (void)setBackgroundOpacity:(CGFloat)opacity;
- (void)setAlwaysOnTop:(BOOL)enabled;
- (void)setPositionLocked:(BOOL)enabled;
- (void)setCollapsed:(BOOL)collapsed;
- (void)applyHomeCodexOrder:(NSArray<NSString *> *)codexOrder computerOrder:(NSArray<NSString *> *)computerOrder;
@end
