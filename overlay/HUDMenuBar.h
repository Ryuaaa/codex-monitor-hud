#import <Foundation/Foundation.h>
#import "CodexStatusProvider.h"
#import "NativeSampler.h"
#import <math.h>

// Presentation only. Never starts a provider, adds a timer, or reads account files.
static inline NSString *HUDMenuMode(id value) {
    return [@[@"floating", @"menuBar", @"both"] containsObject:value ?: @""] ? value : @"both";
}
static inline NSString *HUDMenuModeForVisibility(BOOL menuBarVisible, BOOL floatingVisible) {
    if (menuBarVisible && floatingVisible) return @"both";
    if (menuBarVisible) return @"menuBar";
    if (floatingVisible) return @"floating";
    return nil;
}
static inline NSArray<NSString *> *HUDMenuMetrics(id value) {
    if (![value isKindOfClass:NSArray.class]) return @[@"weekly"];
    NSMutableArray *result = [NSMutableArray array];
    for (id metric in value) {
        if ([@[@"weekly", @"fiveHour", @"cpu", @"memory"] containsObject:metric] && ![result containsObject:metric]) [result addObject:metric];
        if (result.count == 2) break;
    }
    return result;
}
static inline NSString *HUDMenuQuotaValue(CodexStatusSnapshot *s, BOOL weekly, NSTimeInterval now) {
    BOOL available = weekly ? s.weeklyAvailable : s.fiveHourAvailable;
    double value = weekly ? s.weeklyRemainingPercent : s.fiveHourRemainingPercent;
    NSTimeInterval reset = weekly ? s.weeklyResetAt : s.fiveHourResetAt;
    NSString *state = weekly ? s.weeklyDataState : s.fiveHourDataState;
    if (!s || !available || !isfinite(value) || value < 0 || value > 100 ||
        (reset > 0 && reset <= now) || [state isEqual:@"expired"]) return @"—";
    BOOL stale = s.quotaErrorText.length > 0 || ![state isEqual:@"live"] ||
                 s.quotaUpdatedAt <= 0 || now - s.quotaUpdatedAt > 900 || s.quotaUpdatedAt > now + 60;
    return [NSString stringWithFormat:@"%.0f%%%@", value, stale ? @"*" : @""];
}
static inline NSString *HUDMenuSystemValue(NativeSnapshot *s, BOOL memory, NSTimeInterval now) {
    double value = memory ? s.systemMemoryUsedPercent : s.systemCPUPercent;
    if (!s || s.timestamp <= 0 || !isfinite(value) || value < 0 || value > 100 ||
        now - s.timestamp > 60 || s.timestamp > now + 60) return @"—";
    return [NSString stringWithFormat:@"%.0f%%", value];
}
