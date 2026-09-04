#import <Foundation/Foundation.h>
#import "CodexStatusProvider.h"

@interface CodexStatusProvider (LiveCheckAccess)
- (void)sendObject:(NSDictionary *)object;
- (void)handleObject:(NSDictionary *)object;
@end
@interface MetadataOnlyProvider : CodexStatusProvider
@property(nonatomic) BOOL requestedLightweight;
@property(nonatomic) BOOL legacyRetry;
@end
@implementation MetadataOnlyProvider
- (void)refreshActivity {}
- (void)refreshCostHistory {}
- (void)handleObject:(NSDictionary *)object {
    if ([object[@"id"] isEqual:@2] || [object[@"id"] isEqual:@102]) {
        NSDictionary *error = [object[@"error"] isKindOfClass:NSDictionary.class] ? object[@"error"] : nil;
        NSNumber *code = [error[@"code"] isKindOfClass:NSNumber.class] ? error[@"code"] : nil;
        if (error) printf("quota_rpc_error_code=%s\n", code.stringValue.UTF8String ?: "unavailable");
    }
    [super handleObject:object];
}
- (void)sendObject:(NSDictionary *)object {
    if ([object[@"method"] isEqual:@"account/rateLimits/read"]) {
        if (object[@"params"][@"excludeResetCreditDetails"]) self.requestedLightweight = YES;
        if ([object[@"id"] isEqual:@102]) self.legacyRetry = YES;
    }
    [super sendObject:object];
}
@end

int main(void) {
    @autoreleasepool {
        MetadataOnlyProvider *p = [MetadataOnlyProvider new];
        p.costHistoryEnabled = NO; p.quotaForecastEnabled = NO;
        BOOL passed = YES;
        for (NSString *mode in @[@"startup", @"background", @"manual"]) {
            p.requestedLightweight = NO; p.legacyRetry = NO;
            if ([mode isEqual:@"background"]) [p refreshQuotaInBackground]; else [p refreshQuota];
            NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:10];
            while ([p valueForKey:@"task"] && deadline.timeIntervalSinceNow > 0) [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
            BOOL quotaOK = p.snapshot.quotaAvailable && !p.snapshot.interfaceFailureKinds[@"2"];
            printf("%s quota=%s lightweight=%s legacy_retry=%s allowance_field=%s failure_categories=%s\n", mode.UTF8String,
                quotaOK ? "pass" : "fail", p.requestedLightweight ? "yes" : "no", p.legacyRetry ? "yes" : "no",
                p.snapshot.ordinaryUsageAllowed ? "available" : "unavailable", p.snapshot.interfaceFailureKinds.description.UTF8String);
            passed = passed && quotaOK; [p stop];
        }
        return passed ? 0 : 1;
    }
}
