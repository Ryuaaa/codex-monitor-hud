#import "CodexCostHistory.h"
#import <CommonCrypto/CommonDigest.h>
#import <math.h>

static NSInteger const CodexCostHistoryDays = 30;
static unsigned long long const CodexCostScanBudgetBytes = 64ULL * 1024ULL * 1024ULL;
static NSUInteger const CodexCostMaxRetainedLineBytes = 512 * 1024;
static NSInteger const CodexCostCacheVersion = 3;
static NSString *const CodexCostPricingVersion = @"OpenAI 2026-08-08";

static NSDate *CodexCostParseTimestamp(id value) {
    if (![value isKindOfClass:NSString.class] || [value length] == 0) return nil;
    static NSISO8601DateFormatter *fractional;
    static NSISO8601DateFormatter *whole;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        fractional = [NSISO8601DateFormatter new];
        fractional.formatOptions = NSISO8601DateFormatWithInternetDateTime | NSISO8601DateFormatWithFractionalSeconds;
        whole = [NSISO8601DateFormatter new];
        whole.formatOptions = NSISO8601DateFormatWithInternetDateTime;
    });
    return [fractional dateFromString:value] ?: [whole dateFromString:value];
}

static NSDateFormatter *CodexCostDayFormatter(void) {
    NSDateFormatter *formatter = [NSDateFormatter new];
    formatter.calendar = [NSCalendar calendarWithIdentifier:NSCalendarIdentifierGregorian];
    formatter.calendar.timeZone = NSTimeZone.localTimeZone;
    formatter.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
    formatter.timeZone = NSTimeZone.localTimeZone;
    formatter.dateFormat = @"yyyy-MM-dd";
    return formatter;
}

static unsigned long long CodexStableHash(NSString *text) {
    NSData *data = [text dataUsingEncoding:NSUTF8StringEncoding] ?: NSData.data;
    const uint8_t *bytes = data.bytes;
    unsigned long long hash = 1469598103934665603ULL;
    for (NSUInteger index = 0; index < data.length; index++) {
        hash ^= bytes[index];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static NSString *CodexPathCacheKey(NSString *path) {
    NSData *data = [path ?: @"" dataUsingEncoding:NSUTF8StringEncoding] ?: NSData.data;
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(data.bytes, (CC_LONG)data.length, digest);
    NSMutableString *result = [NSMutableString stringWithCapacity:CC_SHA256_DIGEST_LENGTH * 2];
    for (NSUInteger index = 0; index < CC_SHA256_DIGEST_LENGTH; index++) [result appendFormat:@"%02x", digest[index]];
    return result;
}

static NSString *CodexNormalizedCostModel(NSString *raw) {
    NSString *model = [raw isKindOfClass:NSString.class] ? [raw stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet] : @"";
    if ([model hasPrefix:@"openai/"]) model = [model substringFromIndex:7];
    if ([model isEqualToString:@"gpt-5.6"]) return @"gpt-5.6-sol";
    NSRegularExpression *dated = [NSRegularExpression regularExpressionWithPattern:@"-\\d{4}-\\d{2}-\\d{2}$" options:0 error:nil];
    NSTextCheckingResult *match = [dated firstMatchInString:model options:0 range:NSMakeRange(0, model.length)];
    if (match) model = [model substringToIndex:(NSUInteger)match.range.location];
    NSDictionary<NSString *, NSString *> *aliases = @{
        @"gpt-5-codex": @"gpt-5",
        @"gpt-5.1-codex": @"gpt-5.1",
        @"gpt-5.1-codex-max": @"gpt-5.1",
        @"gpt-5.1-codex-mini": @"gpt-5-mini",
        @"gpt-5.2-codex": @"gpt-5.2"
    };
    return aliases[model] ?: model;
}

static NSDictionary<NSString *, NSNumber *> *CodexPricingForModel(NSString *rawModel) {
    NSString *model = CodexNormalizedCostModel(rawModel);
    static NSDictionary<NSString *, NSDictionary<NSString *, NSNumber *> *> *prices;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        prices = @{
            @"gpt-5": @{ @"i": @1.25, @"c": @0.125, @"o": @10.0 },
            @"gpt-5-mini": @{ @"i": @0.25, @"c": @0.025, @"o": @2.0 },
            @"gpt-5-nano": @{ @"i": @0.05, @"c": @0.005, @"o": @0.4 },
            @"gpt-5-pro": @{ @"i": @15.0, @"c": @15.0, @"o": @120.0 },
            @"gpt-5.1": @{ @"i": @1.25, @"c": @0.125, @"o": @10.0 },
            @"gpt-5.2": @{ @"i": @1.75, @"c": @0.175, @"o": @14.0 },
            @"gpt-5.2-pro": @{ @"i": @21.0, @"c": @21.0, @"o": @168.0 },
            @"gpt-5.3-codex": @{ @"i": @1.75, @"c": @0.175, @"o": @14.0 },
            @"gpt-5.4": @{ @"i": @2.5, @"c": @0.25, @"o": @15.0, @"li": @5.0, @"lc": @0.5, @"lo": @22.5, @"t": @272000 },
            @"gpt-5.4-mini": @{ @"i": @0.75, @"c": @0.075, @"o": @4.5 },
            @"gpt-5.4-nano": @{ @"i": @0.2, @"c": @0.02, @"o": @1.25 },
            @"gpt-5.4-pro": @{ @"i": @30.0, @"c": @30.0, @"o": @180.0, @"li": @60.0, @"lc": @60.0, @"lo": @270.0, @"t": @272000 },
            @"gpt-5.5": @{ @"i": @5.0, @"c": @0.5, @"o": @30.0, @"li": @10.0, @"lc": @1.0, @"lo": @45.0, @"t": @272000 },
            @"gpt-5.5-pro": @{ @"i": @30.0, @"c": @30.0, @"o": @180.0, @"li": @60.0, @"lc": @60.0, @"lo": @270.0, @"t": @272000 },
            @"gpt-5.6-sol": @{ @"i": @5.0, @"c": @0.5, @"w": @6.25, @"o": @30.0, @"li": @10.0, @"lc": @1.0, @"lw": @12.5, @"lo": @45.0, @"t": @272000 },
            @"gpt-5.6-terra": @{ @"i": @2.0, @"c": @0.2, @"w": @2.5, @"o": @12.0, @"li": @4.0, @"lc": @0.4, @"lw": @5.0, @"lo": @18.0, @"t": @272000 },
            @"gpt-5.6-luna": @{ @"i": @0.2, @"c": @0.02, @"w": @0.25, @"o": @1.2, @"li": @0.4, @"lc": @0.04, @"lw": @0.5, @"lo": @1.8, @"t": @272000 }
        };
    });
    return prices[model];
}

NSDictionary<NSString *, id> *CodexCostEstimateForTokens(NSString *model,
                                                           long long inputTokens,
                                                           long long cachedInputTokens,
                                                           long long cacheWriteInputTokens,
                                                           long long outputTokens) {
    NSString *normalized = CodexNormalizedCostModel(model);
    NSDictionary<NSString *, NSNumber *> *pricing = CodexPricingForModel(normalized);
    if (!pricing) return @{ @"available": @NO, @"model": normalized ?: @"" };
    long long input = MAX(0, inputTokens);
    long long cached = MIN(input, MAX(0, cachedInputTokens));
    long long write = MIN(input - cached, MAX(0, cacheWriteInputTokens));
    long long uncached = input - cached - write;
    BOOL longContext = pricing[@"t"] && input > pricing[@"t"].longLongValue;
    double inputRate = [pricing[longContext ? @"li" : @"i"] doubleValue];
    double cachedRate = [pricing[longContext ? @"lc" : @"c"] doubleValue];
    NSNumber *writeRateValue = pricing[longContext ? @"lw" : @"w"];
    double writeRate = writeRateValue ? writeRateValue.doubleValue : inputRate;
    double outputRate = [pricing[longContext ? @"lo" : @"o"] doubleValue];
    double cost = ((double)uncached * inputRate + (double)cached * cachedRate + (double)write * writeRate + (double)MAX(0, outputTokens) * outputRate) / 1000000.0;
    return @{ @"available": @YES, @"model": normalized, @"cost": @(cost), @"longContext": @(longContext), @"pricingVersion": CodexCostPricingVersion };
}

static NSDictionary<NSString *, NSNumber *> *CodexTokenTuple(NSDictionary *usage) {
    if (![usage isKindOfClass:NSDictionary.class]) return nil;
    long long input = [usage[@"input_tokens"] longLongValue];
    long long cached = [usage[@"cached_input_tokens"] longLongValue];
    long long output = [usage[@"output_tokens"] longLongValue];
    long long write = [usage[@"cache_write_input_tokens"] longLongValue];
    if (write == 0) write = [usage[@"cache_creation_input_tokens"] longLongValue];
    if (input <= 0 && output <= 0 && cached <= 0 && write <= 0) return nil;
    return @{ @"i": @(MAX(0, input)), @"c": @(MAX(0, cached)), @"w": @(MAX(0, write)), @"o": @(MAX(0, output)) };
}

static NSDictionary<NSString *, NSNumber *> *CodexTokenDeltaAboveWatermark(NSDictionary<NSString *, NSNumber *> *current,
                                                                            NSDictionary<NSString *, NSNumber *> *watermark) {
    if (!current) return nil;
    if (!watermark) return current;
    return @{
        @"i": @(MAX(0, [current[@"i"] longLongValue] - [watermark[@"i"] longLongValue])),
        @"c": @(MAX(0, [current[@"c"] longLongValue] - [watermark[@"c"] longLongValue])),
        @"w": @(MAX(0, [current[@"w"] longLongValue] - [watermark[@"w"] longLongValue])),
        @"o": @(MAX(0, [current[@"o"] longLongValue] - [watermark[@"o"] longLongValue]))
    };
}

static NSDictionary<NSString *, NSNumber *> *CodexTokenWatermark(NSDictionary<NSString *, NSNumber *> *current,
                                                                  NSDictionary<NSString *, NSNumber *> *watermark) {
    if (!current) return watermark;
    if (!watermark) return current;
    return @{
        @"i": @(MAX([current[@"i"] longLongValue], [watermark[@"i"] longLongValue])),
        @"c": @(MAX([current[@"c"] longLongValue], [watermark[@"c"] longLongValue])),
        @"w": @(MAX([current[@"w"] longLongValue], [watermark[@"w"] longLongValue])),
        @"o": @(MAX([current[@"o"] longLongValue], [watermark[@"o"] longLongValue]))
    };
}

static void CodexParseCostLine(NSString *line,
                               NSMutableDictionary<NSString *, id> *state,
                               NSMutableArray<NSDictionary<NSString *, id> *> *events,
                               NSDate *earliest,
                               NSDateFormatter *dayFormatter) {
    NSUInteger prefixLength = MIN((NSUInteger)8192, line.length);
    NSRange prefixRange = NSMakeRange(0, prefixLength);
    BOOL isTurnContext = [line rangeOfString:@"\"type\":\"turn_context\"" options:0 range:prefixRange].location != NSNotFound;
    BOOL isEventMessage = [line rangeOfString:@"\"type\":\"event_msg\"" options:0 range:prefixRange].location != NSNotFound;
    if (!isTurnContext && !isEventMessage) return;
    if (isTurnContext) {
        NSString *model = nil;
        NSData *data = line.length <= CodexCostMaxRetainedLineBytes ? [line dataUsingEncoding:NSUTF8StringEncoding] : nil;
        NSDictionary *object = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
        NSDictionary *payload = [object[@"payload"] isKindOfClass:NSDictionary.class] ? object[@"payload"] : nil;
        if ([payload[@"model"] isKindOfClass:NSString.class]) model = payload[@"model"];
        if (model.length == 0) {
            NSRegularExpression *modelPattern = [NSRegularExpression regularExpressionWithPattern:@"\\\"model\\\"\\s*:\\s*\\\"([^\\\"\\\\]+)\\\"" options:0 error:nil];
            NSTextCheckingResult *match = [modelPattern firstMatchInString:line options:0 range:prefixRange];
            if (match.numberOfRanges > 1) model = [line substringWithRange:[match rangeAtIndex:1]];
        }
        if (model.length > 0) state[@"model"] = CodexNormalizedCostModel(model);
        return;
    }
    if ([line rangeOfString:@"\"type\":\"token_count\"" options:0 range:NSMakeRange(0, MIN((NSUInteger)32768, line.length))].location == NSNotFound) return;
    NSData *data = [line dataUsingEncoding:NSUTF8StringEncoding];
    NSDictionary *object = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    if (![object isKindOfClass:NSDictionary.class]) return;
    NSDictionary *payload = [object[@"payload"] isKindOfClass:NSDictionary.class] ? object[@"payload"] : nil;
    if (![payload[@"type"] isEqualToString:@"token_count"]) return;
    NSDictionary *info = [payload[@"info"] isKindOfClass:NSDictionary.class] ? payload[@"info"] : nil;
    if (!info) return;
    NSString *model = [info[@"model"] isKindOfClass:NSString.class] ? info[@"model"] : state[@"model"];
    model = CodexNormalizedCostModel(model ?: @"");
    NSDictionary<NSString *, NSNumber *> *last = CodexTokenTuple(info[@"last_token_usage"]);
    NSDictionary<NSString *, NSNumber *> *total = CodexTokenTuple(info[@"total_token_usage"]);
    NSDictionary *watermark = [state[@"rawTotalsWatermark"] isKindOfClass:NSDictionary.class] ? state[@"rawTotalsWatermark"] : nil;
    NSDictionary<NSString *, NSNumber *> *counted = total ? CodexTokenDeltaAboveWatermark(total, watermark) : last;
    if (total) state[@"rawTotalsWatermark"] = CodexTokenWatermark(total, watermark);
    long long countedTokens = [counted[@"i"] longLongValue] + [counted[@"o"] longLongValue];
    if (countedTokens <= 0) return;
    if (!counted) return;
    NSDate *timestamp = CodexCostParseTimestamp(object[@"timestamp"]);
    if (!timestamp || (earliest && [timestamp compare:earliest] == NSOrderedAscending)) return;
    NSString *base = [NSString stringWithFormat:@"%016llx", CodexStableHash(line)];
    NSMutableDictionary *occurrences = [state[@"occurrences"] isKindOfClass:NSMutableDictionary.class] ? state[@"occurrences"] : nil;
    if (!occurrences) {
        occurrences = [[state[@"occurrences"] isKindOfClass:NSDictionary.class] ? state[@"occurrences"] : @{} mutableCopy];
        state[@"occurrences"] = occurrences;
    }
    NSInteger occurrence = [occurrences[base] integerValue] + 1;
    occurrences[base] = @(occurrence);
    NSString *fingerprint = [NSString stringWithFormat:@"%@#%ld", base, (long)occurrence];
    [events addObject:@{
        @"k": fingerprint,
        @"d": [dayFormatter stringFromDate:timestamp],
        @"m": model.length > 0 ? model : @"unknown",
        @"i": counted[@"i"], @"c": counted[@"c"], @"w": counted[@"w"], @"o": counted[@"o"]
    }];
}

NSDictionary<NSString *, id> *CodexCostEventsFromJSONLLines(NSArray<NSString *> *lines, NSDate *now, NSInteger historyDays) {
    NSCalendar *calendar = [NSCalendar calendarWithIdentifier:NSCalendarIdentifierGregorian];
    calendar.timeZone = NSTimeZone.localTimeZone;
    NSDate *earliest = [calendar dateByAddingUnit:NSCalendarUnitDay value:-(MAX(1, historyDays) - 1) toDate:[calendar startOfDayForDate:now ?: NSDate.date] options:0];
    NSMutableDictionary<NSString *, id> *state = [@{ @"model": @"unknown", @"occurrences": [NSMutableDictionary dictionary] } mutableCopy];
    NSMutableArray *events = [NSMutableArray array];
    NSDateFormatter *formatter = CodexCostDayFormatter();
    for (NSString *line in lines ?: @[]) if ([line isKindOfClass:NSString.class]) CodexParseCostLine(line, state, events, earliest, formatter);
    return @{ @"events": events, @"state": state };
}

static NSArray<NSDictionary<NSString *, id> *> *CodexCompactedCostEvents(NSArray<NSDictionary<NSString *, id> *> *events,
                                                                          NSString *sourceKey) {
    NSMutableDictionary<NSString *, NSMutableDictionary<NSString *, id> *> *rows = [NSMutableDictionary dictionary];
    for (NSDictionary<NSString *, id> *event in events ?: @[]) {
        NSString *day = [event[@"d"] isKindOfClass:NSString.class] ? event[@"d"] : nil;
        NSString *model = [event[@"m"] isKindOfClass:NSString.class] ? event[@"m"] : @"unknown";
        if (day.length == 0) continue;
        NSString *groupKey = [NSString stringWithFormat:@"%@\n%@", day, model];
        NSMutableDictionary<NSString *, id> *row = rows[groupKey];
        if (!row) {
            NSString *fingerprint = [NSString stringWithFormat:@"compact-%016llx-%016llx", CodexStableHash(sourceKey ?: @""), CodexStableHash(groupKey)];
            row = [@{ @"k": fingerprint, @"d": day, @"m": model, @"i": @0LL, @"c": @0LL, @"w": @0LL, @"o": @0LL, @"x": @0.0, @"p": @0LL } mutableCopy];
            rows[groupKey] = row;
        }
        for (NSString *tokenKey in @[@"i", @"c", @"w", @"o"]) {
            row[tokenKey] = @([row[tokenKey] longLongValue] + MAX(0LL, [event[tokenKey] longLongValue]));
        }
        long long eventTokens = MAX(0LL, [event[@"i"] longLongValue]) + MAX(0LL, [event[@"o"] longLongValue]);
        NSNumber *storedCost = [event[@"x"] isKindOfClass:NSNumber.class] ? event[@"x"] : nil;
        NSNumber *storedPricedTokens = [event[@"p"] isKindOfClass:NSNumber.class] ? event[@"p"] : nil;
        if (storedCost && storedPricedTokens) {
            row[@"x"] = @([row[@"x"] doubleValue] + storedCost.doubleValue);
            row[@"p"] = @([row[@"p"] longLongValue] + MIN(eventTokens, MAX(0LL, storedPricedTokens.longLongValue)));
        } else {
            NSDictionary *estimate = CodexCostEstimateForTokens(model, [event[@"i"] longLongValue], [event[@"c"] longLongValue], [event[@"w"] longLongValue], [event[@"o"] longLongValue]);
            if ([estimate[@"available"] boolValue]) {
                row[@"x"] = @([row[@"x"] doubleValue] + [estimate[@"cost"] doubleValue]);
                row[@"p"] = @([row[@"p"] longLongValue] + eventTokens);
            }
        }
    }
    return [rows.allValues sortedArrayUsingComparator:^NSComparisonResult(NSDictionary *left, NSDictionary *right) {
        NSComparisonResult dayResult = [left[@"d"] compare:right[@"d"]];
        return dayResult != NSOrderedSame ? dayResult : [left[@"m"] compare:right[@"m"]];
    }];
}

static NSArray<NSURL *> *CodexCostCandidateFiles(NSURL *home, NSDate *now) {
    if (home.path.length == 0) return @[];
    NSFileManager *fm = NSFileManager.defaultManager;
    NSCalendar *calendar = [NSCalendar calendarWithIdentifier:NSCalendarIdentifierGregorian];
    calendar.timeZone = NSTimeZone.localTimeZone;
    NSDateFormatter *folder = [NSDateFormatter new];
    folder.calendar = calendar; folder.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
    folder.timeZone = calendar.timeZone; folder.dateFormat = @"yyyy/MM/dd";
    NSMutableArray<NSURL *> *files = [NSMutableArray array];
    NSURL *sessions = [home URLByAppendingPathComponent:@"sessions" isDirectory:YES];
    for (NSInteger offset = 0; offset < CodexCostHistoryDays; offset++) {
        NSDate *date = [calendar dateByAddingUnit:NSCalendarUnitDay value:-offset toDate:now ?: NSDate.date options:0];
        NSURL *dir = [sessions URLByAppendingPathComponent:[folder stringFromDate:date] isDirectory:YES];
        NSArray<NSURL *> *dayFiles = [fm contentsOfDirectoryAtURL:dir includingPropertiesForKeys:@[NSURLContentModificationDateKey, NSURLFileSizeKey, NSURLIsRegularFileKey] options:NSDirectoryEnumerationSkipsHiddenFiles error:nil];
        for (NSURL *file in dayFiles ?: @[]) if ([file.pathExtension.lowercaseString isEqualToString:@"jsonl"]) [files addObject:file];
    }
    NSDate *oldestModification = [calendar dateByAddingUnit:NSCalendarUnitDay value:-(CodexCostHistoryDays + 2) toDate:now ?: NSDate.date options:0];
    NSURL *archived = [home URLByAppendingPathComponent:@"archived_sessions" isDirectory:YES];
    NSArray<NSURL *> *archivedFiles = [fm contentsOfDirectoryAtURL:archived includingPropertiesForKeys:@[NSURLContentModificationDateKey, NSURLFileSizeKey, NSURLIsRegularFileKey] options:NSDirectoryEnumerationSkipsHiddenFiles error:nil];
    for (NSURL *file in archivedFiles ?: @[]) {
        if (![file.pathExtension.lowercaseString isEqualToString:@"jsonl"]) continue;
        NSDate *modified = [file resourceValuesForKeys:@[NSURLContentModificationDateKey] error:nil][NSURLContentModificationDateKey];
        if (modified && [modified compare:oldestModification] != NSOrderedAscending) [files addObject:file];
    }
    [files sortUsingComparator:^NSComparisonResult(NSURL *left, NSURL *right) {
        NSDate *leftDate = [left resourceValuesForKeys:@[NSURLContentModificationDateKey] error:nil][NSURLContentModificationDateKey] ?: NSDate.distantPast;
        NSDate *rightDate = [right resourceValuesForKeys:@[NSURLContentModificationDateKey] error:nil][NSURLContentModificationDateKey] ?: NSDate.distantPast;
        return [rightDate compare:leftDate];
    }];
    return files;
}

static unsigned long long CodexParseFileFromOffset(NSURL *file,
                                                    unsigned long long offset,
                                                    unsigned long long byteBudget,
                                                    void (^lineHandler)(NSString *line),
                                                    BOOL *complete,
                                                    BOOL *reachedEnd) {
    NSFileHandle *handle = [NSFileHandle fileHandleForReadingFromURL:file error:nil];
    if (!handle) {
        if (complete) *complete = NO;
        if (reachedEnd) *reachedEnd = NO;
        return offset;
    }
    unsigned long long fileSize = [handle seekToEndOfFile];
    if (offset > fileSize) offset = 0;
    [handle seekToFileOffset:offset];
    NSMutableData *lineBuffer = [NSMutableData data];
    unsigned long long lastCompleteOffset = offset;
    unsigned long long currentLineBytes = 0;
    unsigned long long bytesRead = 0;
    BOOL reachedEOF = NO;
    while (bytesRead < MAX(1ULL, byteBudget)) {
        NSUInteger request = (NSUInteger)MIN(65536ULL, MAX(1ULL, byteBudget - bytesRead));
        NSData *chunk = [handle readDataOfLength:request];
        if (chunk.length == 0) { reachedEOF = YES; break; }
        bytesRead += chunk.length;
        const uint8_t *bytes = chunk.bytes;
        NSUInteger segmentStart = 0;
        for (NSUInteger index = 0; index < chunk.length; index++) {
            if (bytes[index] != '\n') continue;
            NSUInteger segmentLength = index - segmentStart;
            NSUInteger available = lineBuffer.length < CodexCostMaxRetainedLineBytes ? CodexCostMaxRetainedLineBytes - lineBuffer.length : 0;
            if (available > 0 && segmentLength > 0) [lineBuffer appendBytes:bytes + segmentStart length:MIN(available, segmentLength)];
            currentLineBytes += segmentLength + 1;
            NSString *line = [[NSString alloc] initWithData:lineBuffer encoding:NSUTF8StringEncoding];
            if (!line && lineBuffer.length > 3) {
                for (NSUInteger trim = 1; trim <= 3 && !line; trim++) line = [[NSString alloc] initWithData:[lineBuffer subdataWithRange:NSMakeRange(0, lineBuffer.length - trim)] encoding:NSUTF8StringEncoding];
            }
            if (line.length > 0) lineHandler(line);
            lastCompleteOffset += currentLineBytes;
            currentLineBytes = 0;
            [lineBuffer setLength:0];
            segmentStart = index + 1;
        }
        NSUInteger trailingLength = chunk.length - segmentStart;
        NSUInteger available = lineBuffer.length < CodexCostMaxRetainedLineBytes ? CodexCostMaxRetainedLineBytes - lineBuffer.length : 0;
        if (available > 0 && trailingLength > 0) [lineBuffer appendBytes:bytes + segmentStart length:MIN(available, trailingLength)];
        currentLineBytes += trailingLength;
    }
    if (!reachedEOF) reachedEOF = [handle offsetInFile] >= fileSize;
    if (reachedEOF && lineBuffer.length > 0) {
        NSString *line = [[NSString alloc] initWithData:lineBuffer encoding:NSUTF8StringEncoding];
        NSData *data = [line dataUsingEncoding:NSUTF8StringEncoding];
        if (data && [NSJSONSerialization JSONObjectWithData:data options:0 error:nil]) {
            lineHandler(line);
            lastCompleteOffset = fileSize;
            [lineBuffer setLength:0];
        }
    }
    [handle closeFile];
    if (complete) *complete = reachedEOF && lastCompleteOffset == fileSize;
    if (reachedEnd) *reachedEnd = reachedEOF;
    return lastCompleteOffset;
}

static NSDictionary *CodexLoadDictionary(NSURL *url) {
    NSData *data = url ? [NSData dataWithContentsOfURL:url] : nil;
    NSDictionary *object = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    return [object isKindOfClass:NSDictionary.class] ? object : @{};
}

static BOOL CodexWriteJSONObject(id object, NSURL *url) {
    if (!object || !url) return NO;
    [NSFileManager.defaultManager createDirectoryAtURL:url.URLByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:nil];
    NSData *data = [NSJSONSerialization dataWithJSONObject:object options:0 error:nil];
    return data && [data writeToURL:url options:NSDataWritingAtomic error:nil];
}

NSDictionary<NSString *, id> *CodexAggregateCostEvents(NSArray<NSDictionary<NSString *, id> *> *events, NSDate *now, BOOL scanIncomplete) {
    NSDate *referenceNow = now ?: NSDate.date;
    NSCalendar *calendar = [NSCalendar calendarWithIdentifier:NSCalendarIdentifierGregorian];
    calendar.timeZone = NSTimeZone.localTimeZone;
    NSDate *today = [calendar startOfDayForDate:referenceNow];
    NSDateFormatter *formatter = CodexCostDayFormatter();
    NSMutableDictionary<NSString *, NSMutableDictionary *> *daily = [NSMutableDictionary dictionary];
    NSMutableDictionary<NSString *, NSNumber *> *modelTokens = [NSMutableDictionary dictionary];
    NSMutableSet<NSString *> *seen = [NSMutableSet set];
    long long totalTokens = 0, pricedTokens = 0;
    for (NSDictionary *event in events ?: @[]) {
        NSString *fingerprint = [event[@"k"] isKindOfClass:NSString.class] ? event[@"k"] : nil;
        if (fingerprint.length > 0 && [seen containsObject:fingerprint]) continue;
        if (fingerprint.length > 0) [seen addObject:fingerprint];
        NSString *day = [event[@"d"] isKindOfClass:NSString.class] ? event[@"d"] : nil;
        NSDate *date = day ? [formatter dateFromString:day] : nil;
        if (!date || [date compare:today] == NSOrderedDescending) continue;
        long long input = [event[@"i"] longLongValue], cached = [event[@"c"] longLongValue], write = [event[@"w"] longLongValue], output = [event[@"o"] longLongValue];
        long long tokens = MAX(0, input) + MAX(0, output);
        if (tokens == 0) continue;
        NSString *model = [event[@"m"] isKindOfClass:NSString.class] ? event[@"m"] : @"unknown";
        NSNumber *storedCost = [event[@"x"] isKindOfClass:NSNumber.class] ? event[@"x"] : nil;
        NSNumber *storedPricedTokens = [event[@"p"] isKindOfClass:NSNumber.class] ? event[@"p"] : nil;
        NSDictionary *estimate = storedCost && storedPricedTokens ? nil : CodexCostEstimateForTokens(model, input, cached, write, output);
        long long eventPricedTokens = storedPricedTokens ? MIN(tokens, MAX(0LL, storedPricedTokens.longLongValue)) : ([estimate[@"available"] boolValue] ? tokens : 0);
        double cost = storedCost ? storedCost.doubleValue : ([estimate[@"available"] boolValue] ? [estimate[@"cost"] doubleValue] : 0);
        NSMutableDictionary *row = daily[day];
        if (!row) { row = [@{ @"tokens": @0LL, @"cost": @0.0, @"pricedTokens": @0LL } mutableCopy]; daily[day] = row; }
        row[@"tokens"] = @([row[@"tokens"] longLongValue] + tokens);
        row[@"cost"] = @([row[@"cost"] doubleValue] + cost);
        row[@"pricedTokens"] = @([row[@"pricedTokens"] longLongValue] + eventPricedTokens);
        modelTokens[model] = @([modelTokens[model] longLongValue] + tokens);
        totalTokens += tokens;
        pricedTokens += eventPricedTokens;
    }
    long long todayTokens = 0, sevenTokens = 0, thirtyTokens = 0;
    double todayCost = 0, sevenCost = 0, thirtyCost = 0, monthCost = 0;
    NSMutableArray<NSNumber *> *trendRaw = [NSMutableArray array];
    for (NSInteger offset = 29; offset >= 0; offset--) {
        NSDate *date = [calendar dateByAddingUnit:NSCalendarUnitDay value:-offset toDate:today options:0];
        NSString *key = [formatter stringFromDate:date];
        NSDictionary *row = daily[key];
        long long tokens = [row[@"tokens"] longLongValue];
        double cost = [row[@"cost"] doubleValue];
        thirtyTokens += tokens; thirtyCost += cost;
        if (offset <= 6) { sevenTokens += tokens; sevenCost += cost; }
        if (offset == 0) { todayTokens = tokens; todayCost = cost; }
        if (offset <= 13) [trendRaw addObject:@(tokens)];
    }
    NSDateComponents *monthParts = [calendar components:NSCalendarUnitYear | NSCalendarUnitMonth fromDate:today];
    NSDate *monthStart = [calendar dateFromComponents:monthParts];
    NSRange daysRange = [calendar rangeOfUnit:NSCalendarUnitDay inUnit:NSCalendarUnitMonth forDate:today];
    for (NSString *key in daily) {
        NSDate *date = [formatter dateFromString:key];
        if (date && [date compare:monthStart] != NSOrderedAscending && [date compare:today] != NSOrderedDescending) monthCost += [daily[key][@"cost"] doubleValue];
    }
    NSInteger dayOfMonth = [calendar component:NSCalendarUnitDay fromDate:today];
    NSTimeInterval secondsToday = [referenceNow timeIntervalSinceDate:today];
    double elapsedDays = MAX(1.0, (double)(dayOfMonth - 1) + MIN(1.0, secondsToday / 86400.0));
    double monthForecast = monthCost > 0 ? monthCost / elapsedDays * (double)daysRange.length : 0;
    long long maxTrend = 0;
    for (NSNumber *value in trendRaw) maxTrend = MAX(maxTrend, value.longLongValue);
    NSMutableArray<NSNumber *> *trend = [NSMutableArray array];
    for (NSNumber *value in trendRaw) [trend addObject:@(maxTrend > 0 ? value.doubleValue / (double)maxTrend * 100.0 : 0)];
    __block NSString *topModel = @"";
    __block long long topTokens = 0;
    [modelTokens enumerateKeysAndObjectsUsingBlock:^(NSString *key, NSNumber *value, __unused BOOL *stop) {
        if (value.longLongValue > topTokens) { topTokens = value.longLongValue; topModel = key; }
    }];
    double coverage = totalTokens > 0 ? (double)pricedTokens / (double)totalTokens * 100.0 : 0;
    return @{
        @"available": @(totalTokens > 0), @"scanIncomplete": @(scanIncomplete),
        @"todayTokens": @(todayTokens), @"sevenDayTokens": @(sevenTokens), @"thirtyDayTokens": @(thirtyTokens),
        @"todayCost": @(todayCost), @"sevenDayCost": @(sevenCost), @"thirtyDayCost": @(thirtyCost),
        @"monthCost": @(monthCost), @"monthForecastCost": @(monthForecast),
        @"pricedTokenPercent": @(coverage), @"topModel": topModel, @"dailyTrend": trend,
        @"pricingVersion": CodexCostPricingVersion, @"eventCount": @(seen.count)
    };
}

NSDictionary<NSString *, id> *CodexScanCostHistoryAtHome(NSURL *codexHome, NSURL *cacheURL, NSDate *now) {
    NSDate *referenceNow = now ?: NSDate.date;
    NSArray<NSURL *> *files = CodexCostCandidateFiles(codexHome, referenceNow);
    if (files.count == 0) return @{ @"available": @NO, @"error": @"未找到本机会话Token记录", @"pricingVersion": CodexCostPricingVersion };
    NSDictionary *cache = CodexLoadDictionary(cacheURL);
    NSDictionary *cachedFiles = [cache[@"version"] integerValue] == CodexCostCacheVersion && [cache[@"files"] isKindOfClass:NSDictionary.class] ? cache[@"files"] : @{};
    NSMutableDictionary *newFiles = [NSMutableDictionary dictionary];
    NSCalendar *calendar = [NSCalendar calendarWithIdentifier:NSCalendarIdentifierGregorian];
    calendar.timeZone = NSTimeZone.localTimeZone;
    NSDate *earliest = [calendar dateByAddingUnit:NSCalendarUnitDay value:-(CodexCostHistoryDays - 1) toDate:[calendar startOfDayForDate:referenceNow] options:0];
    NSDateFormatter *formatter = CodexCostDayFormatter();
    unsigned long long remainingBudget = CodexCostScanBudgetBytes;
    BOOL incomplete = NO;
    for (NSURL *file in files) {
        NSDictionary *values = [file resourceValuesForKeys:@[NSURLContentModificationDateKey, NSURLFileSizeKey, NSURLIsRegularFileKey] error:nil];
        if (![values[NSURLIsRegularFileKey] boolValue]) continue;
        unsigned long long size = [values[NSURLFileSizeKey] unsignedLongLongValue];
        NSTimeInterval mtime = [values[NSURLContentModificationDateKey] timeIntervalSince1970];
        NSString *fileKey = CodexPathCacheKey(file.path);
        NSDictionary *old = [cachedFiles[fileKey] isKindOfClass:NSDictionary.class] ? cachedFiles[fileKey] : nil;
        BOOL unchanged = old && [old[@"size"] unsignedLongLongValue] == size && fabs([old[@"mtime"] doubleValue] - mtime) < 0.001 && [old[@"complete"] boolValue];
        if (unchanged) { newFiles[fileKey] = old; continue; }
        if (remainingBudget == 0) { if (old) newFiles[fileKey] = old; incomplete = YES; continue; }
        unsigned long long oldSize = [old[@"size"] unsignedLongLongValue];
        unsigned long long oldParsedBytes = [old[@"parsedBytes"] unsignedLongLongValue];
        BOOL append = old && oldParsedBytes > 0 && oldParsedBytes < size && size >= oldSize &&
                      (([old[@"complete"] boolValue] && oldSize < size) || ![old[@"complete"] boolValue]);
        unsigned long long startOffset = append ? [old[@"parsedBytes"] unsignedLongLongValue] : 0;
        NSMutableArray *events = append && [old[@"events"] isKindOfClass:NSArray.class] ? [old[@"events"] mutableCopy] : [NSMutableArray array];
        NSMutableDictionary *state = append && [old[@"state"] isKindOfClass:NSDictionary.class] ? [old[@"state"] mutableCopy] : [@{ @"model": @"unknown", @"occurrences": @{} } mutableCopy];
        if ([state[@"occurrences"] isKindOfClass:NSDictionary.class]) state[@"occurrences"] = [state[@"occurrences"] mutableCopy];
        BOOL complete = NO;
        BOOL reachedEnd = NO;
        unsigned long long parsed = CodexParseFileFromOffset(file, startOffset, remainingBudget, ^(NSString *line) {
            CodexParseCostLine(line, state, events, earliest, formatter);
        }, &complete, &reachedEnd);
        unsigned long long consumed = parsed > startOffset ? parsed - startOffset : MIN(size - startOffset, remainingBudget);
        remainingBudget = consumed >= remainingBudget ? 0 : remainingBudget - consumed;
        NSArray *compactedEvents = CodexCompactedCostEvents(events, fileKey);
        [state removeObjectForKey:@"occurrences"];
        NSDictionary *entry = @{ @"size": @(size), @"mtime": @(mtime), @"parsedBytes": @(parsed), @"complete": @(complete), @"state": state, @"events": compactedEvents };
        newFiles[fileKey] = entry;
        if (!complete && !reachedEnd) incomplete = YES;
    }
    CodexWriteJSONObject(@{ @"version": @(CodexCostCacheVersion), @"updatedAt": @(referenceNow.timeIntervalSince1970), @"files": newFiles }, cacheURL);
    NSMutableArray *allEvents = [NSMutableArray array];
    for (NSDictionary *entry in newFiles.allValues) if ([entry[@"events"] isKindOfClass:NSArray.class]) [allEvents addObjectsFromArray:entry[@"events"]];
    NSMutableDictionary *aggregate = [CodexAggregateCostEvents(allEvents, referenceNow, incomplete) mutableCopy];
    aggregate[@"updatedAt"] = @(referenceNow.timeIntervalSince1970);
    if (![aggregate[@"available"] boolValue]) aggregate[@"error"] = incomplete ? @"正在补齐本机Token历史" : @"本机记录暂时没有Token数据";
    return aggregate;
}

NSDictionary<NSString *, id> *CodexQuotaForecastFromSamples(NSArray<NSDictionary<NSString *, id> *> *samples,
                                                              NSString *remainingKey,
                                                              NSString *resetKey,
                                                              double currentRemaining,
                                                              NSTimeInterval currentResetAt,
                                                              NSDate *now) {
    NSDate *referenceNow = now ?: NSDate.date;
    NSTimeInterval nowValue = referenceNow.timeIntervalSince1970;
    if (currentResetAt <= nowValue || remainingKey.length == 0 || resetKey.length == 0) return @{ @"available": @NO };
    NSMutableArray<NSDictionary *> *valid = [NSMutableArray array];
    for (NSDictionary *sample in samples ?: @[]) {
        NSNumber *timestamp = [sample[@"t"] isKindOfClass:NSNumber.class] ? sample[@"t"] : nil;
        NSNumber *remaining = [sample[remainingKey] isKindOfClass:NSNumber.class] ? sample[remainingKey] : nil;
        NSNumber *reset = [sample[resetKey] isKindOfClass:NSNumber.class] ? sample[resetKey] : nil;
        if (!timestamp || !remaining || !reset || fabs(reset.doubleValue - currentResetAt) > 300.0) continue;
        if (nowValue - timestamp.doubleValue > 24.0 * 3600.0 || timestamp.doubleValue > nowValue + 60.0) continue;
        [valid addObject:@{ @"t": timestamp, @"r": remaining }];
    }
    [valid sortUsingComparator:^NSComparisonResult(NSDictionary *left, NSDictionary *right) { return [left[@"t"] compare:right[@"t"]]; }];
    if (valid.count < 3) return @{ @"available": @NO, @"detail": @"至少需要15分钟历史" };
    NSTimeInterval firstTime = [valid.firstObject[@"t"] doubleValue];
    NSTimeInterval span = [valid.lastObject[@"t"] doubleValue] - firstTime;
    if (span < 900.0) return @{ @"available": @NO, @"detail": @"至少需要15分钟历史" };
    double meanX = 0, meanY = 0;
    for (NSDictionary *sample in valid) { meanX += ([sample[@"t"] doubleValue] - firstTime) / 3600.0; meanY += [sample[@"r"] doubleValue]; }
    meanX /= valid.count; meanY /= valid.count;
    double numerator = 0, denominator = 0;
    for (NSDictionary *sample in valid) {
        double x = ([sample[@"t"] doubleValue] - firstTime) / 3600.0;
        double y = [sample[@"r"] doubleValue];
        numerator += (x - meanX) * (y - meanY); denominator += (x - meanX) * (x - meanX);
    }
    double ratePerHour = denominator > 0 ? MAX(0, -numerator / denominator) : 0;
    NSString *confidence = span >= 6 * 3600.0 && valid.count >= 12 ? @"高" : (span >= 3600.0 && valid.count >= 6 ? @"中" : @"低");
    NSTimeInterval secondsToReset = currentResetAt - nowValue;
    if (ratePerHour < 0.02) return @{ @"available": @YES, @"headline": @"近期用量平稳", @"detail": @"按当前速度可撑到重置", @"confidence": confidence, @"projectedRemaining": @(currentRemaining) };
    double projected = currentRemaining - ratePerHour * secondsToReset / 3600.0;
    NSTimeInterval exhaustAt = nowValue + currentRemaining / ratePerHour * 3600.0;
    if (projected > 0) return @{ @"available": @YES, @"headline": @"可撑到重置", @"detail": [NSString stringWithFormat:@"预计重置时剩余%.0f%% · %@可信", MIN(100.0, projected), confidence], @"confidence": confidence, @"projectedRemaining": @(projected), @"exhaustAt": @(exhaustAt) };
    NSTimeInterval hours = MAX(0, (exhaustAt - nowValue) / 3600.0);
    NSString *timeText = hours < 1.0 ? [NSString stringWithFormat:@"约%.0f分钟后", hours * 60.0] : [NSString stringWithFormat:@"约%.1f小时后", hours];
    return @{ @"available": @YES, @"headline": @"可能提前用完", @"detail": [NSString stringWithFormat:@"%@ · %@可信", timeText, confidence], @"confidence": confidence, @"projectedRemaining": @(projected), @"exhaustAt": @(exhaustAt) };
}

NSDictionary<NSString *, id> *CodexUpdateQuotaForecastHistory(NSURL *historyURL, NSDictionary<NSString *, id> *sample, NSDate *now) {
    NSDate *referenceNow = now ?: NSDate.date;
    NSDictionary *stored = CodexLoadDictionary(historyURL);
    NSArray<NSDictionary *> *storedSamples = [stored[@"samples"] isKindOfClass:NSArray.class] ? stored[@"samples"] : @[];
    NSMutableArray<NSDictionary *> *samples = [storedSamples mutableCopy];
    NSTimeInterval cutoff = referenceNow.timeIntervalSince1970 - 30.0 * 86400.0;
    NSIndexSet *expired = [samples indexesOfObjectsPassingTest:^BOOL(NSDictionary *item, __unused NSUInteger idx, __unused BOOL *stop) { return [item[@"t"] doubleValue] < cutoff; }];
    if (expired.count > 0) [samples removeObjectsAtIndexes:expired];
    NSDictionary *last = samples.lastObject;
    if (!last || referenceNow.timeIntervalSince1970 - [last[@"t"] doubleValue] >= 60.0) {
        NSMutableDictionary *row = [sample mutableCopy] ?: [NSMutableDictionary dictionary];
        row[@"t"] = @(referenceNow.timeIntervalSince1970);
        [samples addObject:row];
    }
    if (samples.count > 10000) [samples removeObjectsInRange:NSMakeRange(0, samples.count - 10000)];
    CodexWriteJSONObject(@{ @"version": @1, @"samples": samples }, historyURL);
    NSMutableDictionary *result = [NSMutableDictionary dictionary];
    NSNumber *five = [sample[@"f"] isKindOfClass:NSNumber.class] ? sample[@"f"] : nil;
    NSNumber *fiveReset = [sample[@"fr"] isKindOfClass:NSNumber.class] ? sample[@"fr"] : nil;
    NSNumber *weekly = [sample[@"w"] isKindOfClass:NSNumber.class] ? sample[@"w"] : nil;
    NSNumber *weeklyReset = [sample[@"wr"] isKindOfClass:NSNumber.class] ? sample[@"wr"] : nil;
    if (five && fiveReset) result[@"fiveHour"] = CodexQuotaForecastFromSamples(samples, @"f", @"fr", five.doubleValue, fiveReset.doubleValue, referenceNow);
    if (weekly && weeklyReset) result[@"weekly"] = CodexQuotaForecastFromSamples(samples, @"w", @"wr", weekly.doubleValue, weeklyReset.doubleValue, referenceNow);
    return result;
}
