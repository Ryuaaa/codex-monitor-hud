#import <Foundation/Foundation.h>

FOUNDATION_EXPORT NSDictionary<NSString *, NSNumber *> *CodexCalendarUsage(NSArray *buckets, NSDate *now);

@interface CodexStatusSnapshot : NSObject
@property(nonatomic) BOOL quotaAvailable;
@property(nonatomic) NSTimeInterval quotaUpdatedAt;
@property(nonatomic, copy) NSString *quotaErrorText;
@property(nonatomic) BOOL fiveHourAvailable;
@property(nonatomic) double fiveHourRemainingPercent;
@property(nonatomic) NSTimeInterval fiveHourResetAt;
@property(nonatomic) BOOL weeklyAvailable;
@property(nonatomic) double weeklyRemainingPercent;
@property(nonatomic) NSTimeInterval weeklyResetAt;
@property(nonatomic) BOOL accountAvailable;
@property(nonatomic, copy) NSString *planType;
@property(nonatomic) NSTimeInterval accountUpdatedAt;
@property(nonatomic, copy) NSString *accountErrorText;
@property(nonatomic) BOOL usageAvailable;
@property(nonatomic) NSTimeInterval usageUpdatedAt;
@property(nonatomic, copy) NSString *usageErrorText;
@property(nonatomic) long long todayTokens;
@property(nonatomic) long long sevenDayTokens;
@property(nonatomic) long long previousSevenDayTokens;
@property(nonatomic) long long lifetimeTokens;
@property(nonatomic) NSInteger currentStreakDays;
@property(nonatomic) BOOL modelQuotaAvailable;
@property(nonatomic, copy) NSString *modelQuotaName;
@property(nonatomic) double modelQuotaRemainingPercent;
@property(nonatomic) NSTimeInterval modelQuotaResetAt;
@property(nonatomic, copy) NSString *modelQuotaWindowLabel;
@property(nonatomic) NSTimeInterval updatedAt;
@property(nonatomic, copy) NSString *statusText;
@end

@interface CodexStatusProvider : NSObject
@property(nonatomic, readonly) CodexStatusSnapshot *snapshot;
@property(nonatomic, copy) void (^updateHandler)(void);
- (void)start;
- (void)refreshQuota;
- (void)stop;
@end
