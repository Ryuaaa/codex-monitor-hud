#import <Foundation/Foundation.h>

FOUNDATION_EXPORT NSDictionary<NSString *, id> *CodexCostEstimateForTokens(NSString *model,
                                                                            long long inputTokens,
                                                                            long long cachedInputTokens,
                                                                            long long cacheWriteInputTokens,
                                                                            long long outputTokens);
FOUNDATION_EXPORT NSDictionary<NSString *, id> *CodexCostEventsFromJSONLLines(NSArray<NSString *> *lines,
                                                                               NSDate *now,
                                                                               NSInteger historyDays);
FOUNDATION_EXPORT NSDictionary<NSString *, id> *CodexAggregateCostEvents(NSArray<NSDictionary<NSString *, id> *> *events,
                                                                          NSDate *now,
                                                                          BOOL scanIncomplete);
FOUNDATION_EXPORT NSDictionary<NSString *, id> *CodexScanCostHistoryAtHome(NSURL *codexHome,
                                                                            NSURL *cacheURL,
                                                                            NSURL *trackingStartURL,
                                                                            NSDate *now);
FOUNDATION_EXPORT NSDictionary<NSString *, id> *CodexQuotaForecastFromSamples(NSArray<NSDictionary<NSString *, id> *> *samples,
                                                                               NSString *remainingKey,
                                                                               NSString *resetKey,
                                                                               double currentRemaining,
                                                                               NSTimeInterval currentResetAt,
                                                                               NSDate *now);
FOUNDATION_EXPORT NSDictionary<NSString *, id> *CodexUpdateQuotaForecastHistory(NSURL *historyURL,
                                                                                 NSDictionary<NSString *, id> *sample,
                                                                                 NSDate *now);
