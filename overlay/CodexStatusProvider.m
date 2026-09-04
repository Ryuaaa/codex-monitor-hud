#import "CodexStatusProvider.h"
#import "CodexCostHistory.h"
#import "CodexProtocolCompatibility.h"

static NSTimeInterval const CodexActivityWindow = 120.0;
static unsigned long long const CodexActivityTailBytes = 1024 * 1024;
static NSUInteger const CodexActivityMaxCandidateFiles = 64;
static NSUInteger const CodexProtocolBufferLimit = 4 * 1024 * 1024;

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
    // status/notLoaded, thread/closed and environments=null describe loading only.
    // Do not turn them into task completion/failure or persist environment content.
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
@property(nonatomic) BOOL backgroundFetch;
@property(nonatomic) BOOL lightweightQuotaRequest;
@property(nonatomic) BOOL retriedLegacyQuota;
@property(nonatomic) BOOL legacyQuotaParameters;
@property(nonatomic) NSInteger quotaRequestID;
@property(nonatomic, copy) NSString *executableIdentity;
@property(nonatomic) NSUInteger consecutiveFetchFailures;
@property(nonatomic, readwrite) NSTimeInterval recommendedRetryInterval;
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
- (void)startFetchInBackground:(BOOL)background;
- (void)recordFailure:(NSString *)kind requestID:(NSInteger)requestID;
- (void)failPendingRequests:(NSString *)kind;
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
    _snapshot.interfaceFailureKinds = @{};
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
    [self startFetchInBackground:NO];
}

- (void)startFetchInBackground:(BOOL)background {
    if (!self.accountDataEnabled) return;
    if (self.task.running) return;
    self.receivedQuota = self.receivedAccount = self.receivedUsage = self.receivedThreads = NO;
    self.backgroundFetch = background;
    self.retriedLegacyQuota = NO;
    self.quotaRequestID = 2;
    NSString *executable = [self codexExecutable];
    if (!executable) {
        [self failPendingRequests:@"missing_executable"];
        self.snapshot.activityErrorText = @"未找到本机会话数据";
        [self finishFetch];
        [self notifyUpdate];
        return;
    }
    NSDictionary *attributes = [NSFileManager.defaultManager attributesOfItemAtPath:executable error:nil];
    NSString *identity = [NSString stringWithFormat:@"%@:%@:%@", executable, attributes[NSFileSize], attributes[NSFileModificationDate]];
    if (![identity isEqualToString:self.executableIdentity]) self.legacyQuotaParameters = NO;
    self.executableIdentity = identity;
    self.lightweightQuotaRequest = background && !self.legacyQuotaParameters;

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
    NSTask *fetchTask = self.task;
    self.outputPipe.fileHandleForReading.readabilityHandler = ^(NSFileHandle *handle) {
        NSData *data = handle.availableData;
        if (data.length == 0) return;
        dispatch_async(dispatch_get_main_queue(), ^{ if (weakSelf.task == fetchTask) [weakSelf consumeData:data]; });
    };
    self.task.terminationHandler = ^(NSTask *task) {
        dispatch_async(dispatch_get_main_queue(), ^{
            if (weakSelf.task != task) return;
            weakSelf.initialized = NO;
            if (!weakSelf.intentionalStop) {
                [weakSelf failPendingRequests:@"network"];
                [weakSelf finishFetch];
                [weakSelf notifyUpdate];
            }
        });
    };
    NSError *error = nil;
    if (![self.task launchAndReturnError:&error]) {
        [self failPendingRequests:@"launch_failed"];
        [self finishFetch];
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
        if (weakSelf.task == fetchTask && fetchTask.running) {
            [weakSelf failPendingRequests:@"timeout"];
            [weakSelf notifyUpdate];
            [weakSelf finishFetch];
        }
    });
}

- (void)consumeData:(NSData *)data {
    if (data.length > CodexProtocolBufferLimit || self.outputBuffer.length > CodexProtocolBufferLimit - data.length) {
        [self failPendingRequests:@"protocol"];
        [self finishFetch];
        [self notifyUpdate];
        return;
    }
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
        else {
            [self failPendingRequests:@"protocol"]; [self finishFetch]; [self notifyUpdate]; return;
        }
    }
}

- (void)handleObject:(NSDictionary *)object {
    // Notifications (including thread/closed and unknown item types) are not RPC replies.
    NSNumber *identifier = CodexProtocolNumber(object[@"id"]);
    if (!identifier || identifier.doubleValue != identifier.integerValue) return;
    NSInteger requestID = identifier.integerValue;
    NSDictionary *result = [object[@"result"] isKindOfClass:NSDictionary.class] ? object[@"result"] : nil;
    if (requestID == 1 && !self.initialized && result) {
        self.initialized = YES;
        [self sendObject:@{ @"method": @"initialized", @"params": @{} }];
        self.quotaRequestID = 2;
        [self sendObject:CodexQuotaReadRequest(@2, self.lightweightQuotaRequest)];
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
    if (requestID == 1 && !self.initialized) {
        [self failPendingRequests:object[@"error"] ? CodexProtocolErrorKind(object[@"error"]) : @"protocol"];
        [self finishFetch]; [self notifyUpdate]; return;
    }
    if (requestID == self.quotaRequestID && !self.receivedQuota && result) {
        [self consumeRateLimits:result];
        self.receivedQuota = YES;
        [self maybeFinishFetch];
        return;
    }
    if (requestID == self.quotaRequestID && !self.receivedQuota) {
        if (CodexQuotaNeedsLegacyRetry(object[@"error"], self.lightweightQuotaRequest, self.retriedLegacyQuota)) {
            self.retriedLegacyQuota = YES;
            self.legacyQuotaParameters = YES;
            self.quotaRequestID = 102;
            [self sendObject:CodexQuotaReadRequest(@102, NO)];
            return;
        }
        [self recordFailure:object[@"error"] ? CodexProtocolErrorKind(object[@"error"]) : @"protocol" requestID:2];
        self.receivedQuota = YES;
        [self notifyUpdate];
        [self maybeFinishFetch];
        return;
    }
    if (requestID == 3 && !self.receivedAccount) {
        if (result) [self consumeAccount:result]; else [self recordFailure:object[@"error"] ? CodexProtocolErrorKind(object[@"error"]) : @"protocol" requestID:3];
        self.receivedAccount = YES; [self notifyUpdate]; [self maybeFinishFetch]; return;
    }
    if (requestID == 4 && !self.receivedUsage) {
        if (result) [self consumeUsage:result]; else [self recordFailure:object[@"error"] ? CodexProtocolErrorKind(object[@"error"]) : @"protocol" requestID:4];
        self.receivedUsage = YES; [self notifyUpdate]; [self maybeFinishFetch]; return;
    }
    if (requestID == 5 && !self.receivedThreads) {
        if (result) [self consumeThreads:result]; else [self recordFailure:object[@"error"] ? CodexProtocolErrorKind(object[@"error"]) : @"protocol" requestID:5];
        self.receivedThreads = YES; [self refreshActivity]; [self maybeFinishFetch]; return;
    }
}

- (void)clearFailureForRequestID:(NSInteger)requestID {
    NSMutableDictionary *kinds = [self.snapshot.interfaceFailureKinds mutableCopy] ?: [NSMutableDictionary dictionary];
    [kinds removeObjectForKey:[@(requestID) stringValue]];
    self.snapshot.interfaceFailureKinds = kinds;
}

- (void)recordFailure:(NSString *)kind requestID:(NSInteger)requestID {
    CodexStatusSnapshot *s = self.snapshot;
    NSMutableDictionary *kinds = [s.interfaceFailureKinds mutableCopy] ?: [NSMutableDictionary dictionary];
    kinds[[@(requestID) stringValue]] = kind;
    s.interfaceFailureKinds = kinds;
    if (requestID == 2) {
        NSTimeInterval now = NSDate.date.timeIntervalSince1970;
        if (s.fiveHourAvailable) { s.fiveHourAvailable = s.fiveHourResetAt > now; s.fiveHourDataState = s.fiveHourAvailable ? @"previous" : @"expired"; }
        if (s.weeklyAvailable) { s.weeklyAvailable = s.weeklyResetAt > now; s.weeklyDataState = s.weeklyAvailable ? @"previous" : @"expired"; }
        s.modelQuotaAvailable = s.modelQuotaAvailable && s.modelQuotaResetAt > now;
        s.quotaAvailable = s.fiveHourAvailable || s.weeklyAvailable || s.modelQuotaAvailable;
        s.ordinaryUsageAllowed = nil;
        s.quotaErrorText = CodexProtocolFailureText(kind, s.quotaUpdatedAt > 0);
        s.statusText = s.quotaErrorText;
    } else if (requestID == 3) s.accountErrorText = CodexProtocolFailureText(kind, s.accountAvailable);
    else if (requestID == 4) s.usageErrorText = CodexProtocolFailureText(kind, s.usageAvailable);
    else if (requestID == 5) s.recentTasksErrorText = CodexProtocolFailureText(kind, s.recentTasksAvailable);
}

- (void)failPendingRequests:(NSString *)kind {
    if (!self.receivedQuota) [self recordFailure:kind requestID:2];
    if (!self.receivedAccount) [self recordFailure:kind requestID:3];
    if (!self.receivedUsage) [self recordFailure:kind requestID:4];
    if (!self.receivedThreads) [self recordFailure:kind requestID:5];
}

- (void)consumeThreads:(NSDictionary *)result {
    NSArray *threads = [result[@"data"] isKindOfClass:NSArray.class] ? result[@"data"] : ([result[@"threads"] isKindOfClass:NSArray.class] ? result[@"threads"] : nil);
    if (!threads) { [self recordFailure:@"protocol" requestID:5]; return; }
    for (id thread in threads) {
        if (![thread isKindOfClass:NSDictionary.class] || ![thread[@"id"] isKindOfClass:NSString.class] || [thread[@"id"] length] == 0) {
            [self recordFailure:@"protocol" requestID:5]; return;
        }
    }
    [self clearFailureForRequestID:5];
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
    self.snapshot.ordinaryUsageAllowed = CodexProtocolBoolean(result[@"ordinaryUsageAllowed"]);
    self.snapshot.ordinaryUsageUpdatedAt = self.snapshot.ordinaryUsageAllowed ? NSDate.date.timeIntervalSince1970 : 0;
    [self clearFailureForRequestID:2];
    NSDictionary *buckets = [result[@"rateLimitsByLimitId"] isKindOfClass:NSDictionary.class] ? result[@"rateLimitsByLimitId"] : nil;
    NSDictionary *rate = [buckets[@"codex"] isKindOfClass:NSDictionary.class] ? buckets[@"codex"] : result[@"rateLimits"];
    if (rate && ![rate isKindOfClass:NSDictionary.class] && rate != (id)NSNull.null) {
        [self recordFailure:@"protocol" requestID:2]; [self notifyUpdate]; return;
    }
    if (![rate isKindOfClass:NSDictionary.class]) {
        NSTimeInterval now = NSDate.date.timeIntervalSince1970;
        BOOL retained = NO;
        if (self.snapshot.fiveHourAvailable && self.snapshot.fiveHourResetAt > now) {
            self.snapshot.fiveHourDataState = @"previous";
            retained = YES;
        } else if (self.snapshot.fiveHourAvailable) {
            self.snapshot.fiveHourAvailable = NO;
            self.snapshot.fiveHourDataState = @"expired";
        }
        if (self.snapshot.weeklyAvailable && self.snapshot.weeklyResetAt > now) {
            self.snapshot.weeklyDataState = @"previous";
            retained = YES;
        } else if (self.snapshot.weeklyAvailable) {
            self.snapshot.weeklyAvailable = NO;
            self.snapshot.weeklyDataState = @"expired";
        }
        self.snapshot.quotaErrorText = retained ? @"本轮未返回，显示上次数据" : @"额度当前未返回";
        self.snapshot.quotaAvailable = retained;
        self.snapshot.statusText = self.snapshot.quotaErrorText;
        [self notifyUpdate];
        return;
    }
    NSTimeInterval now = NSDate.date.timeIntervalSince1970;
    BOOL previousFiveAvailable = self.snapshot.fiveHourAvailable;
    BOOL previousWeeklyAvailable = self.snapshot.weeklyAvailable;
    BOOL receivedFive = NO;
    BOOL receivedWeekly = NO;
    double fiveRemaining = 0, fiveDuration = 0, weeklyRemaining = 0, weeklyDuration = 0;
    NSTimeInterval fiveReset = 0, weeklyReset = 0;
    self.snapshot.modelQuotaAvailable = NO;
    self.snapshot.rateLimitReachedType = [rate[@"rateLimitReachedType"] isKindOfClass:NSString.class] ? rate[@"rateLimitReachedType"] : @"";
    NSDictionary *resetCredits = [result[@"rateLimitResetCredits"] isKindOfClass:NSDictionary.class] ? result[@"rateLimitResetCredits"] : nil;
    NSNumber *resetCount = CodexProtocolNumber(resetCredits[@"availableCount"]);
    if (resetCount.doubleValue < 0 || resetCount.doubleValue != resetCount.integerValue) resetCount = nil;
    self.snapshot.rateLimitResetCreditsAvailable = resetCount != nil;
    self.snapshot.rateLimitResetCreditsCount = MAX(0, resetCount.integerValue);
    NSString *plan = [rate[@"planType"] isKindOfClass:NSString.class] ? rate[@"planType"] : nil;
    if (plan.length > 0) { self.snapshot.accountAvailable = YES; self.snapshot.planType = plan; }
    NSMutableArray<NSDictionary<NSString *, id> *> *quotaRows = [NSMutableArray array];
    for (id value in @[rate[@"primary"] ?: NSNull.null, rate[@"secondary"] ?: NSNull.null]) {
        if (![value isKindOfClass:NSDictionary.class]) continue;
        NSDictionary *window = value;
        NSNumber *used = CodexProtocolNumber(window[@"usedPercent"]);
        NSNumber *duration = CodexProtocolNumber(window[@"windowDurationMins"]);
        if (!used || used.doubleValue < 0 || !duration || duration.doubleValue <= 0) continue;
        double remaining = MAX(0, MIN(100, 100.0 - used.doubleValue));
        NSTimeInterval reset = [CodexProtocolNumber(window[@"resetsAt"]) doubleValue];
        if (duration.doubleValue <= 24.0 * 60.0) {
            receivedFive = YES; fiveRemaining = remaining; fiveReset = reset; fiveDuration = duration.doubleValue;
        } else {
            receivedWeekly = YES; weeklyRemaining = remaining; weeklyReset = reset; weeklyDuration = duration.doubleValue;
        }
    }
    for (NSString *key in [buckets.allKeys sortedArrayUsingSelector:@selector(localizedCaseInsensitiveCompare:)]) {
        NSDictionary *bucket = [buckets[key] isKindOfClass:NSDictionary.class] ? buckets[key] : nil;
        if (!bucket) continue;
        NSString *name = [bucket[@"limitName"] isKindOfClass:NSString.class] ? bucket[@"limitName"] : ([key isEqualToString:@"codex"] ? @"Codex" : key);
        NSInteger slot = 0;
        for (id value in @[bucket[@"primary"] ?: NSNull.null, bucket[@"secondary"] ?: NSNull.null]) {
            slot++;
            if (![value isKindOfClass:NSDictionary.class]) continue;
            NSDictionary *window = value;
            NSNumber *used = CodexProtocolNumber(window[@"usedPercent"]);
            if (!used || used.doubleValue < 0) continue;
            NSNumber *duration = CodexProtocolNumber(window[@"windowDurationMins"]);
            double remaining = MAX(0, MIN(100, 100.0 - used.doubleValue));
            [quotaRows addObject:@{
                @"limitId": key, @"name": name.length > 0 ? name : key,
                @"slot": @(slot), @"remainingPercent": @(remaining),
                @"usedPercent": @(MAX(0, MIN(100, used.doubleValue))),
                @"windowDurationMins": duration ?: @0,
                @"resetsAt": @([CodexProtocolNumber(window[@"resetsAt"]) doubleValue]),
                @"reachedType": [bucket[@"rateLimitReachedType"] isKindOfClass:NSString.class] ? bucket[@"rateLimitReachedType"] : @""
            }];
            if (![key isEqualToString:@"codex"] && !self.snapshot.modelQuotaAvailable) {
                self.snapshot.modelQuotaAvailable = YES;
                self.snapshot.modelQuotaName = name.length > 0 ? name : key;
                self.snapshot.modelQuotaRemainingPercent = remaining;
                self.snapshot.modelQuotaResetAt = [CodexProtocolNumber(window[@"resetsAt"]) doubleValue];
                self.snapshot.modelQuotaWindowLabel = duration.doubleValue <= 0 ? @"周期未知" : (duration.doubleValue <= 24.0 * 60.0 ? @"短周期" : @"每周");
            }
        }
    }
    if (quotaRows.count == 0) {
        NSInteger slot = 0;
        for (id value in @[rate[@"primary"] ?: NSNull.null, rate[@"secondary"] ?: NSNull.null]) {
            slot++;
            if (![value isKindOfClass:NSDictionary.class]) continue;
            NSDictionary *window = value;
            NSNumber *used = CodexProtocolNumber(window[@"usedPercent"]);
            if (!used || used.doubleValue < 0) continue;
            [quotaRows addObject:@{ @"limitId": @"codex", @"name": @"Codex", @"slot": @(slot),
                                    @"remainingPercent": @(MAX(0, MIN(100, 100.0 - used.doubleValue))),
                                    @"usedPercent": @(MAX(0, MIN(100, used.doubleValue))),
                                    @"windowDurationMins": CodexProtocolNumber(window[@"windowDurationMins"]) ?: @0,
                                    @"resetsAt": @([CodexProtocolNumber(window[@"resetsAt"]) doubleValue]), @"reachedType": self.snapshot.rateLimitReachedType ?: @"" }];
        }
    }
    self.snapshot.rateLimitBuckets = quotaRows;
    if (receivedFive) {
        self.snapshot.fiveHourAvailable = YES; self.snapshot.fiveHourRemainingPercent = fiveRemaining;
        self.snapshot.fiveHourResetAt = fiveReset; self.snapshot.fiveHourWindowDurationMins = fiveDuration;
        self.snapshot.fiveHourDataState = @"live";
    } else if (previousFiveAvailable && self.snapshot.fiveHourResetAt > now) {
        self.snapshot.fiveHourAvailable = YES; self.snapshot.fiveHourDataState = @"previous";
    } else {
        self.snapshot.fiveHourAvailable = NO; self.snapshot.fiveHourDataState = previousFiveAvailable ? @"expired" : @"unavailable";
    }
    if (receivedWeekly) {
        self.snapshot.weeklyAvailable = YES; self.snapshot.weeklyRemainingPercent = weeklyRemaining;
        self.snapshot.weeklyResetAt = weeklyReset; self.snapshot.weeklyWindowDurationMins = weeklyDuration;
        self.snapshot.weeklyDataState = @"live";
    } else if (previousWeeklyAvailable && self.snapshot.weeklyResetAt > now) {
        self.snapshot.weeklyAvailable = YES; self.snapshot.weeklyDataState = @"previous";
    } else {
        self.snapshot.weeklyAvailable = NO; self.snapshot.weeklyDataState = previousWeeklyAvailable ? @"expired" : @"unavailable";
    }
    BOOL receivedAny = receivedFive || receivedWeekly || quotaRows.count > 0;
    self.snapshot.quotaAvailable = receivedAny || self.snapshot.fiveHourAvailable || self.snapshot.weeklyAvailable;
    if (receivedAny) self.snapshot.quotaUpdatedAt = now;
    BOOL retainedPrevious = [self.snapshot.fiveHourDataState isEqualToString:@"previous"] ||
                            [self.snapshot.weeklyDataState isEqualToString:@"previous"];
    self.snapshot.quotaErrorText = retainedPrevious ? @"本轮部分额度未返回，显示上次数据" : nil;
    self.snapshot.updatedAt = now;
    self.snapshot.statusText = retainedPrevious ? self.snapshot.quotaErrorText : @"Codex额度已更新";
    [self notifyUpdate];
    if (self.quotaForecastEnabled) {
        NSMutableDictionary<NSString *, id> *sample = [NSMutableDictionary dictionary];
        if (receivedFive) {
            sample[@"f"] = @(self.snapshot.fiveHourRemainingPercent);
            sample[@"fr"] = @(self.snapshot.fiveHourResetAt);
        }
        if (receivedWeekly) {
            sample[@"w"] = @(self.snapshot.weeklyRemainingPercent);
            sample[@"wr"] = @(self.snapshot.weeklyResetAt);
        }
        if (sample.count > 0) [self updateQuotaForecastWithSample:sample];
    }
}

- (void)consumeAccount:(NSDictionary *)result {
    if (result[@"account"] && result[@"account"] != NSNull.null && ![result[@"account"] isKindOfClass:NSDictionary.class]) {
        [self recordFailure:@"protocol" requestID:3]; return;
    }
    [self clearFailureForRequestID:3];
    NSDictionary *account = [result[@"account"] isKindOfClass:NSDictionary.class] ? result[@"account"] : nil;
    NSString *plan = [account[@"planType"] isKindOfClass:NSString.class] ? account[@"planType"] : nil;
    if (plan.length > 0) {
        self.snapshot.accountAvailable = YES; self.snapshot.planType = plan;
        self.snapshot.accountUpdatedAt = NSDate.date.timeIntervalSince1970; self.snapshot.accountErrorText = nil;
    } else self.snapshot.accountErrorText = @"订阅当前未返回";
}

- (void)consumeUsage:(NSDictionary *)result {
    if ((result[@"dailyUsageBuckets"] && result[@"dailyUsageBuckets"] != NSNull.null && ![result[@"dailyUsageBuckets"] isKindOfClass:NSArray.class]) ||
        (result[@"summary"] && result[@"summary"] != NSNull.null && ![result[@"summary"] isKindOfClass:NSDictionary.class])) {
        [self recordFailure:@"protocol" requestID:4]; return;
    }
    [self clearFailureForRequestID:4];
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
    self.snapshot.lifetimeTokens = [CodexProtocolNumber(summary[@"lifetimeTokens"]) longLongValue];
    NSNumber *peakDaily = [summary[@"peakDailyTokens"] isKindOfClass:NSNumber.class] ? summary[@"peakDailyTokens"] : nil;
    self.snapshot.peakDailyTokensAvailable = peakDaily != nil;
    self.snapshot.peakDailyTokens = peakDaily.longLongValue;
    self.snapshot.currentStreakDays = [CodexProtocolNumber(summary[@"currentStreakDays"]) integerValue];
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

- (void)refreshQuotaInBackground {
    [self startFetchInBackground:YES];
}

- (void)refreshCostHistory {
    if (!self.costHistoryEnabled) return;
    if (self.costRefreshInProgress) { self.costRefreshPending = YES; return; }
    self.costRefreshInProgress = YES;
    self.costRefreshPending = NO;
    NSURL *homeURL = [self codexHomeURL];
    NSURL *cacheURL = [self applicationDataURL:@"codex-cost-cache.json"];
    NSURL *trackingStartURL = [self applicationDataURL:@"codex-cost-tracking-start.json"];
    __weak typeof(self) weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        NSDictionary<NSString *, id> *summary = CodexScanCostHistoryAtHome(homeURL, cacheURL, trackingStartURL, NSDate.date);
        dispatch_async(dispatch_get_main_queue(), ^{
            if (!weakSelf) return;
            weakSelf.costRefreshInProgress = NO;
            weakSelf.snapshot.localCostAvailable = [summary[@"available"] boolValue];
            weakSelf.snapshot.localCostUpdatedAt = [summary[@"updatedAt"] doubleValue] ?: NSDate.date.timeIntervalSince1970;
            weakSelf.snapshot.localCostErrorText = [summary[@"error"] length] > 0 ? summary[@"error"] : nil;
            weakSelf.snapshot.localCostScanIncomplete = [summary[@"scanIncomplete"] boolValue];
            weakSelf.snapshot.localCostTrackingStartedAt = [summary[@"trackingStartedAt"] doubleValue];
            weakSelf.snapshot.localTokenBucketsStartedAt = [summary[@"tokenBucketsStartedAt"] doubleValue];
            weakSelf.snapshot.localTokenBuckets = [summary[@"tokenBuckets"] isKindOfClass:NSArray.class] ? summary[@"tokenBuckets"] : @[];
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
            NSDictionary *weeklyRolling = [forecast[@"weeklyRolling24h"] isKindOfClass:NSDictionary.class] ? forecast[@"weeklyRolling24h"] : nil;
            NSDictionary *weeklyNaturalDay = [forecast[@"weeklyNaturalDay"] isKindOfClass:NSDictionary.class] ? forecast[@"weeklyNaturalDay"] : nil;
            weakSelf.snapshot.fiveHourForecastAvailable = [five[@"available"] boolValue];
            weakSelf.snapshot.fiveHourForecastHeadline = [five[@"headline"] isKindOfClass:NSString.class] ? five[@"headline"] : @"";
            weakSelf.snapshot.fiveHourForecastDetail = [five[@"detail"] isKindOfClass:NSString.class] ? five[@"detail"] : @"";
            weakSelf.snapshot.weeklyForecastAvailable = [weekly[@"available"] boolValue];
            weakSelf.snapshot.weeklyForecastHeadline = [weekly[@"headline"] isKindOfClass:NSString.class] ? weekly[@"headline"] : @"";
            weakSelf.snapshot.weeklyForecastDetail = [weekly[@"detail"] isKindOfClass:NSString.class] ? weekly[@"detail"] : @"";
            weakSelf.snapshot.weeklyRolling24hConsumptionAvailable = [weeklyRolling[@"available"] boolValue];
            weakSelf.snapshot.weeklyRolling24hConsumedPercent = [weeklyRolling[@"consumedPercent"] doubleValue];
            weakSelf.snapshot.weeklyNaturalDayConsumptionAvailable = [weeklyNaturalDay[@"available"] boolValue];
            weakSelf.snapshot.weeklyNaturalDayConsumedPercent = [weeklyNaturalDay[@"consumedPercent"] doubleValue];
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
    self.outputBuffer.length = 0;
    self.initialized = NO;
}

- (void)finishFetch {
    if (self.snapshot.interfaceFailureKinds.count > 0) {
        self.consecutiveFetchFailures = MIN(4, self.consecutiveFetchFailures + 1);
        BOOL permanentOnly = YES;
        for (NSString *kind in self.snapshot.interfaceFailureKinds.allValues) {
            if (![@[@"unsupported", @"invalid_parameters", @"protocol", @"authentication", @"missing_executable", @"launch_failed"] containsObject:kind]) permanentOnly = NO;
        }
        self.recommendedRetryInterval = permanentOnly ? 300.0 : MIN(300.0, 60.0 * pow(2.0, self.consecutiveFetchFailures - 1));
    } else {
        self.consecutiveFetchFailures = 0;
        self.recommendedRetryInterval = 0;
    }
    self.intentionalStop = YES;
    self.outputPipe.fileHandleForReading.readabilityHandler = nil;
    if (self.task.running) [self.task terminate];
    self.task = nil;
    self.initialized = NO;
    self.outputBuffer.length = 0;
}

@end
