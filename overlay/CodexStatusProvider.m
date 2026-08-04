#import "CodexStatusProvider.h"

NSDictionary<NSString *, id> *CodexCalendarUsage(NSArray *buckets, NSDate *now) {
    NSMutableDictionary<NSString *, NSNumber *> *tokensByDate = [NSMutableDictionary dictionary];
    for (NSDictionary *bucket in buckets ?: @[]) {
        NSString *date = [bucket[@"startDate"] isKindOfClass:NSString.class] ? bucket[@"startDate"] : nil;
        if (date.length == 0) continue;
        long long total = [tokensByDate[date] longLongValue] + [bucket[@"tokens"] longLongValue];
        tokensByDate[date] = @(total);
    }
    NSCalendar *calendar = [NSCalendar calendarWithIdentifier:NSCalendarIdentifierGregorian];
    calendar.timeZone = NSTimeZone.localTimeZone;
    NSDateFormatter *dateFormatter = [NSDateFormatter new];
    dateFormatter.calendar = calendar; dateFormatter.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
    dateFormatter.timeZone = calendar.timeZone; dateFormatter.dateFormat = @"yyyy-MM-dd";
    long long today = 0, recent = 0, previous = 0;
    NSDate *startOfToday = [calendar startOfDayForDate:now ?: NSDate.date];
    NSString *todayKey = [dateFormatter stringFromDate:startOfToday];
    BOOL todayAvailable = tokensByDate[todayKey] != nil;
    NSString *latestDate = nil;
    for (NSString *date in tokensByDate) {
        NSDate *parsed = [dateFormatter dateFromString:date];
        if (!parsed || ![[dateFormatter stringFromDate:parsed] isEqualToString:date] || [date compare:todayKey] == NSOrderedDescending) continue;
        if (!latestDate || [date compare:latestDate] == NSOrderedDescending) latestDate = date;
    }
    NSDate *anchorDate = latestDate ? [dateFormatter dateFromString:latestDate] : startOfToday;
    for (NSInteger dayOffset = 0; dayOffset < 14; dayOffset++) {
        NSDate *date = [calendar dateByAddingUnit:NSCalendarUnitDay value:-dayOffset toDate:anchorDate options:0];
        long long tokens = [tokensByDate[[dateFormatter stringFromDate:date]] longLongValue];
        if (dayOffset < 7) recent += tokens; else previous += tokens;
    }
    if (todayAvailable) today = [tokensByDate[todayKey] longLongValue];
    long long latestTokens = latestDate ? [tokensByDate[latestDate] longLongValue] : 0;
    return @{ @"today": @(today), @"todayAvailable": @(todayAvailable), @"recent": @(recent), @"previous": @(previous),
              @"latestDate": latestDate ?: @"", @"latestTokens": @(latestTokens) };
}

@implementation CodexStatusSnapshot
@end

@interface CodexStatusProvider ()
@property(nonatomic, strong) NSTask *task;
@property(nonatomic, strong) NSPipe *inputPipe;
@property(nonatomic, strong) NSPipe *outputPipe;
@property(nonatomic, strong) NSMutableData *outputBuffer;
@property(nonatomic, readwrite) CodexStatusSnapshot *snapshot;
@property(nonatomic) BOOL initialized;
@property(nonatomic) BOOL receivedQuota;
@property(nonatomic) BOOL receivedAccount;
@property(nonatomic) BOOL receivedUsage;
@property(nonatomic) BOOL intentionalStop;
- (void)finishFetch;
- (void)maybeFinishFetch;
@end

@implementation CodexStatusProvider

- (instancetype)init {
    self = [super init];
    if (!self) return nil;
    _snapshot = [CodexStatusSnapshot new];
    _snapshot.statusText = @"正在连接本机Codex";
    _outputBuffer = [NSMutableData data];
    return self;
}

- (NSString *)codexExecutable {
    NSArray<NSString *> *paths = @[
        @"/Applications/ChatGPT.app/Contents/Resources/codex",
        [NSHomeDirectory() stringByAppendingPathComponent:@"Applications/ChatGPT.app/Contents/Resources/codex"],
        @"/Applications/Codex.app/Contents/Resources/codex"
    ];
    for (NSString *path in paths) if ([[NSFileManager defaultManager] isExecutableFileAtPath:path]) return path;
    return nil;
}

- (void)start {
    if (self.task.running) return;
    NSString *executable = [self codexExecutable];
    if (!executable) {
        self.snapshot.statusText = @"未找到Codex本机接口";
        self.snapshot.quotaErrorText = @"未找到Codex本机接口";
        self.snapshot.accountErrorText = @"未找到Codex本机接口";
        self.snapshot.usageErrorText = @"未找到Codex本机接口";
        [self notifyUpdate];
        return;
    }

    self.initialized = NO;
    self.receivedQuota = NO;
    self.receivedAccount = NO;
    self.receivedUsage = NO;
    self.intentionalStop = NO;
    self.outputBuffer.length = 0;
    self.inputPipe = [NSPipe pipe];
    self.outputPipe = [NSPipe pipe];
    self.task = [NSTask new];
    self.task.executableURL = [NSURL fileURLWithPath:executable];
    self.task.arguments = @[@"app-server", @"--stdio"];
    self.task.standardInput = self.inputPipe;
    self.task.standardOutput = self.outputPipe;
    self.task.standardError = [NSFileHandle fileHandleWithNullDevice];
    __weak typeof(self) weakSelf = self;
    self.outputPipe.fileHandleForReading.readabilityHandler = ^(NSFileHandle *handle) {
        NSData *data = handle.availableData;
        if (data.length == 0) return;
        dispatch_async(dispatch_get_main_queue(), ^{ [weakSelf consumeData:data]; });
    };
    self.task.terminationHandler = ^(NSTask *task) {
        dispatch_async(dispatch_get_main_queue(), ^{
            if (weakSelf.task != task) return;
            weakSelf.initialized = NO;
            if (!weakSelf.intentionalStop) {
                weakSelf.snapshot.statusText = @"Codex接口读取失败，稍后重试";
                if (!weakSelf.receivedQuota) weakSelf.snapshot.quotaErrorText = @"更新失败，显示上次数据";
                if (!weakSelf.receivedAccount) weakSelf.snapshot.accountErrorText = @"更新失败，显示上次数据";
                if (!weakSelf.receivedUsage) weakSelf.snapshot.usageErrorText = @"更新失败，显示上次数据";
                [weakSelf notifyUpdate];
            }
        });
    };
    NSError *error = nil;
    if (![self.task launchAndReturnError:&error]) {
        self.snapshot.statusText = @"无法启动Codex本机接口";
        self.snapshot.quotaErrorText = @"无法启动本机接口";
        self.snapshot.accountErrorText = @"无法启动本机接口";
        self.snapshot.usageErrorText = @"无法启动本机接口";
        [self notifyUpdate];
        return;
    }
    [self sendObject:@{
        @"id": @1,
        @"method": @"initialize",
        @"params": @{ @"clientInfo": @{ @"name": @"codex-monitor-hud", @"version": @"0.7.0" } }
    }];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(8 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        if (weakSelf.task.running) {
            if (!weakSelf.receivedQuota) { weakSelf.snapshot.statusText = @"额度读取超时，稍后重试"; weakSelf.snapshot.quotaErrorText = @"更新超时，显示上次数据"; }
            if (!weakSelf.receivedAccount) weakSelf.snapshot.accountErrorText = @"更新超时，显示上次数据";
            if (!weakSelf.receivedUsage) weakSelf.snapshot.usageErrorText = @"更新超时，显示上次数据";
            [weakSelf notifyUpdate];
            [weakSelf finishFetch];
        }
    });
}

- (void)consumeData:(NSData *)data {
    [self.outputBuffer appendData:data];
    while (YES) {
        const uint8_t *bytes = self.outputBuffer.bytes;
        NSUInteger length = self.outputBuffer.length;
        NSUInteger newline = NSNotFound;
        for (NSUInteger index = 0; index < length; index++) {
            if (bytes[index] == '\n') { newline = index; break; }
        }
        if (newline == NSNotFound) break;
        NSData *line = [self.outputBuffer subdataWithRange:NSMakeRange(0, newline)];
        [self.outputBuffer replaceBytesInRange:NSMakeRange(0, newline + 1) withBytes:NULL length:0];
        if (line.length == 0) continue;
        NSDictionary *object = [NSJSONSerialization JSONObjectWithData:line options:0 error:nil];
        if ([object isKindOfClass:NSDictionary.class]) [self handleObject:object];
    }
}

- (void)handleObject:(NSDictionary *)object {
    NSInteger requestID = [object[@"id"] integerValue];
    NSDictionary *result = [object[@"result"] isKindOfClass:NSDictionary.class] ? object[@"result"] : nil;
    if (requestID == 1 && result) {
        self.initialized = YES;
        [self sendObject:@{ @"method": @"initialized", @"params": @{} }];
        [self sendObject:@{ @"id": @2, @"method": @"account/rateLimits/read", @"params": NSNull.null }];
        [self sendObject:@{ @"id": @3, @"method": @"account/read", @"params": @{ @"refreshToken": @NO } }];
        [self sendObject:@{ @"id": @4, @"method": @"account/usage/read", @"params": NSNull.null }];
        return;
    }
    if (requestID == 2 && result) {
        [self consumeRateLimits:result];
        self.receivedQuota = YES;
        [self maybeFinishFetch];
        return;
    }
    if (requestID == 2 && object[@"error"]) {
        self.snapshot.statusText = @"额度接口返回错误，稍后重试";
        self.snapshot.quotaErrorText = @"更新失败，显示上次数据";
        self.receivedQuota = YES;
        [self notifyUpdate];
        [self maybeFinishFetch];
        return;
    }
    if (requestID == 3) {
        if (result) [self consumeAccount:result]; else self.snapshot.accountErrorText = @"更新失败，显示上次数据";
        self.receivedAccount = YES; [self notifyUpdate]; [self maybeFinishFetch]; return;
    }
    if (requestID == 4) {
        if (result) [self consumeUsage:result]; else self.snapshot.usageErrorText = @"更新失败，显示上次数据";
        self.receivedUsage = YES; [self notifyUpdate]; [self maybeFinishFetch]; return;
    }
}

- (void)consumeRateLimits:(NSDictionary *)result {
    NSDictionary *buckets = [result[@"rateLimitsByLimitId"] isKindOfClass:NSDictionary.class] ? result[@"rateLimitsByLimitId"] : nil;
    NSDictionary *rate = [buckets[@"codex"] isKindOfClass:NSDictionary.class] ? buckets[@"codex"] : result[@"rateLimits"];
    if (![rate isKindOfClass:NSDictionary.class]) {
        self.snapshot.statusText = @"额度当前未返回";
        [self notifyUpdate];
        return;
    }
    self.snapshot.quotaAvailable = YES;
    self.snapshot.quotaUpdatedAt = NSDate.date.timeIntervalSince1970;
    self.snapshot.quotaErrorText = nil;
    self.snapshot.fiveHourAvailable = NO;
    self.snapshot.weeklyAvailable = NO;
    self.snapshot.modelQuotaAvailable = NO;
    NSString *plan = [rate[@"planType"] isKindOfClass:NSString.class] ? rate[@"planType"] : nil;
    if (plan.length > 0) { self.snapshot.accountAvailable = YES; self.snapshot.planType = plan; }
    for (id value in @[rate[@"primary"] ?: NSNull.null, rate[@"secondary"] ?: NSNull.null]) {
        if (![value isKindOfClass:NSDictionary.class]) continue;
        NSDictionary *window = value;
        NSNumber *used = window[@"usedPercent"];
        NSNumber *duration = window[@"windowDurationMins"];
        if (![used isKindOfClass:NSNumber.class]) continue;
        double remaining = MAX(0, MIN(100, 100.0 - used.doubleValue));
        NSTimeInterval reset = [window[@"resetsAt"] doubleValue];
        if ([duration isKindOfClass:NSNumber.class] && duration.doubleValue <= 24.0 * 60.0) {
            self.snapshot.fiveHourAvailable = YES;
            self.snapshot.fiveHourRemainingPercent = remaining;
            self.snapshot.fiveHourResetAt = reset;
        } else {
            self.snapshot.weeklyAvailable = YES;
            self.snapshot.weeklyRemainingPercent = remaining;
            self.snapshot.weeklyResetAt = reset;
        }
    }
    for (NSString *key in [buckets.allKeys sortedArrayUsingSelector:@selector(localizedCaseInsensitiveCompare:)]) {
        if ([key isEqualToString:@"codex"]) continue;
        NSDictionary *bucket = [buckets[key] isKindOfClass:NSDictionary.class] ? buckets[key] : nil;
        if (!bucket) continue;
        NSDictionary *window = [bucket[@"primary"] isKindOfClass:NSDictionary.class] ? bucket[@"primary"] : ([bucket[@"secondary"] isKindOfClass:NSDictionary.class] ? bucket[@"secondary"] : nil);
        NSNumber *used = [window[@"usedPercent"] isKindOfClass:NSNumber.class] ? window[@"usedPercent"] : nil;
        if (!used) continue;
        NSString *name = [bucket[@"limitName"] isKindOfClass:NSString.class] ? bucket[@"limitName"] : key;
        NSNumber *duration = [window[@"windowDurationMins"] isKindOfClass:NSNumber.class] ? window[@"windowDurationMins"] : nil;
        self.snapshot.modelQuotaAvailable = YES;
        self.snapshot.modelQuotaName = name.length > 0 ? name : key;
        self.snapshot.modelQuotaRemainingPercent = MAX(0, MIN(100, 100.0 - used.doubleValue));
        self.snapshot.modelQuotaResetAt = [window[@"resetsAt"] doubleValue];
        self.snapshot.modelQuotaWindowLabel = duration.doubleValue <= 24.0 * 60.0 ? @"短周期" : @"每周";
        break;
    }
    self.snapshot.updatedAt = NSDate.date.timeIntervalSince1970;
    self.snapshot.statusText = @"Codex额度已更新";
    [self notifyUpdate];
}

- (void)consumeAccount:(NSDictionary *)result {
    NSDictionary *account = [result[@"account"] isKindOfClass:NSDictionary.class] ? result[@"account"] : nil;
    NSString *plan = [account[@"planType"] isKindOfClass:NSString.class] ? account[@"planType"] : nil;
    if (plan.length > 0) {
        self.snapshot.accountAvailable = YES; self.snapshot.planType = plan;
        self.snapshot.accountUpdatedAt = NSDate.date.timeIntervalSince1970; self.snapshot.accountErrorText = nil;
    } else self.snapshot.accountErrorText = @"订阅当前未返回";
}

- (void)consumeUsage:(NSDictionary *)result {
    NSArray *buckets = [result[@"dailyUsageBuckets"] isKindOfClass:NSArray.class] ? result[@"dailyUsageBuckets"] : nil;
    NSDictionary *summary = [result[@"summary"] isKindOfClass:NSDictionary.class] ? result[@"summary"] : nil;
    if (!buckets && !summary) { self.snapshot.usageErrorText = @"用量当前未返回"; return; }
    NSDictionary<NSString *, id> *usage = CodexCalendarUsage(buckets, NSDate.date);
    self.snapshot.usageAvailable = YES;
    self.snapshot.usageUpdatedAt = NSDate.date.timeIntervalSince1970;
    self.snapshot.usageErrorText = nil;
    self.snapshot.todayTokens = [usage[@"today"] longLongValue];
    self.snapshot.todayUsageAvailable = [usage[@"todayAvailable"] boolValue];
    self.snapshot.sevenDayTokens = [usage[@"recent"] longLongValue];
    self.snapshot.previousSevenDayTokens = [usage[@"previous"] longLongValue];
    self.snapshot.latestUsageDate = usage[@"latestDate"];
    self.snapshot.latestUsageTokens = [usage[@"latestTokens"] longLongValue];
    self.snapshot.lifetimeTokens = [summary[@"lifetimeTokens"] longLongValue];
    self.snapshot.currentStreakDays = [summary[@"currentStreakDays"] integerValue];
    NSNumber *longestTurn = [summary[@"longestRunningTurnSec"] isKindOfClass:NSNumber.class] ? summary[@"longestRunningTurnSec"] : nil;
    self.snapshot.longestRunningTurnAvailable = longestTurn != nil;
    self.snapshot.longestRunningTurnSec = longestTurn.integerValue;
    NSNumber *longestStreak = [summary[@"longestStreakDays"] isKindOfClass:NSNumber.class] ? summary[@"longestStreakDays"] : nil;
    self.snapshot.longestStreakAvailable = longestStreak != nil;
    self.snapshot.longestStreakDays = longestStreak.integerValue;
}

- (void)sendObject:(NSDictionary *)object {
    if (!self.task.running) return;
    NSData *json = [NSJSONSerialization dataWithJSONObject:object options:0 error:nil];
    if (!json) return;
    NSMutableData *line = [json mutableCopy];
    uint8_t newline = '\n';
    [line appendBytes:&newline length:1];
    @try { [self.inputPipe.fileHandleForWriting writeData:line]; } @catch (__unused NSException *exception) {}
}

- (void)refreshQuota {
    if (!self.task.running) [self start];
}

- (void)notifyUpdate {
    if (self.updateHandler) self.updateHandler();
}

- (void)maybeFinishFetch {
    if (self.receivedQuota && self.receivedAccount && self.receivedUsage) [self finishFetch];
}

- (void)stop {
    self.intentionalStop = YES;
    self.outputPipe.fileHandleForReading.readabilityHandler = nil;
    if (self.task.running) [self.task terminate];
    self.task = nil;
}

- (void)finishFetch {
    self.intentionalStop = YES;
    self.outputPipe.fileHandleForReading.readabilityHandler = nil;
    if (self.task.running) [self.task terminate];
    self.task = nil;
    self.initialized = NO;
}

@end
