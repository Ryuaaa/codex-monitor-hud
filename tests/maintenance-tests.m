#import <Cocoa/Cocoa.h>
#import "CodexStatusProvider.h"
#import "CodexProtocolCompatibility.h"
#import "CodexCostHistory.h"
#import "UpdateManager.h"
#import "HUDLocalization.h"

// Synthetic fixtures only: no real app-server, user session files, launchctl or installer.
@interface CodexStatusProvider (MaintenanceAccess)
- (void)consumeUsage:(NSDictionary *)result;
- (void)consumeAccount:(NSDictionary *)result;
- (void)handleObject:(NSDictionary *)object;
- (void)finishFetch;
@end
@interface MaintenanceProvider : CodexStatusProvider
@property(nonatomic) NSMutableArray *sent;
@end
@implementation MaintenanceProvider
- (instancetype)init { if ((self = [super init])) { _sent = [NSMutableArray array]; self.costHistoryEnabled = NO; self.quotaForecastEnabled = NO; } return self; }
- (void)sendObject:(NSDictionary *)object { [self.sent addObject:object]; }
- (void)refreshActivity {}
- (void)refreshCostHistory {}
@end

static NSUInteger checks = 0, failures = 0;
static void Check(BOOL pass, const char *name) { checks++; if (!pass) { failures++; fprintf(stderr, "FAIL %s\n", name); } }
static void Write(NSString *text, NSURL *url, BOOL executable) {
    [NSFileManager.defaultManager createDirectoryAtURL:url.URLByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:nil];
    Check([text writeToURL:url atomically:YES encoding:NSUTF8StringEncoding error:nil], "fixture saved");
    if (executable) [NSFileManager.defaultManager setAttributes:@{NSFilePosixPermissions: @0700} ofItemAtPath:url.path error:nil];
}
static NSDictionary *JSON(NSURL *url) {
    return [NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfURL:url] options:NSJSONReadingMutableContainers error:nil];
}
static NSString *TokenLine(NSDate *now, id input) {
    NSString *timestamp = [[NSISO8601DateFormatter new] stringFromDate:now];
    NSDictionary *event = @{@"timestamp": timestamp, @"type": @"event_msg", @"payload": @{@"type": @"token_count", @"info": @{@"model": @"gpt-5.6", @"total_token_usage": @{@"input_tokens": input, @"output_tokens": @100}}}};
    return [[[NSString alloc] initWithData:[NSJSONSerialization dataWithJSONObject:event options:NSJSONWritingWithoutEscapingSlashes error:nil] encoding:NSUTF8StringEncoding] stringByAppendingString:@"\n"];
}

static void UsageTests(void) {
    NSDate *now = NSDate.date;
    for (id invalid in @[NSNull.null, @[], @{}, @"100", @YES, @(-1), @1.5, @(INFINITY), @(NAN), @(0x1p63)]) {
        MaintenanceProvider *p = [MaintenanceProvider new]; p.snapshot.usageAvailable = YES; p.snapshot.sevenDayTokens = 500; p.snapshot.usageUpdatedAt = 123;
        [p consumeUsage:@{@"dailyUsageBuckets": @[@{@"startDate": @"2026-09-04", @"tokens": invalid}]}];
        Check(p.snapshot.sevenDayTokens == 500 && p.snapshot.usageUpdatedAt == 123 && p.snapshot.usageErrorText.length > 0, "invalid daily token preserves prior value/time");
    }
    MaintenanceProvider *p = [MaintenanceProvider new]; p.snapshot.usageAvailable = YES; p.snapshot.sevenDayTokens = 500; p.snapshot.thirtyDayTokens = 900; p.snapshot.usageUpdatedAt = 123;
    for (id invalid in @[NSNull.null, @"bad", @[]]) [p consumeUsage:@{@"dailyUsageBuckets": @[invalid]}];
    Check(p.snapshot.sevenDayTokens == 500, "malformed nested daily elements isolated");
    p.snapshot.peakDailyTokens = 600; p.snapshot.peakDailyTokensAvailable = YES;
    [p consumeUsage:@{@"summary": @{@"lifetimeTokens": @1000}}];
    Check(p.snapshot.sevenDayTokens == 500 && p.snapshot.thirtyDayTokens == 900 && p.snapshot.usageUpdatedAt == 123 && p.snapshot.lifetimeTokens == 1000, "partial summary not zero or newly dated calendar");
    Check(p.snapshot.peakDailyTokens == 600 && p.snapshot.peakDailyTokensAvailable, "missing optional summary retained");
    [p consumeUsage:@{@"dailyUsageBuckets": @[], @"summary": @{@"peakDailyTokens": @0}}];
    Check(p.snapshot.sevenDayTokens == 0 && p.snapshot.peakDailyTokens == 0 && p.snapshot.peakDailyTokensAvailable, "explicit empty calendar and numeric zero authoritative");
    NSString *invalidLine = TokenLine(now, NSNull.null);
    NSDictionary *parsed = CodexCostEventsFromJSONLLines(@[invalidLine, TokenLine(now, @900)], now, 30);
    Check([parsed[@"events"] count] == 1, "bad local token skipped; next valid line survives");
    for (id invalid in @[@YES, @"1", @[], @{}, @(-1), @1.5]) Check([CodexCostEventsFromJSONLLines(@[TokenLine(now, invalid)], now, 30)[@"events"] count] == 0, "invalid local counts do not enter totals");
    (void)CodexCostEventsFromJSONLLines(@[@"[{\"type\":\"turn_context\"}]"], now, 30);
    Check(CodexAddTokenCounts(LLONG_MAX - 1, 10) == LLONG_MAX, "token addition does not overflow");

    MaintenanceProvider *onlyQuota = [MaintenanceProvider new]; onlyQuota.enabledRequestIDs = [NSSet setWithObject:@2];
    [onlyQuota handleObject:@{@"id": @1, @"result": @{}}];
    Check(onlyQuota.sent.count == 2 && [onlyQuota.sent.lastObject[@"method"] isEqual:@"account/rateLimits/read"], "quota only never requests hidden modules");
    [onlyQuota handleObject:@{@"id": @2, @"result": @{}}];
    Check(![[onlyQuota valueForKey:@"initialized"] boolValue], "disabled replies not awaited");
    MaintenanceProvider *onlyThreads = [MaintenanceProvider new]; onlyThreads.enabledRequestIDs = [NSSet setWithObject:@5];
    [onlyThreads handleObject:@{@"id": @1, @"result": @{}}];
    Check(onlyThreads.sent.count == 2 && [onlyThreads.sent.lastObject[@"method"] isEqual:@"thread/list"], "task only no account requests");
    [onlyThreads consumeAccount:@{@"account": NSNull.null}]; [onlyThreads finishFetch];
    Check(onlyThreads.recommendedRetryInterval == 0 && onlyThreads.snapshot.interfaceFailureKinds.count == 0, "normal missing optional field is not transport failure");
}

static void HistoryTests(NSURL *root) {
    NSURL *home = [root URLByAppendingPathComponent:@"synthetic-home"], *cache = [root URLByAppendingPathComponent:@"cache.json"], *marker = [root URLByAppendingPathComponent:@"marker.json"];
    NSDate *now = NSDate.date, *start = [now dateByAddingTimeInterval:-3600];
    (void)CodexScanCostHistoryAtHome(home, cache, marker, start);
    NSDateFormatter *folder = [NSDateFormatter new]; folder.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"]; folder.dateFormat = @"yyyy/MM/dd";
    NSString *oldDate = [folder stringFromDate:[now dateByAddingTimeInterval:-40 * 86400]];
    NSURL *rollout = [home URLByAppendingPathComponent:[NSString stringWithFormat:@"sessions/%@/synthetic.jsonl", oldDate]];
    Write(TokenLine([now dateByAddingTimeInterval:-1], @900), rollout, NO);
    NSDictionary *first = CodexScanCostHistoryAtHome(home, cache, marker, now);
    if ([first[@"sevenDayTokens"] longLongValue] != 1000) {
        fprintf(stderr, "synthetic_history_result=%s home=%s now=%.3f\n", first.description.UTF8String, home.path.UTF8String, now.timeIntervalSince1970);
        NSURL *sessions = [home URLByAppendingPathComponent:@"sessions"];
        NSArray *keys = @[NSURLIsDirectoryKey, NSURLIsRegularFileKey, NSURLIsSymbolicLinkKey, NSURLContentModificationDateKey];
        for (NSURL *entry in [NSFileManager.defaultManager enumeratorAtURL:sessions includingPropertiesForKeys:keys options:0 errorHandler:nil]) fprintf(stderr, "synthetic_entry=%s attrs=%s\n", entry.path.UTF8String, [entry resourceValuesForKeys:keys error:nil].description.UTF8String);
    }
    Check([first[@"available"] boolValue] && [first[@"sevenDayTokens"] longLongValue] == 1000, "recent tokens in old conversation counted");
    NSDictionary *second = CodexScanCostHistoryAtHome(home, cache, marker, now);
    Check([second[@"sevenDayTokens"] longLongValue] == 1000, "old conversation repeat scan deduplicated");
    NSMutableDictionary *oversize = [JSON(cache) mutableCopy];
    oversize[@"syntheticPadding"] = [@"x" stringByPaddingToLength:8 * 1024 * 1024 withString:@"x" startingAtIndex:0];
    [[NSJSONSerialization dataWithJSONObject:oversize options:0 error:nil] writeToURL:cache atomically:YES];
    NSDictionary *recovered = CodexScanCostHistoryAtHome(home, cache, marker, now);
    Check([recovered[@"sevenDayTokens"] longLongValue] == 1000 && fabs([recovered[@"trackingStartedAt"] doubleValue] - start.timeIntervalSince1970) < 1, "oversize cache recovers backup without changing start");
    Check([NSData dataWithContentsOfURL:cache].length <= 8 * 1024 * 1024, "recovered cache stays within read limit");
    Write(@"{broken", cache, NO);
    recovered = CodexScanCostHistoryAtHome(home, cache, marker, now);
    Check([recovered[@"sevenDayTokens"] longLongValue] == 1000, "truncated cache recovers backup");
    Write(@"{broken", cache, NO); Write(@"{broken", [cache URLByAppendingPathExtension:@"previous"], NO);
    NSDictionary *unrecoverable = CodexScanCostHistoryAtHome(home, cache, marker, now);
    Check([unrecoverable[@"preservePrevious"] boolValue] && ![unrecoverable[@"available"] boolValue], "no valid backup is explicit unavailable, not reset");
    Check(fabs([JSON(marker)[@"startedAt"] doubleValue] - start.timeIntervalSince1970) < 1 && [[NSString stringWithContentsOfURL:cache encoding:NSUTF8StringEncoding error:nil] isEqual:@"{broken"], "unrecoverable history and epoch retained untouched");
}

static void CountingRepairTests(NSURL *root) {
    NSDate *now = NSDate.date;
    NSString *child = @"{\"type\":\"session_meta\",\"payload\":{\"source\":{\"subagent\":{}}}}";
    NSArray *lines = @[child, TokenLine(now, @1000000000), TokenLine(now, @1000000200), TokenLine(now, @1000000200), TokenLine(now, @999999999)];
    NSDictionary *parsed = CodexCostEventsFromJSONLLines(lines, now, 30);
    Check([CodexAggregateCostEvents(parsed[@"events"], now, NO)[@"todayTokens"] longLongValue] == 200, "inherited billion excluded; duplicate and regressed totals ignored");
    NSArray *rootLines = @[TokenLine(now, @900), TokenLine(now, @900)];
    Check([CodexAggregateCostEvents(CodexCostEventsFromJSONLLines(rootLines, now, 30)[@"events"], now, NO)[@"todayTokens"] longLongValue] == 1000, "root initial usage counted exactly once");
    NSString *zero = [TokenLine(now, @0) stringByReplacingOccurrencesOfString:@"\"output_tokens\":100" withString:@"\"output_tokens\":0"];
    Check([CodexAggregateCostEvents(CodexCostEventsFromJSONLLines(@[child, zero, TokenLine(now, @900)], now, 30)[@"events"], now, NO)[@"todayTokens"] longLongValue] == 1000, "explicit zero child baseline retains first real request");
    (void)CodexCostEventsFromJSONLLines(@[@"[{\"type\":\"session_meta\"}]"], now, 30);
    Check(fabs([CodexCostEstimateForTokens(@"gpt-6-astra", 100000, 50000, 10000, 10000)[@"cost"] doubleValue] - 1.075) < 1e-9, "Astra separate input/cache-write/output per million");
    Check(fabs([CodexCostEstimateForTokens(@"gpt-5.6-sol", 100000, 50000, 10000, 10000)[@"cost"] doubleValue] - .43) < 1e-9, "Sol current standard reference rate");
    Check(![CodexCostEstimateForTokens(@"unknown", 10000, 0, 0, 100)[@"available"] boolValue], "unknown price never guessed");

    NSURL *home = [root URLByAppendingPathComponent:@"repair-home"], *cache = [root URLByAppendingPathComponent:@"repair-cache.json"], *marker = [root URLByAppendingPathComponent:@"repair-marker.json"];
    NSDate *start = [now dateByAddingTimeInterval:-3600];
    (void)CodexScanCostHistoryAtHome(home, cache, marker, start);
    NSDateFormatter *f = [NSDateFormatter new]; f.dateFormat = @"yyyy/MM/dd";
    NSURL *file = [home URLByAppendingPathComponent:[NSString stringWithFormat:@"sessions/%@/synthetic-child.jsonl", [f stringFromDate:now]]];
    Write([lines componentsJoinedByString:@"\n"], file, NO);
    Write([[lines componentsJoinedByString:@"\n"] stringByAppendingString:@"\n"], file, NO);
    NSDictionary *first = CodexScanCostHistoryAtHome(home, cache, marker, now);
    Check([first[@"thirtyDayTokens"] longLongValue] == 200, "child scanner matches independently checked delta");
    NSMutableDictionary *legacy = [JSON(cache) mutableCopy];
    for (NSMutableDictionary *entry in [legacy[@"files"] allValues]) [entry removeObjectForKey:@"countingVersion"];
    [[NSJSONSerialization dataWithJSONObject:legacy options:0 error:nil] writeToURL:cache atomically:YES];
    NSDictionary *migrated = CodexScanCostHistoryAtHome(home, cache, marker, now);
    NSURL *backup = [cache URLByAppendingPathExtension:@"before-counting-repair"];
    NSData *backupData = [NSData dataWithContentsOfURL:backup];
    Check(backupData.length > 0 && [migrated[@"thirtyDayTokens"] longLongValue] == 200, "legacy cache backed up and rebuilt");
    NSDictionary *again = CodexScanCostHistoryAtHome(home, cache, marker, now);
    Check([again[@"thirtyDayTokens"] longLongValue] == 200 && [again[@"thirtyDayCost"] isEqual:first[@"thirtyDayCost"]], "repeat scan keeps tokens and cost unchanged");
    Check([[NSData dataWithContentsOfURL:backup] isEqual:backupData] && fabs([JSON(marker)[@"startedAt"] doubleValue] - start.timeIntervalSince1970) < 1, "migration backup immutable; installation epoch preserved");
    NSDictionary *arguments = [NSUserDefaults.standardUserDefaults volatileDomainForName:NSArgumentDomain];
    for (NSString *currency in @[@"CNY", @"USD", @"EUR", @"JPY", @"KRW"]) {
        NSMutableDictionary *temporary = [arguments mutableCopy]; temporary[@"displayCurrency"] = currency;
        [NSUserDefaults.standardUserDefaults setVolatileDomain:temporary forName:NSArgumentDomain];
        Check([HUDMoney(1) hasPrefix:currency] && [HUDMoney(NAN) isEqual:@"--"], "currency formatting does not mutate counts or show nonfinite costs");
    }
    [NSUserDefaults.standardUserDefaults setVolatileDomain:arguments forName:NSArgumentDomain];
}

static void UpdateTest(NSURL *root, NSString *mode, BOOL success, BOOL keepNew) {
    NSURL *dir = [root URLByAppendingPathComponent:mode], *source = [dir URLByAppendingPathComponent:@"New Codex Monitor HUD.app"], *target = [dir URLByAppendingPathComponent:@"Codex Monitor HUD.app"], *work = [dir URLByAppendingPathComponent:@"work"];
    Write(@"new", [source URLByAppendingPathComponent:@"marker"], NO); Write(@"old", [target URLByAppendingPathComponent:@"marker"], NO);
    Write([NSString stringWithFormat:@"#!/bin/sh\nif [ \"$1\" = '-dv' ]; then printf '%%s\\n' 'TeamIdentifier=%@'; fi\nexit 0\n", [mode isEqual:@"wrong-team"] ? @"OTHER" : @"L8K9749GM7"], [dir URLByAppendingPathComponent:@"codesign"], YES);
    Write([mode isEqual:@"signature-failure"] ? @"#!/bin/sh\nexit 1\n" : @"#!/bin/sh\nexit 0\n", [dir URLByAppendingPathComponent:@"spctl"], YES);
    NSString *restart = [NSString stringWithFormat:@"#!/bin/zsh\nmarker=$(/bin/cat \"${@[-1]}/marker\")\n[[ '%@' == *restart-failure && \"$marker\" == new ]] && exit 9\nexit 0\n", mode];
    Write(restart, [dir URLByAppendingPathComponent:@"restart"], YES);
    NSString *probe = [NSString stringWithFormat:@"#!/bin/zsh\nif [[ \"$1\" == --update-stop-probe ]]; then [[ '%@' != stop-failure ]]; exit $?; fi\nif [[ '%@' == early-crash || '%@' == restart-loop ]]; then\n fixture_count=0; [[ ! -f \"$0.count\" ]] || read fixture_count < \"$0.count\"\n (( fixture_count += 1 )); print -r -- \"$fixture_count\" > \"$0.count\"\n if [[ '%@' == restart-loop ]]; then print -r -- $((424242 + fixture_count)); exit 0; fi\n print -r -- 424242; [[ $fixture_count -le 2 ]]; exit $?\nfi\nprint -r -- 424242; [[ '%@' == *healthy ]]\n", mode, mode, mode, mode, mode];
    Write(probe, [work URLByAppendingPathComponent:@"health-probe.app/Contents/MacOS/CodexMonitorHUD"], YES);
    NSMutableDictionary *replace = [@{@"/usr/bin/codesign": [dir URLByAppendingPathComponent:@"codesign"].path, @"/usr/sbin/spctl": [dir URLByAppendingPathComponent:@"spctl"].path, @"/usr/bin/open": [dir URLByAppendingPathComponent:@"restart"].path, @"/bin/launchctl": [dir URLByAppendingPathComponent:@"restart"].path, @"/bin/sleep": @"/usr/bin/true", @"$HOME/Library/LaunchAgents/$label.plist": @"$work_dir/no-agent.plist"} mutableCopy];
    if ([mode isEqual:@"old-not-exiting"]) replace[@"/bin/kill"] = @"/usr/bin/true";
    if ([mode hasPrefix:@"managed-"]) {
        Write(@"synthetic only", [work URLByAppendingPathComponent:@"no-agent.plist"], NO);
        Write([NSString stringWithFormat:@"#!/bin/sh\nprintf '%%s\\n' '%@/Contents/MacOS/CodexMonitorHUD'\n", target.path], [dir URLByAppendingPathComponent:@"plist-reader"], YES);
        Write([NSString stringWithFormat:@"#!/bin/zsh\nif [[ \"$1\" == print ]]; then [[ '%@' != *disabled* ]]; exit $?; fi\nprint -r -- \"$1\" >> '%@/launchctl-activity.log'\nif [[ \"$1\" == bootout ]]; then print bootout > '%@/bootout.log'; exit 0; fi\nmarker=$(/bin/cat '%@/marker')\n[[ \"$marker\" != new ]]\n", mode, dir.path, dir.path, target.path], [dir URLByAppendingPathComponent:@"launchctl"], YES);
        replace[@"/usr/libexec/PlistBuddy"] = [dir URLByAppendingPathComponent:@"plist-reader"].path;
        replace[@"/bin/launchctl"] = [dir URLByAppendingPathComponent:@"launchctl"].path;
    }
    NSString *script = HUDInstallHelperScript(); for (NSString *key in replace) script = [script stringByReplacingOccurrencesOfString:key withString:replace[key]];
    NSURL *helper = [dir URLByAppendingPathComponent:@"helper.zsh"]; Write(script, helper, YES);
    NSTask *task = [NSTask new]; task.executableURL = helper; task.arguments = @[@"2147483647", source.path, target.path, work.path];
    Check([task launchAndReturnError:nil], "isolated helper launch"); [task waitUntilExit];
    Check((task.terminationStatus == 0) == success, mode.UTF8String);
    NSString *installed = [NSString stringWithContentsOfURL:[target URLByAppendingPathComponent:@"marker"] encoding:NSUTF8StringEncoding error:nil];
    Check([installed isEqual:(keepNew ? @"new" : @"old")], "correct build retained");
    BOOL backup = [NSFileManager.defaultManager fileExistsAtPath:[target.path stringByAppendingString:@".update-backup"]];
    Check(backup == [mode isEqual:@"stop-failure"], "backup removed only after healthy start or successful restore");
    if (backup) Check([[NSString stringWithContentsOfFile:[[target.path stringByAppendingString:@".update-backup"] stringByAppendingPathComponent:@"marker"] encoding:NSUTF8StringEncoding error:nil] isEqual:@"old"], "unresponsive process never destroys old backup");
    if ([mode isEqual:@"managed-restart-failure"]) Check([NSFileManager.defaultManager fileExistsAtPath:[dir URLByAppendingPathComponent:@"bootout.log"].path], "managed restart is suspended before rollback");
    if ([mode isEqual:@"managed-disabled-healthy"]) Check(![NSFileManager.defaultManager fileExistsAtPath:[dir URLByAppendingPathComponent:@"launchctl-activity.log"].path], "disabled login service is never enabled or restarted");
}

int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc != 2) return 2;
        NSURL *root = [NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]] isDirectory:YES];
        if (![root.lastPathComponent hasPrefix:@"hud-maintenance-tests."]) return 2;
        @try {
            UsageTests(); HistoryTests(root); CountingRepairTests(root);
            NSURL *invalidBundle = [root URLByAppendingPathComponent:@"Invalid Probe.app"];
            NSURL *probeWork = [root URLByAppendingPathComponent:@"probe-validation"];
            Write(@"unsigned synthetic executable", [invalidBundle URLByAppendingPathComponent:@"Contents/MacOS/CodexMonitorHUD"], YES);
            [NSFileManager.defaultManager createDirectoryAtURL:probeWork withIntermediateDirectories:YES attributes:nil error:nil];
            Check(!HUDPrepareInstallProbe(invalidBundle, probeWork, nil), "invalid probe bundle rejected before replacement");
            Check(![NSFileManager.defaultManager fileExistsAtPath:[probeWork URLByAppendingPathComponent:@"health-probe.app"].path], "invalid copied probe cleaned up");
            NSURL *signedBundle = [root URLByAppendingPathComponent:@"Signed Probe.app"];
            Write(@"<?xml version=\"1.0\"?><plist version=\"1.0\"><dict><key>CFBundleIdentifier</key><string>test.hud.probe</string><key>CFBundleExecutable</key><string>CodexMonitorHUD</string><key>CFBundlePackageType</key><string>APPL</string><key>CFBundleVersion</key><string>1</string></dict></plist>", [signedBundle URLByAppendingPathComponent:@"Contents/Info.plist"], NO);
            NSURL *signedExecutable = [signedBundle URLByAppendingPathComponent:@"Contents/MacOS/CodexMonitorHUD"];
            [NSFileManager.defaultManager createDirectoryAtURL:signedExecutable.URLByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:nil];
            Check([NSFileManager.defaultManager copyItemAtURL:NSBundle.mainBundle.executableURL toURL:signedExecutable error:nil], "synthetic signing fixture copied");
            NSTask *signer = [NSTask new]; signer.executableURL = [NSURL fileURLWithPath:@"/usr/bin/codesign"];
            signer.arguments = @[@"--force", @"--sign", @"-", signedBundle.path];
            BOOL signingStarted = [signer launchAndReturnError:nil];
            Check(signingStarted, "synthetic bundle signer launched");
            if (signingStarted) [signer waitUntilExit];
            Check(signingStarted && signer.terminationStatus == 0, "synthetic bundle signed with Info.plist binding");
            Check(HUDPrepareInstallProbe(signedBundle, probeWork, nil), "complete signed probe bundle copied and verified");
            Check([NSFileManager.defaultManager isExecutableFileAtPath:[probeWork URLByAppendingPathComponent:@"health-probe.app/Contents/MacOS/CodexMonitorHUD"].path], "installer probe remains inside verified bundle");
            UpdateTest(root, @"healthy", YES, YES);
            UpdateTest(root, @"restart-failure", NO, NO);
            UpdateTest(root, @"not-ready", NO, NO);
            UpdateTest(root, @"signature-failure", NO, NO);
            UpdateTest(root, @"stop-failure", NO, YES);
            UpdateTest(root, @"wrong-team", NO, NO);
            UpdateTest(root, @"early-crash", NO, NO);
            UpdateTest(root, @"old-not-exiting", NO, NO);
            UpdateTest(root, @"restart-loop", NO, NO);
            UpdateTest(root, @"managed-restart-failure", NO, NO);
            UpdateTest(root, @"managed-disabled-healthy", YES, YES);
        } @catch (NSException *exception) { Check(NO, "unexpected exception in synthetic fixtures"); fprintf(stderr, "%s\n", exception.name.UTF8String); }
        printf("maintenance_checks=%lu failures=%lu\n", (unsigned long)checks, (unsigned long)failures);
        return failures ? 1 : 0;
    }
}
