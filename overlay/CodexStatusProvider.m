#import "CodexStatusProvider.h"
#import "CodexCostHistory.h"

static NSTimeInterval const CodexActivityWindow = 120.0;
static unsigned long long const CodexActivityTailBytes = 1024 * 1024;
static NSUInteger const CodexActivityMaxCandidateFiles = 64;

static NSDate *CodexParseTimestamp(id value) {
    if (![value isKindOfClass:NSString.class] || [value length] == 0) return nil;
    static NSISO8601DateFormatter *fractionalFormatter;
    static NSISO8601DateFormatter *wholeSecondFormatter;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        fractionalFormatter = [NSISO8601DateFormatter new];
        fractionalFormatter.formatOptions = NSISO8601DateFormatWithInternetDateTime | NSISO8601DateFormatWithFractionalSeconds;
        wholeSecondFormatter = [NSISO8601DateFormatter new];
        wholeSecondFormatter.formatOptions = NSISO8601DateFormatWithInternetDateTime;
    });
    NSDate *date = [fractionalFormatter dateFromString:value];
    if (date) return date;
    return [wholeSecondFormatter dateFromString:value];
}

NSDictionary<NSString *, id> *CodexActivityStateFromJSONLLinesWithFallback(NSArray<NSString *> *lines, NSDate *modifiedAt, NSDate *now, NSDate *fallbackStart) {
    NSDate *latestStart = nil;
    NSDate *latestFinish = nil;
    for (NSString *line in lines ?: @[]) {
        if (![line isKindOfClass:NSString.class] || line.length == 0) continue;
        NSData *data = [line dataUsingEncoding:NSUTF8StringEncoding];
        NSDictionary *object = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
        if (![object isKindOfClass:NSDictionary.class]) continue;
        NSDate *timestamp = CodexParseTimestamp(object[@"timestamp"]);
        if (!timestamp) continue;
        NSString *type = [object[@"type"] isKindOfClass:NSString.class] ? object[@"type"] : @"";
        NSDictionary *payload = [object[@"payload"] isKindOfClass:NSDictionary.class] ? object[@"payload"] : nil;
        NSString *payloadType = [payload[@"type"] isKindOfClass:NSString.class] ? payload[@"type"] : @"";
        BOOL startsTurn = NO;
        BOOL finishesTurn = NO;
        if ([type isEqualToString:@"response_item"] && [payloadType isEqualToString:@"message"]) {
            NSString *role = [payload[@"role"] isKindOfClass:NSString.class] ? payload[@"role"] : @"";
            NSString *phase = [payload[@"phase"] isKindOfClass:NSString.class] ? payload[@"phase"] : @"";
            startsTurn = [role isEqualToString:@"user"];
            finishesTurn = [role isEqualToString:@"assistant"] && [phase isEqualToString:@"final_answer"];
        } else if ([type isEqualToString:@"event_msg"]) {
            startsTurn = [@[@"task_started", @"turn_started", @"user_message"] containsObject:payloadType];
            finishesTurn = [@[@"task_complete", @"turn_completed"] containsObject:payloadType];
        }
        if (startsTurn && (!latestStart || [timestamp compare:latestStart] == NSOrderedDescending)) latestStart = timestamp;
        if (finishesTurn && (!latestFinish || [timestamp compare:latestFinish] == NSOrderedDescending)) latestFinish = timestamp;
    }
    NSDate *referenceNow = now ?: NSDate.date;
    NSDate *effectiveStart = latestStart;
    if (fallbackStart && (!effectiveStart || [fallbackStart compare:effectiveStart] == NSOrderedDescending)) effectiveStart = fallbackStart;
    BOOL recentlyWritten = modifiedAt && [referenceNow timeIntervalSinceDate:modifiedAt] <= CodexActivityWindow;
    BOOL unfinished = effectiveStart && (!latestFinish || [effectiveStart compare:latestFinish] == NSOrderedDescending);
    BOOL active = recentlyWritten && unfinished;
    NSInteger duration = active ? MAX(0, (NSInteger)[referenceNow timeIntervalSinceDate:effectiveStart]) : 0;
    return @{ @"active": @(active), @"startedAt": @(effectiveStart.timeIntervalSince1970), @"finishedAt": @(latestFinish.timeIntervalSince1970), @"durationSec": @(duration) };
}

NSDictionary<NSString *, id> *CodexActivityStateFromJSONLLines(NSArray<NSString *> *lines, NSDate *modifiedAt, NSDate *now) {
    return CodexActivityStateFromJSONLLinesWithFallback(lines, modifiedAt, now, nil);
}

static NSArray<NSString *> *CodexTailLines(NSURL *fileURL) {
    NSFileHandle *handle = [NSFileHandle fileHandleForReadingFromURL:fileURL error:nil];
    if (!handle) return nil;
    unsigned long long size = [handle seekToEndOfFile];
    unsigned long long offset = size > CodexActivityTailBytes ? size - CodexActivityTailBytes : 0;
    [handle seekToFileOffset:offset];
    NSData *data = [handle readDataToEndOfFile];
    [handle closeFile];
    NSString *text = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
    if (!text) return nil;
    NSMutableArray<NSString *> *lines = [[text componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet] mutableCopy];
    if (offset > 0 && lines.count > 0) [lines removeObjectAtIndex:0];
    return lines;
}

static NSString *CodexSessionIDFromFilename(NSString *filename) {
    NSString *stem = filename ?: @"";
    while ([stem.pathExtension.lowercaseString isEqualToString:@"zst"] || [stem.pathExtension.lowercaseString isEqualToString:@"jsonl"]) {
        stem = stem.stringByDeletingPathExtension;
    }
    if (stem.length < 36) return nil;
    NSString *candidate = [stem substringFromIndex:stem.length - 36];
    return [[NSUUID alloc] initWithUUIDString:candidate] ? candidate.lowercaseString : nil;
}

NSDictionary<NSString *, id> *CodexNormalizedThreadMetadata(NSDictionary *thread) {
    if (![thread isKindOfClass:NSDictionary.class]) return @{};
    NSString *threadID = [thread[@"id"] isKindOfClass:NSString.class] ? [thread[@"id"] lowercaseString] : @"";
    NSString *name = [thread[@"name"] isKindOfClass:NSString.class] ? thread[@"name"] : @"";
    NSNumber *recencyAt = [thread[@"recencyAt"] isKindOfClass:NSNumber.class] ? thread[@"recencyAt"] : nil;
    if (!recencyAt && [thread[@"updatedAt"] isKindOfClass:NSNumber.class]) recencyAt = thread[@"updatedAt"];
    NSString *path = [thread[@"path"] isKindOfClass:NSString.class] ? thread[@"path"] : @"";
    NSString *cwd = [thread[@"cwd"] isKindOfClass:NSString.class] ? thread[@"cwd"] : @"";
    return @{ @"id": threadID, @"name": name, @"recencyAt": recencyAt ?: @0, @"path": path, @"cwd": cwd };
}

static NSTimeInterval CodexUnixTimestamp(NSNumber *value) {
    NSTimeInterval timestamp = [value isKindOfClass:NSNumber.class] ? value.doubleValue : 0;
    if (timestamp > 1000000000000.0) timestamp /= 1000.0;
    return timestamp;
}

static BOOL CodexIsPlainRolloutPath(NSString *path) {
    return [path.lowercaseString hasSuffix:@".jsonl"];
}

NSDictionary<NSString *, id> *CodexScanRecentActivityAtRoot(NSDictionary<NSString *, NSDictionary *> *metadataByID, NSDate *now, NSURL *root) {
    NSDate *referenceNow = now ?: NSDate.date;
    BOOL rootAvailable = root.path.length > 0 && [NSFileManager.defaultManager fileExistsAtPath:root.path];
    NSCalendar *calendar = [NSCalendar calendarWithIdentifier:NSCalendarIdentifierGregorian];
    calendar.timeZone = NSTimeZone.localTimeZone;
    NSDateFormatter *folderFormatter = [NSDateFormatter new];
    folderFormatter.calendar = calendar;
    folderFormatter.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
    folderFormatter.timeZone = calendar.timeZone;
    folderFormatter.dateFormat = @"yyyy/MM/dd";
    NSMutableArray<NSDictionary *> *candidates = [NSMutableArray array];
    NSMutableSet<NSString *> *seenPaths = [NSMutableSet set];
    NSMutableSet<NSString *> *unresolvedRecentThreadIDs = [NSMutableSet set];
    BOOL (^addCandidate)(NSURL *) = ^BOOL(NSURL *file) {
        if (!file.path.length || [seenPaths containsObject:file.path] || !CodexIsPlainRolloutPath(file.path)) return NO;
        NSDictionary *values = [file resourceValuesForKeys:@[NSURLContentModificationDateKey, NSURLIsRegularFileKey] error:nil];
        if (![values[NSURLIsRegularFileKey] boolValue]) return NO;
        NSDate *modifiedAt = values[NSURLContentModificationDateKey];
        if (!modifiedAt || [referenceNow timeIntervalSinceDate:modifiedAt] > CodexActivityWindow) return NO;
        [seenPaths addObject:file.path];
        [candidates addObject:@{ @"url": file, @"modifiedAt": modifiedAt }];
        return YES;
    };
    [metadataByID enumerateKeysAndObjectsUsingBlock:^(NSString *threadID, NSDictionary *metadata, __unused BOOL *stop) {
        NSString *path = [metadata[@"path"] isKindOfClass:NSString.class] ? metadata[@"path"] : nil;
        NSTimeInterval recencyAt = CodexUnixTimestamp(metadata[@"recencyAt"]);
        NSTimeInterval age = referenceNow.timeIntervalSince1970 - recencyAt;
        BOOL recentlyUpdated = recencyAt > 0 && age >= -CodexActivityWindow && age <= CodexActivityWindow;
        BOOL added = path.length > 0 && addCandidate([NSURL fileURLWithPath:path]);
        if (recentlyUpdated && !added && threadID.length > 0) [unresolvedRecentThreadIDs addObject:threadID];
    }];
    if (rootAvailable) {
        for (NSInteger dayOffset = 0; dayOffset <= 1; dayOffset++) {
            NSDate *day = [calendar dateByAddingUnit:NSCalendarUnitDay value:-dayOffset toDate:referenceNow options:0];
            NSURL *directory = [root URLByAppendingPathComponent:[folderFormatter stringFromDate:day] isDirectory:YES];
            NSArray<NSURL *> *files = [NSFileManager.defaultManager contentsOfDirectoryAtURL:directory includingPropertiesForKeys:@[NSURLContentModificationDateKey, NSURLIsRegularFileKey] options:NSDirectoryEnumerationSkipsHiddenFiles error:nil];
            for (NSURL *file in files ?: @[]) {
                if (![file.lastPathComponent hasPrefix:@"rollout-"]) continue;
                if (CodexIsPlainRolloutPath(file.path)) addCandidate(file);
                else if ([file.path.lowercaseString hasSuffix:@".jsonl.zst"]) {
                    NSDictionary *values = [file resourceValuesForKeys:@[NSURLContentModificationDateKey, NSURLIsRegularFileKey] error:nil];
                    NSDate *modifiedAt = values[NSURLContentModificationDateKey];
                    NSString *sessionID = CodexSessionIDFromFilename(file.lastPathComponent);
                    if ([values[NSURLIsRegularFileKey] boolValue] && modifiedAt && [referenceNow timeIntervalSinceDate:modifiedAt] <= CodexActivityWindow && sessionID.length > 0) {
                        [unresolvedRecentThreadIDs addObject:sessionID];
                    }
                }
            }
        }
    }
    for (NSDictionary *candidate in candidates) {
        NSURL *candidateURL = candidate[@"url"];
        NSString *sessionID = CodexSessionIDFromFilename(candidateURL.lastPathComponent);
        if (sessionID.length > 0) [unresolvedRecentThreadIDs removeObject:sessionID];
    }
    [candidates sortUsingComparator:^NSComparisonResult(NSDictionary *left, NSDictionary *right) {
        return [right[@"modifiedAt"] compare:left[@"modifiedAt"]];
    }];
    NSMutableArray<NSString *> *activeNames = [NSMutableArray array];
    NSInteger activeCount = 0;
    NSInteger longest = 0;
    NSInteger readableCandidateCount = 0;
    NSUInteger limit = MIN(CodexActivityMaxCandidateFiles, candidates.count);
    for (NSUInteger index = 0; index < limit; index++) {
        NSDictionary *candidate = candidates[index];
        NSURL *url = candidate[@"url"];
        NSString *sessionID = CodexSessionIDFromFilename(url.lastPathComponent);
        NSDictionary *metadata = sessionID ? metadataByID[sessionID] : nil;
        NSTimeInterval recencyAt = CodexUnixTimestamp(metadata[@"recencyAt"]);
        NSDate *fallbackStart = recencyAt > 0 ? [NSDate dateWithTimeIntervalSince1970:recencyAt] : nil;
        NSArray<NSString *> *tailLines = CodexTailLines(url);
        if (!tailLines) {
            if (sessionID.length > 0) [unresolvedRecentThreadIDs addObject:sessionID];
            continue;
        }
        readableCandidateCount++;
        NSDictionary *state = CodexActivityStateFromJSONLLinesWithFallback(tailLines, candidate[@"modifiedAt"], now, fallbackStart);
        if (![state[@"active"] boolValue]) continue;
        activeCount++;
        longest = MAX(longest, [state[@"durationSec"] integerValue]);
        NSString *name = [metadata[@"name"] isKindOfClass:NSString.class] ? metadata[@"name"] : nil;
        if (name.length > 0 && activeNames.count < 3 && ![activeNames containsObject:name]) [activeNames addObject:name];
    }
    NSInteger unresolvedCount = unresolvedRecentThreadIDs.count;
    if (readableCandidateCount == 0 && unresolvedCount > 0) {
        return @{ @"available": @NO, @"error": @"近期会话记录已迁移、压缩或暂不可读，无法可靠判断活动", @"partial": @NO, @"note": @"", @"unresolvedRecent": @(unresolvedCount), @"count": @0, @"longest": @0, @"names": @[] };
    }
    if (candidates.count == 0 && !rootAvailable) {
        return @{ @"available": @NO, @"error": @"未找到本机会话记录", @"partial": @NO, @"note": @"", @"unresolvedRecent": @0, @"count": @0, @"longest": @0, @"names": @[] };
    }
    BOOL partial = unresolvedCount > 0;
    NSString *note = partial ? @"部分近期会话记录已迁移、压缩或暂不可读" : @"";
    return @{ @"available": @YES, @"error": @"", @"partial": @(partial), @"note": note, @"unresolvedRecent": @(unresolvedCount), @"count": @(activeCount), @"longest": @(longest), @"names": activeNames };
}

static NSDictionary<NSString *, id> *CodexScanRecentActivity(NSDictionary<NSString *, NSDictionary *> *metadataByID, NSDate *now) {
    NSURL *root = [NSURL fileURLWithPath:[NSHomeDirectory() stringByAppendingPathComponent:@".codex/sessions"] isDirectory:YES];
    return CodexScanRecentActivityAtRoot(metadataByID, now, root);
}

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
    long long today = 0, recent = 0, previous = 0, thirty = 0, monthToDate = 0;
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
    for (NSInteger dayOffset = 0; dayOffset < 30; dayOffset++) {
        NSDate *date = [calendar dateByAddingUnit:NSCalendarUnitDay value:-dayOffset toDate:anchorDate options:0];
        long long tokens = [tokensByDate[[dateFormatter stringFromDate:date]] longLongValue];
        thirty += tokens;
        if (dayOffset < 7) recent += tokens;
        else if (dayOffset < 14) previous += tokens;
    }
    NSDateComponents *currentMonth = [calendar components:NSCalendarUnitYear | NSCalendarUnitMonth fromDate:startOfToday];
    NSDate *monthStart = [calendar dateFromComponents:currentMonth];
    for (NSString *date in tokensByDate) {
        NSDate *parsed = [dateFormatter dateFromString:date];
        if (!parsed || [parsed compare:monthStart] == NSOrderedAscending || [parsed compare:startOfToday] == NSOrderedDescending) continue;
        monthToDate += [tokensByDate[date] longLongValue];
    }
    NSInteger latestMonthDay = latestDate ? [calendar component:NSCalendarUnitDay fromDate:[dateFormatter dateFromString:latestDate]] : 0;
    NSRange monthDays = [calendar rangeOfUnit:NSCalendarUnitDay inUnit:NSCalendarUnitMonth forDate:startOfToday];
    long long monthForecast = latestMonthDay > 0 ? (long long)llround((double)monthToDate / (double)latestMonthDay * (double)monthDays.length) : 0;
    if (todayAvailable) today = [tokensByDate[todayKey] longLongValue];
    long long latestTokens = latestDate ? [tokensByDate[latestDate] longLongValue] : 0;
    return @{ @"today": @(today), @"todayAvailable": @(todayAvailable), @"recent": @(recent), @"previous": @(previous),
              @"thirty": @(thirty), @"monthToDate": @(monthToDate), @"monthForecast": @(monthForecast),
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
@property(nonatomic) BOOL receivedThreads;
@property(nonatomic) BOOL intentionalStop;
@property(nonatomic) BOOL activityRefreshInProgress;
@property(nonatomic) BOOL activityRefreshPending;
@property(nonatomic) BOOL costRefreshInProgress;
@property(nonatomic) BOOL costRefreshPending;
@property(nonatomic) BOOL quotaForecastInProgress;
@property(nonatomic, copy) NSDictionary<NSString *, id> *pendingQuotaForecastSample;
@property(nonatomic, copy) NSDictionary<NSString *, NSDictionary *> *threadMetadataByID;
- (void)finishFetch;
- (void)maybeFinishFetch;
- (void)consumeThreads:(NSDictionary *)result;
- (void)updateQuotaForecastWithSample:(NSDictionary<NSString *, id> *)sample;
@end

@implementation CodexStatusProvider

- (instancetype)init {
    self = [super init];
    if (!self) return nil;
    _snapshot = [CodexStatusSnapshot new];
    _snapshot.statusText = @"正在连接本机Codex";
    _snapshot.activeTaskNames = @[];
    _snapshot.recentTasks = @[];
    _snapshot.localDailyTokenTrend = @[];
    _threadMetadataByID = @{};
    _outputBuffer = [NSMutableData data];
    _costHistoryEnabled = YES;
    _quotaForecastEnabled = YES;
    _accountDataEnabled = YES;
    return self;
}

- (NSURL *)codexHomeURL {
    NSString *configured = NSProcessInfo.processInfo.environment[@"CODEX_HOME"];
    NSString *path = configured.length > 0 ? configured.stringByExpandingTildeInPath : [NSHomeDirectory() stringByAppendingPathComponent:@".codex"];
    return [NSURL fileURLWithPath:path isDirectory:YES];
}

- (NSURL *)applicationDataURL:(NSString *)filename {
    NSURL *support = [NSFileManager.defaultManager URLsForDirectory:NSApplicationSupportDirectory inDomains:NSUserDomainMask].firstObject;
    NSURL *directory = [support URLByAppendingPathComponent:@"CodexSystemMonitor" isDirectory:YES];
    return [directory URLByAppendingPathComponent:filename isDirectory:NO];
}

- (NSString *)codexExecutable {
    NSArray<NSString *> *paths = @[
        @"/Applications/ChatGPT.app/Contents/Resources/codex",
        [NSHomeDirectory() stringByAppendingPathComponent:@"Applications/ChatGPT.app/Contents/Resources/codex"],
        @"/Applications/Codex.app/Contents/Resources/codex",
        [NSHomeDirectory() stringByAppendingPathComponent:@"Applications/Codex.app/Contents/Resources/codex"],
        @"/opt/homebrew/bin/codex",
        @"/usr/local/bin/codex",
        [NSHomeDirectory() stringByAppendingPathComponent:@".local/bin/codex"]
    ];
    for (NSString *path in paths) if ([[NSFileManager defaultManager] isExecutableFileAtPath:path]) return path;
    return nil;
}

- (void)start {
    if (!self.accountDataEnabled) return;
    if (self.task.running) return;
    NSString *executable = [self codexExecutable];
    if (!executable) {
        self.snapshot.statusText = @"未找到Codex本机接口";
        self.snapshot.quotaErrorText = @"未找到Codex本机接口";
        self.snapshot.accountErrorText = @"未找到Codex本机接口";
        self.snapshot.usageErrorText = @"未找到Codex本机接口";
        self.snapshot.recentTasksErrorText = @"未找到Codex本机接口";
        self.snapshot.activityErrorText = @"未找到本机会话数据";
        [self notifyUpdate];
        return;
    }

    self.initialized = NO;
    self.receivedQuota = NO;
    self.receivedAccount = NO;
    self.receivedUsage = NO;
    self.receivedThreads = NO;
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
                if (!weakSelf.receivedThreads) weakSelf.snapshot.recentTasksErrorText = @"更新失败，显示上次数据";
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
        self.snapshot.recentTasksErrorText = @"无法启动本机接口";
        [self notifyUpdate];
        return;
    }
    NSString *clientVersion = [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"] ?: @"unknown";
    [self sendObject:@{
        @"id": @1,
        @"method": @"initialize",
        @"params": @{ @"clientInfo": @{ @"name": @"codex-monitor-hud", @"title": @"Codex Monitor HUD", @"version": clientVersion } }
    }];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(8 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        if (weakSelf.task.running) {
            if (!weakSelf.receivedQuota) { weakSelf.snapshot.statusText = @"额度读取超时，稍后重试"; weakSelf.snapshot.quotaErrorText = @"更新超时，显示上次数据"; }
            if (!weakSelf.receivedAccount) weakSelf.snapshot.accountErrorText = @"更新超时，显示上次数据";
            if (!weakSelf.receivedUsage) weakSelf.snapshot.usageErrorText = @"更新超时，显示上次数据";
            if (!weakSelf.receivedThreads) weakSelf.snapshot.recentTasksErrorText = @"更新超时，显示上次数据";
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
        [self sendObject:@{ @"id": @5, @"method": @"thread/list", @"params": @{
            @"limit": @64,
            @"sortKey": @"recency_at",
            @"sortDirection": @"desc",
            @"useStateDbOnly": @YES
        } }];
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
    if (requestID == 5) {
        if (result) [self consumeThreads:result]; else self.snapshot.recentTasksErrorText = @"更新失败，显示上次数据";
        self.receivedThreads = YES; [self refreshActivity]; [self maybeFinishFetch]; return;
    }
}

- (void)consumeThreads:(NSDictionary *)result {
    NSArray *threads = [result[@"data"] isKindOfClass:NSArray.class] ? result[@"data"] : ([result[@"threads"] isKindOfClass:NSArray.class] ? result[@"threads"] : @[]);
    NSMutableDictionary<NSString *, NSDictionary *> *metadata = [NSMutableDictionary dictionary];
    NSMutableArray<NSDictionary<NSString *, id> *> *recentTasks = [NSMutableArray array];
    NSInteger projectCount = 0;
    for (NSDictionary *thread in threads) {
        if (![thread isKindOfClass:NSDictionary.class]) continue;
        NSDictionary<NSString *, id> *normalized = CodexNormalizedThreadMetadata(thread);
        NSString *threadID = normalized[@"id"];
        NSString *name = normalized[@"name"];
        NSString *displayName = name.length > 0 ? name : @"未命名任务";
        NSNumber *recencyAt = normalized[@"recencyAt"];
        NSString *path = normalized[@"path"];
        NSString *cwd = normalized[@"cwd"];
        if (cwd.length > 0) projectCount++;
        if (threadID.length > 0) metadata[threadID] = @{ @"name": name ?: @"", @"recencyAt": recencyAt ?: @0, @"path": path ?: @"", @"cwd": cwd ?: @"" };
        if (recentTasks.count < 3) {
            NSTimeInterval timestamp = CodexUnixTimestamp(recencyAt);
            [recentTasks addObject:@{ @"name": displayName, @"updatedAt": @(timestamp), @"cwd": cwd ?: @"" }];
        }
    }
    self.threadMetadataByID = metadata;
    self.snapshot.recentTasks = recentTasks;
    self.snapshot.recentTaskCount = threads.count;
    self.snapshot.recentTaskProjectCount = projectCount;
    self.snapshot.recentTasksAvailable = YES;
    self.snapshot.recentTasksUpdatedAt = NSDate.date.timeIntervalSince1970;
    self.snapshot.recentTasksErrorText = nil;
    // This process cannot observe token events inside ChatGPT's separate app-server.
    // Keep the per-task burn feature capability-gated instead of presenting an estimate as fact.
    self.snapshot.livePerTaskUsageAvailable = NO;
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
    if (self.quotaForecastEnabled) {
        NSMutableDictionary<NSString *, id> *sample = [NSMutableDictionary dictionary];
        if (self.snapshot.fiveHourAvailable) {
            sample[@"f"] = @(self.snapshot.fiveHourRemainingPercent);
            sample[@"fr"] = @(self.snapshot.fiveHourResetAt);
        }
        if (self.snapshot.weeklyAvailable) {
            sample[@"w"] = @(self.snapshot.weeklyRemainingPercent);
            sample[@"wr"] = @(self.snapshot.weeklyResetAt);
        }
        if (sample.count > 0) [self updateQuotaForecastWithSample:sample];
    }
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
    self.snapshot.thirtyDayTokens = [usage[@"thirty"] longLongValue];
    self.snapshot.monthToDateTokens = [usage[@"monthToDate"] longLongValue];
    self.snapshot.monthForecastTokens = [usage[@"monthForecast"] longLongValue];
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

- (void)refreshCostHistory {
    if (!self.costHistoryEnabled) return;
    if (self.costRefreshInProgress) { self.costRefreshPending = YES; return; }
    self.costRefreshInProgress = YES;
    self.costRefreshPending = NO;
    NSURL *homeURL = [self codexHomeURL];
    NSURL *cacheURL = [self applicationDataURL:@"codex-cost-cache.json"];
    __weak typeof(self) weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        NSDictionary<NSString *, id> *summary = CodexScanCostHistoryAtHome(homeURL, cacheURL, NSDate.date);
        dispatch_async(dispatch_get_main_queue(), ^{
            if (!weakSelf) return;
            weakSelf.costRefreshInProgress = NO;
            weakSelf.snapshot.localCostAvailable = [summary[@"available"] boolValue];
            weakSelf.snapshot.localCostUpdatedAt = [summary[@"updatedAt"] doubleValue] ?: NSDate.date.timeIntervalSince1970;
            weakSelf.snapshot.localCostErrorText = [summary[@"error"] length] > 0 ? summary[@"error"] : nil;
            weakSelf.snapshot.localCostScanIncomplete = [summary[@"scanIncomplete"] boolValue];
            weakSelf.snapshot.localTodayTokens = [summary[@"todayTokens"] longLongValue];
            weakSelf.snapshot.localSevenDayTokens = [summary[@"sevenDayTokens"] longLongValue];
            weakSelf.snapshot.localThirtyDayTokens = [summary[@"thirtyDayTokens"] longLongValue];
            weakSelf.snapshot.localTodayCostUSD = [summary[@"todayCost"] doubleValue];
            weakSelf.snapshot.localSevenDayCostUSD = [summary[@"sevenDayCost"] doubleValue];
            weakSelf.snapshot.localThirtyDayCostUSD = [summary[@"thirtyDayCost"] doubleValue];
            weakSelf.snapshot.localMonthCostUSD = [summary[@"monthCost"] doubleValue];
            weakSelf.snapshot.localMonthForecastCostUSD = [summary[@"monthForecastCost"] doubleValue];
            weakSelf.snapshot.localPricedTokenPercent = [summary[@"pricedTokenPercent"] doubleValue];
            weakSelf.snapshot.localTopModel = [summary[@"topModel"] isKindOfClass:NSString.class] ? summary[@"topModel"] : @"";
            weakSelf.snapshot.localDailyTokenTrend = [summary[@"dailyTrend"] isKindOfClass:NSArray.class] ? summary[@"dailyTrend"] : @[];
            [weakSelf notifyUpdate];
            if (weakSelf.costRefreshPending && weakSelf.costHistoryEnabled) [weakSelf refreshCostHistory];
        });
    });
}

- (void)updateQuotaForecastWithSample:(NSDictionary<NSString *, id> *)sample {
    if (!self.quotaForecastEnabled || sample.count == 0) return;
    if (self.quotaForecastInProgress) { self.pendingQuotaForecastSample = sample; return; }
    self.quotaForecastInProgress = YES;
    self.pendingQuotaForecastSample = nil;
    NSURL *historyURL = [self applicationDataURL:@"quota-usage-history.json"];
    __weak typeof(self) weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        NSDictionary<NSString *, id> *forecast = CodexUpdateQuotaForecastHistory(historyURL, sample, NSDate.date);
        dispatch_async(dispatch_get_main_queue(), ^{
            if (!weakSelf) return;
            weakSelf.quotaForecastInProgress = NO;
            NSDictionary *five = [forecast[@"fiveHour"] isKindOfClass:NSDictionary.class] ? forecast[@"fiveHour"] : nil;
            NSDictionary *weekly = [forecast[@"weekly"] isKindOfClass:NSDictionary.class] ? forecast[@"weekly"] : nil;
            weakSelf.snapshot.fiveHourForecastAvailable = [five[@"available"] boolValue];
            weakSelf.snapshot.fiveHourForecastHeadline = [five[@"headline"] isKindOfClass:NSString.class] ? five[@"headline"] : @"";
            weakSelf.snapshot.fiveHourForecastDetail = [five[@"detail"] isKindOfClass:NSString.class] ? five[@"detail"] : @"";
            weakSelf.snapshot.weeklyForecastAvailable = [weekly[@"available"] boolValue];
            weakSelf.snapshot.weeklyForecastHeadline = [weekly[@"headline"] isKindOfClass:NSString.class] ? weekly[@"headline"] : @"";
            weakSelf.snapshot.weeklyForecastDetail = [weekly[@"detail"] isKindOfClass:NSString.class] ? weekly[@"detail"] : @"";
            [weakSelf notifyUpdate];
            NSDictionary *pending = weakSelf.pendingQuotaForecastSample;
            weakSelf.pendingQuotaForecastSample = nil;
            if (pending && weakSelf.quotaForecastEnabled) [weakSelf updateQuotaForecastWithSample:pending];
        });
    });
}

- (void)refreshActivity {
    if (self.activityRefreshInProgress) { self.activityRefreshPending = YES; return; }
    self.activityRefreshInProgress = YES;
    self.activityRefreshPending = NO;
    NSDictionary<NSString *, NSDictionary *> *metadata = [self.threadMetadataByID copy] ?: @{};
    __weak typeof(self) weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        NSDictionary<NSString *, id> *summary = CodexScanRecentActivity(metadata, NSDate.date);
        dispatch_async(dispatch_get_main_queue(), ^{
            weakSelf.activityRefreshInProgress = NO;
            if (!weakSelf) return;
            weakSelf.snapshot.activityAvailable = [summary[@"available"] boolValue];
            weakSelf.snapshot.activityUpdatedAt = NSDate.date.timeIntervalSince1970;
            weakSelf.snapshot.activityErrorText = [summary[@"error"] length] > 0 ? summary[@"error"] : nil;
            weakSelf.snapshot.activityPartial = [summary[@"partial"] boolValue];
            weakSelf.snapshot.activityNoteText = [summary[@"note"] length] > 0 ? summary[@"note"] : nil;
            weakSelf.snapshot.unresolvedRecentTaskCount = [summary[@"unresolvedRecent"] integerValue];
            weakSelf.snapshot.activeTaskCount = [summary[@"count"] integerValue];
            weakSelf.snapshot.longestActiveTaskSec = [summary[@"longest"] integerValue];
            weakSelf.snapshot.activeTaskNames = summary[@"names"] ?: @[];
            [weakSelf notifyUpdate];
            if (weakSelf.activityRefreshPending) [weakSelf refreshActivity];
        });
    });
}

- (void)notifyUpdate {
    if (self.updateHandler) self.updateHandler();
}

- (void)maybeFinishFetch {
    if (self.receivedQuota && self.receivedAccount && self.receivedUsage && self.receivedThreads) [self finishFetch];
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
