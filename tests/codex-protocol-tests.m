#import <Foundation/Foundation.h>
#import "CodexStatusProvider.h"
#import "CodexProtocolCompatibility.h"

// Synthetic metadata only. No credentials, real tasks, rollout reads or persistent state.
@interface CodexStatusProvider (FixtureAccess)
- (void)handleObject:(NSDictionary *)object;
- (void)consumeData:(NSData *)data;
- (void)consumeRateLimits:(NSDictionary *)result;
- (void)consumeThreads:(NSDictionary *)result;
- (void)consumeUsage:(NSDictionary *)result;
- (void)consumeAccount:(NSDictionary *)result;
- (void)recordFailure:(NSString *)kind requestID:(NSInteger)requestID;
- (void)finishFetch;
- (void)sendObject:(NSDictionary *)object;
@end

@interface FixtureProvider : CodexStatusProvider
@property(nonatomic) NSMutableArray *sent;
@property(nonatomic) NSMutableArray *samples;
@property(nonatomic, copy) NSString *fixtureExecutable;
@property(nonatomic) BOOL realIO;
@end
@implementation FixtureProvider
- (instancetype)init {
    if ((self = [super init])) { _sent = [NSMutableArray array]; _samples = [NSMutableArray array]; self.costHistoryEnabled = NO; }
    return self;
}
- (void)sendObject:(NSDictionary *)object { [self.sent addObject:object]; if (self.realIO) [super sendObject:object]; }
- (void)refreshActivity {}
- (void)refreshCostHistory {}
- (void)updateQuotaForecastWithSample:(NSDictionary *)sample { [self.samples addObject:sample]; }
- (NSString *)codexExecutable { return self.fixtureExecutable; }
@end

static NSUInteger checks = 0, failures = 0;
static void Check(BOOL pass, const char *name) {
    checks++; if (!pass) { failures++; fprintf(stderr, "FAIL %s\n", name); }
}
static NSDictionary *Quota(id allowed) {
    NSTimeInterval reset = NSDate.date.timeIntervalSince1970 + 86400;
    NSMutableDictionary *r = [@{@"rateLimits": @{
        @"primary": @{@"usedPercent": @25, @"windowDurationMins": @300, @"resetsAt": @(reset)},
        @"secondary": @{@"usedPercent": @40, @"windowDurationMins": @10080, @"resetsAt": @(reset)}
    }} mutableCopy];
    if (allowed) r[@"ordinaryUsageAllowed"] = allowed;
    return r;
}
static FixtureProvider *Ready(BOOL light) {
    FixtureProvider *p = [FixtureProvider new];
    [p setValue:@(light) forKey:@"lightweightQuotaRequest"];
    [p handleObject:@{@"id": @1, @"result": @{}}];
    return p;
}
static void CompleteOtherReplies(FixtureProvider *p) {
    [p handleObject:@{@"id": @3, @"result": @{@"account": @{@"planType": @"pro"}}}];
    [p handleObject:@{@"id": @4, @"result": @{@"dailyUsageBuckets": @[], @"summary": @{}}}];
    [p handleObject:@{@"id": @5, @"result": @{@"data": @[]}}];
}

static void WaitForFetch(FixtureProvider *p) {
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:10];
    while ([p valueForKey:@"task"] && deadline.timeIntervalSinceNow > 0) {
        [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
    }
    Check([p valueForKey:@"task"] == nil, "fetch process cleaned up");
    [p stop];
}

int main(int argc, const char **argv) {
    @autoreleasepool {
        Check(CodexProtocolNumber(NSNull.null) == nil && CodexProtocolNumber(@YES) == nil && CodexProtocolNumber(@"25") == nil, "strict number types");
        Check(CodexProtocolNumber(@(NAN)) == nil && CodexProtocolNumber(@(INFINITY)) == nil, "finite numbers");
        Check(CodexProtocolBoolean(@NO) != nil && CodexProtocolBoolean(@0) == nil && CodexProtocolBoolean(@"false") == nil, "strict boolean types");
        Check(CodexQuotaReadRequest(@2, NO)[@"params"] == nil, "startup and manual full read");
        Check([CodexQuotaReadRequest(@2, YES)[@"params"] isEqual:@{@"excludeResetCreditDetails": @YES}], "background opt-out details only");
        NSDictionary *invalid = @{@"code": @(-32602), @"message": @"SYNTHETIC_PRIVATE"};
        FixtureProvider *legacy = Ready(YES);
        Check([legacy.sent[1][@"params"] isEqual:@{@"excludeResetCreditDetails": @YES}], "initialize sends lean read");
        [legacy handleObject:@{@"id": @2, @"error": invalid}];
        Check(legacy.sent.count == 6 && [legacy.sent.lastObject[@"id"] isEqual:@102] && !legacy.sent.lastObject[@"params"], "one legacy fallback without params");
        [legacy handleObject:@{@"id": @2, @"error": invalid}];
        [legacy handleObject:@{@"id": @102, @"error": invalid}];
        [legacy handleObject:@{@"id": @102, @"error": invalid}];
        Check(legacy.sent.count == 6 && [[legacy valueForKey:@"legacyQuotaParameters"] boolValue], "duplicate replies no retries; capability cached");
        Check([legacy.snapshot.interfaceFailureKinds[@"2"] isEqual:@"invalid_parameters"], "fallback terminal error classified");
        FixtureProvider *recovered = Ready(YES);
        [recovered handleObject:@{@"id": @2, @"error": invalid}];
        [recovered handleObject:@{@"id": @102, @"result": Quota(@NO)}];
        CompleteOtherReplies(recovered);
        Check(recovered.snapshot.weeklyRemainingPercent == 60 && recovered.snapshot.ordinaryUsageAllowed != nil && !recovered.snapshot.ordinaryUsageAllowed.boolValue, "fallback succeeds with official false");
        Check(recovered.snapshot.interfaceFailureKinds.count == 0 && recovered.recommendedRetryInterval == 0, "success resets failures");

        NSDictionary *expected = @{@"unauthorized": @"authentication", @"usageLimitExceeded": @"quota_exhausted", @"rateLimitExceeded": @"rate_limited", @"serverOverloaded": @"overloaded", @"misalignmentPolicyViolation": @"policy_blocked", @"futureError": @"unknown"};
        for (NSString *variant in expected) {
            NSDictionary *error = @{@"code": @(-32000), @"data": @{@"codexErrorInfo": variant, @"_meta": @{@"secret": @"SYNTHETIC_PRIVATE"}}, @"message": @"SYNTHETIC_PRIVATE"};
            Check([CodexProtocolErrorKind(error) isEqual:expected[variant]], "structured error category");
            FixtureProvider *p = Ready(YES); [p handleObject:@{@"id": @2, @"error": error}];
            Check(p.sent.count == 5 && ![p.snapshot.quotaErrorText containsString:@"SYNTHETIC_PRIVATE"], "no retry or sensitive echo");
        }
        Check([CodexProtocolErrorKind(@{@"code": @(-32601)}) isEqual:@"unsupported"], "unsupported method");
        Check(!CodexQuotaNeedsLegacyRetry(invalid, NO, NO) && !CodexQuotaNeedsLegacyRetry(invalid, YES, YES), "retry bounded by request mode");
        Check(CodexQuotaNeedsLegacyRetry(@{@"code": @(-32600)}, YES, NO) && !CodexQuotaNeedsLegacyRetry(@{@"code": @(-32700)}, YES, NO), "legacy request decoder fallback without parse-error retry");
        Check([CodexProtocolErrorKind(@{@"data": @{@"codexErrorInfo": @{@"responseStreamDisconnected": @{@"httpStatusCode": @429}}}}) isEqual:@"rate_limited"], "structured http rate limit");
        Check([CodexProtocolErrorKind(@{@"data": @{@"codexErrorInfo": @{@"httpConnectionFailed": @{@"httpStatusCode": NSNull.null}}}}) isEqual:@"network"], "nullable http status");

        FixtureProvider *p = Ready(NO); [p consumeRateLimits:Quota(@YES)];
        Check(p.snapshot.fiveHourRemainingPercent == 75 && p.snapshot.weeklyRemainingPercent == 60, "official percentage remains direct");
        Check(p.samples.count == 1 && p.samples.lastObject[@"w"] != nil, "live samples only");
        [p consumeRateLimits:@{@"rateLimits": @{@"primary": NSNull.null, @"secondary": NSNull.null}, @"ordinaryUsageAllowed": NSNull.null}];
        Check(p.snapshot.weeklyAvailable && p.snapshot.weeklyRemainingPercent == 60 && [p.snapshot.weeklyDataState isEqual:@"previous"], "nullable quota retains valid previous");
        Check(p.samples.count == 1 && p.snapshot.ordinaryUsageAllowed == nil, "no synthetic forecast sample or inferred allowance");
        [p consumeRateLimits:Quota(@0)]; Check(p.snapshot.ordinaryUsageAllowed == nil, "numeric zero not official false");
        [p consumeRateLimits:Quota(nil)]; Check(p.snapshot.ordinaryUsageAllowed == nil, "missing allowance not inferred from remaining quota");
        [p recordFailure:@"timeout" requestID:2];
        Check(p.snapshot.weeklyRemainingPercent == 60 && p.snapshot.weeklyAvailable && [p.snapshot.quotaErrorText containsString:@"上次数据"], "transport failure retains balance");
        p.snapshot.weeklyResetAt = 1; p.snapshot.fiveHourResetAt = 1;
        [p recordFailure:@"network" requestID:2];
        Check(!p.snapshot.weeklyAvailable && [p.snapshot.weeklyDataState isEqual:@"expired"], "expired prior cycle not live");
        [p consumeRateLimits:Quota(@YES)]; [p consumeRateLimits:@{@"rateLimits": @[], @"ordinaryUsageAllowed": @YES}];
        Check([p.snapshot.interfaceFailureKinds[@"2"] isEqual:@"protocol"] && p.snapshot.weeklyRemainingPercent == 60, "malformed quota domain retains value");
        [p consumeRateLimits:@{@"rateLimits": @{@"primary": @{@"usedPercent": NSNull.null, @"resetsAt": NSNull.null, @"windowDurationMins": NSNull.null}}}];
        Check(p.snapshot.weeklyRemainingPercent == 60 && p.snapshot.fiveHourRemainingPercent == 75, "nested null never becomes zero usage");

        NSDictionary *thread = @{@"id": @"SYNTHETIC-ID", @"name": @"SYNTHETIC task", @"recencyAt": @100, @"status": @{@"type": @"notLoaded"}, @"environments": NSNull.null, @"turns": @[@{@"type": @"functionCallOutput", @"output": @"SYNTHETIC_PRIVATE"}]};
        [p consumeThreads:@{@"data": @[thread]}];
        Check(p.snapshot.recentTaskCount == 1 && ![p.snapshot.recentTasks.description containsString:@"SYNTHETIC_PRIVATE"], "metadata projection discards contents");
        p.snapshot.activeTaskCount = 2;
        [p handleObject:@{@"method": @"thread/closed", @"params": @{@"threadId": @"SYNTHETIC-ID"}}];
        [p handleObject:@{@"method": @"item/completed", @"params": @{@"type": @"futureType"}}];
        [p handleObject:@{@"id": NSNull.null}];
        Check(p.snapshot.activeTaskCount == 2 && p.snapshot.recentTaskCount == 1, "unload notifications not task completion");
        [p consumeThreads:@{@"data": NSNull.null}];
        Check(p.snapshot.recentTaskCount == 1 && [p.snapshot.interfaceFailureKinds[@"5"] isEqual:@"protocol"], "malformed list not empty tasks");
        [p consumeThreads:@{@"data": @[@{@"id": NSNull.null}]}];
        Check(p.snapshot.recentTaskCount == 1, "invalid thread identity not silent disappearance");
        [p consumeThreads:@{@"data": @[]}];
        Check(p.snapshot.recentTaskCount == 0 && !p.snapshot.interfaceFailureKinds[@"5"], "valid empty list authoritative");
        p.snapshot.accountAvailable = YES; p.snapshot.planType = @"pro"; p.snapshot.usageAvailable = YES; p.snapshot.sevenDayTokens = 500;
        [p consumeAccount:@{@"account": @[]}]; [p consumeUsage:@{@"summary": @[]}];
        Check([p.snapshot.planType isEqual:@"pro"] && p.snapshot.sevenDayTokens == 500, "malformed account/usage retain values");
        [p consumeUsage:@{@"summary": @{@"lifetimeTokens": NSNull.null, @"currentStreakDays": NSNull.null}}];
        Check(p.snapshot.lifetimeTokens == 0 && p.snapshot.currentStreakDays == 0, "nullable usage summary safe");

        FixtureProvider *partial = Ready(NO);
        [partial handleObject:@{@"id": @2, @"result": Quota(@YES)}];
        [partial handleObject:@{@"id": @4, @"error": @{@"code": @(-32601)}}];
        Check(partial.snapshot.weeklyAvailable && !partial.snapshot.quotaErrorText && [partial.snapshot.interfaceFailureKinds[@"4"] isEqual:@"unsupported"], "single failed module isolated");
        FixtureProvider *backoff = [FixtureProvider new];
        for (NSNumber *interval in @[@60, @120, @240, @300, @300]) {
            [backoff recordFailure:@"network" requestID:2]; [backoff finishFetch];
            Check(backoff.recommendedRetryInterval == interval.doubleValue, "bounded transport backoff");
        }
        backoff.snapshot.interfaceFailureKinds = @{}; [backoff finishFetch];
        Check(backoff.recommendedRetryInterval == 0, "backoff recovery");
        [backoff recordFailure:@"unsupported" requestID:4]; [backoff finishFetch];
        Check(backoff.recommendedRetryInterval == 300, "unsupported does not increase polling");
        FixtureProvider *missing = [FixtureProvider new]; [missing start];
        Check(missing.recommendedRetryInterval == 300 && missing.snapshot.interfaceFailureKinds.count == 4, "missing executable bounded retry");

        FixtureProvider *json = Ready(NO); [json consumeData:[@"not-json\n" dataUsingEncoding:NSUTF8StringEncoding]];
        Check([json.snapshot.interfaceFailureKinds[@"2"] isEqual:@"protocol"] && [[json valueForKey:@"outputBuffer"] length] == 0, "invalid JSON clears bounded buffer");
        FixtureProvider *large = Ready(NO); [large consumeData:[NSMutableData dataWithLength:4 * 1024 * 1024 + 1]];
        Check([large.snapshot.interfaceFailureKinds[@"2"] isEqual:@"protocol"] && [[large valueForKey:@"outputBuffer"] length] == 0, "oversized response bounded");
        FixtureProvider *split = Ready(NO);
        NSData *wire = [NSJSONSerialization dataWithJSONObject:@{@"id": @2, @"result": Quota(@YES)} options:0 error:nil];
        NSMutableData *line = [wire mutableCopy]; [line appendData:[@"\n" dataUsingEncoding:NSUTF8StringEncoding]];
        [split consumeData:[line subdataWithRange:NSMakeRange(0, 5)]];
        Check(!split.snapshot.quotaAvailable, "partial frame not parsed early");
        [split consumeData:[line subdataWithRange:NSMakeRange(5, line.length - 5)]];
        Check(split.snapshot.weeklyRemainingPercent == 60, "split frame reassembled");
        if (argc > 1) {
            NSString *directory = [NSString stringWithUTF8String:argv[1]];
            FixtureProvider *pipe = [FixtureProvider new]; pipe.realIO = YES;
            pipe.fixtureExecutable = [directory stringByAppendingPathComponent:@"legacy"];
            [pipe refreshQuotaInBackground]; WaitForFetch(pipe);
            Check(pipe.snapshot.weeklyRemainingPercent == 60 && [[pipe valueForKey:@"legacyQuotaParameters"] boolValue], "real pipe legacy handshake");
            [pipe.sent removeAllObjects]; [pipe refreshQuotaInBackground]; WaitForFetch(pipe);
            Check(pipe.sent.count == 6 && !pipe.sent[2][@"params"], "next background read uses cached legacy capability");
            pipe.fixtureExecutable = [directory stringByAppendingPathComponent:@"modern"];
            [pipe.sent removeAllObjects]; [pipe refreshQuotaInBackground]; WaitForFetch(pipe);
            Check([pipe.sent[2][@"params"] isEqual:@{@"excludeResetCreditDetails": @YES}], "new executable re-probes lightweight support");
            [pipe.sent removeAllObjects]; [pipe refreshQuota]; WaitForFetch(pipe);
            Check(!pipe.sent[2][@"params"], "real manual read keeps full details");
            pipe.fixtureExecutable = [directory stringByAppendingPathComponent:@"eof"];
            [pipe start]; WaitForFetch(pipe);
            Check([pipe.snapshot.interfaceFailureKinds[@"2"] isEqual:@"network"] && pipe.recommendedRetryInterval == 60 && pipe.snapshot.weeklyRemainingPercent == 60, "unexpected process exit retains state and backs off");
            pipe.fixtureExecutable = [directory stringByAppendingPathComponent:@"timeout"];
            [pipe start]; WaitForFetch(pipe);
            Check([pipe.snapshot.interfaceFailureKinds[@"2"] isEqual:@"timeout"] && pipe.recommendedRetryInterval == 120, "timeout kills only owned fixture and backs off");
            pipe.fixtureExecutable = [directory stringByAppendingPathComponent:@"modern"];
            [pipe start]; WaitForFetch(pipe);
            Check(pipe.snapshot.interfaceFailureKinds.count == 0 && pipe.recommendedRetryInterval == 0, "transport recovery after timeout");
        }
        printf("codex_protocol_checks=%lu failures=%lu\n", (unsigned long)checks, (unsigned long)failures);
        return failures ? 1 : 0;
    }
}
