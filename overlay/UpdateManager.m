#import "HUDLocalization.h"
#import "UpdateManager.h"

#import <CommonCrypto/CommonDigest.h>
#import <sys/stat.h>
#import <signal.h>

int HUDUpdateProcessProbe(NSString *targetPath, BOOL stop) {
    if (![targetPath isAbsolutePath] || ![targetPath.pathExtension isEqualToString:@"app"]) return 2;
    NSString *expected = targetPath.stringByStandardizingPath.stringByResolvingSymlinksInPath;
    NSMutableArray<NSRunningApplication *> *matches = [NSMutableArray array];
    for (NSRunningApplication *app in NSWorkspace.sharedWorkspace.runningApplications) {
        if (app.processIdentifier == getpid() || app.terminated || ![app.bundleIdentifier isEqualToString:@"com.codexmonitorhud.app"]) continue;
        if ([app.bundleURL.path.stringByStandardizingPath.stringByResolvingSymlinksInPath isEqualToString:expected]) [matches addObject:app];
    }
    if (!stop) {
        if (matches.count != 1 || !matches.firstObject.finishedLaunching || matches.firstObject.terminated) return 1;
        printf("%d\n", matches.firstObject.processIdentifier);
        return 0;
    }
    for (NSRunningApplication *app in matches) kill(app.processIdentifier, SIGTERM);
    for (NSInteger attempt = 0; attempt < 50; attempt++) {
        BOOL alive = NO;
        for (NSRunningApplication *app in matches) if (!app.terminated && kill(app.processIdentifier, 0) == 0) alive = YES;
        if (!alive) return 0;
        [NSThread sleepForTimeInterval:0.1];
    }
    // Never replace files beneath an unresponsive live process; leave the backup recoverable.
    return 1;
}

static NSString *const HUDRepository = @"Ryuaaa/codex-monitor-hud";
static NSString *const HUDMacReleaseTagPrefix = @"v";
static NSString *const HUDReleaseAssetName = @"Codex-Monitor-HUD.app.zip";
static NSString *const HUDBundleIdentifier = @"com.codexmonitorhud.app";
static NSString *const HUDExpectedTeamIdentifier = @"L8K9749GM7";

@implementation HUDReleaseInfo
@end

static NSString *HUDNormalizedVersion(NSString *version) {
    NSString *trimmed = [version ?: @"" stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if ([[trimmed lowercaseString] hasPrefix:@"v"]) return [trimmed substringFromIndex:1];
    return trimmed;
}

NSComparisonResult HUDCompareVersions(NSString *left, NSString *right) {
    return [HUDNormalizedVersion(left) compare:HUDNormalizedVersion(right) options:NSNumericSearch];
}

NSString *HUDSHA256ForFile(NSURL *fileURL) {
    NSInputStream *stream = [NSInputStream inputStreamWithURL:fileURL];
    if (!stream) return nil;
    [stream open];
    CC_SHA256_CTX context;
    CC_SHA256_Init(&context);
    uint8_t buffer[64 * 1024];
    NSInteger count = 0;
    while ((count = [stream read:buffer maxLength:sizeof(buffer)]) > 0) CC_SHA256_Update(&context, buffer, (CC_LONG)count);
    NSError *streamError = stream.streamError;
    [stream close];
    if (count < 0 || streamError) return nil;
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(digest, &context);
    NSMutableString *result = [NSMutableString stringWithCapacity:CC_SHA256_DIGEST_LENGTH * 2];
    for (NSUInteger index = 0; index < CC_SHA256_DIGEST_LENGTH; index++) [result appendFormat:@"%02x", digest[index]];
    return result;
}

NSString *HUDInstallHelperScript(void) {
    return @"#!/bin/zsh\n"
            "set -eu\n"
            "pid=\"$1\"\n"
            "source_app=\"$2\"\n"
            "target_app=\"$3\"\n"
            "work_dir=\"$4\"\n"
            "probe=\"$work_dir/health-probe.app/Contents/MacOS/CodexMonitorHUD\"\n"
            "[[ \"$source_app\" == /*.app && \"$target_app\" == /*.app && \"$source_app\" != \"$target_app\" && \"$work_dir\" == /* && \"$work_dir\" != / && -x \"$probe\" ]] || exit 2\n"
            "backup_app=\"${target_app}.update-backup\"\n"
            "[[ ! -e \"$backup_app\" ]] || exit 2\n"
            "label=\"com.codexmonitorhud.app\"\n"
            "uid_value=\"$(/usr/bin/id -u)\"\n"
            "launch_agent=\"$HOME/Library/LaunchAgents/$label.plist\"\n"
            "managed=0\n"
            "if [[ -f \"$launch_agent\" ]]; then\n"
            "  agent_executable=\"$(/usr/libexec/PlistBuddy -c 'Print :ProgramArguments:0' \"$launch_agent\" 2>/dev/null)\" || agent_executable=\"\"\n"
            "  expected_executable=\"$target_app/Contents/MacOS/CodexMonitorHUD\"\n"
            "  if [[ -n \"$agent_executable\" && \"${agent_executable:A}\" == \"${expected_executable:A}\" ]] && /bin/launchctl print \"gui/$uid_value/$label\" >/dev/null 2>&1; then managed=1; fi\n"
            "fi\n"
            "restart_app() {\n"
            "  if [[ $managed == 1 ]]; then\n"
            "    if /bin/launchctl kickstart -k \"gui/$uid_value/$label\" 2>/dev/null; then return 0; fi\n"
            "    if /bin/launchctl bootstrap \"gui/$uid_value\" \"$launch_agent\" 2>/dev/null; then return 0; fi\n"
            "  fi\n"
            "  /usr/bin/open -g \"$target_app\"\n"
            "}\n"
            "attempt=0\n"
            "while /bin/kill -0 \"$pid\" 2>/dev/null; do\n"
            "  (( attempt += 1 )); [[ $attempt -le 150 ]] || exit 3\n"
            "  /bin/sleep 0.2\n"
            "done\n"
            "rollback() {\n"
            "  if [[ $managed == 1 ]] && /bin/launchctl print \"gui/$uid_value/$label\" >/dev/null 2>&1; then\n"
            "    /bin/launchctl bootout \"gui/$uid_value/$label\" 2>/dev/null || return 1\n"
            "  fi\n"
            "  \"$probe\" --update-stop-probe \"$target_app\" || return 1\n"
            "  /bin/rm -rf \"$target_app\"\n"
            "  /bin/mv \"$backup_app\" \"$target_app\" || return 1\n"
            "  restart_app\n"
            "}\n"
            "/bin/mv \"$target_app\" \"$backup_app\"\n"
            "trap 'if [[ -d \"$backup_app\" ]]; then rollback || true; fi' EXIT\n"
            "if /usr/bin/ditto \"$source_app\" \"$target_app\" && /usr/bin/codesign --verify --deep --strict \"$target_app\"; then\n"
            "  identity=\"$(/usr/bin/codesign -dv --verbose=4 \"$target_app\" 2>&1)\"\n"
            "  if [[ \"$identity\" != *\"TeamIdentifier=L8K9749GM7\"* ]] || ! /usr/sbin/spctl --assess --type execute \"$target_app\"; then\n"
            "    exit 1\n"
            "  fi\n"
            "  restart_app || exit 1\n"
            "  stable=0\n"
            "  last_pid=\"\"\n"
            "  for attempt in {1..30}; do\n"
            "    if running_pid=\"$(\"$probe\" --update-health-probe \"$target_app\")\" && [[ \"$running_pid\" == <-> && \"$running_pid\" != 0 ]]; then\n"
            "      if [[ \"$running_pid\" == \"$last_pid\" ]]; then (( stable += 1 )); else stable=1; fi\n"
            "      last_pid=\"$running_pid\"\n"
            "    else stable=0; last_pid=\"\"; fi\n"
            "    [[ $stable -lt 5 ]] || break\n"
            "    /bin/sleep 1\n"
            "  done\n"
            "  [[ $stable -ge 5 ]] || exit 1\n"
            "  trap - EXIT\n"
            "  /bin/rm -rf \"$backup_app\"\n"
            "  /bin/rm -rf \"$work_dir\"\n"
            "else\n"
            "  exit 1\n"
            "fi\n";
}

HUDReleaseInfo *HUDReleaseInfoFromDictionary(NSDictionary *dictionary, NSError **error) {
    NSString *tag = [dictionary[@"tag_name"] isKindOfClass:NSString.class] ? dictionary[@"tag_name"] : @"";
    if (![tag hasPrefix:HUDMacReleaseTagPrefix]) {
        if (error) *error = [NSError errorWithDomain:@"HUDUpdater" code:1 userInfo:@{NSLocalizedDescriptionKey: HUDL(@"不是macOS更新通道")}];
        return nil;
    }
    NSString *version = HUDNormalizedVersion(tag);
    NSString *page = [dictionary[@"html_url"] isKindOfClass:NSString.class] ? dictionary[@"html_url"] : @"";
    NSArray *assets = [dictionary[@"assets"] isKindOfClass:NSArray.class] ? dictionary[@"assets"] : @[];
    NSDictionary *selected = nil;
    for (id item in assets) {
        if ([item isKindOfClass:NSDictionary.class] && [item[@"name"] isEqualToString:HUDReleaseAssetName]) { selected = item; break; }
    }
    NSString *assetURL = [selected[@"browser_download_url"] isKindOfClass:NSString.class] ? selected[@"browser_download_url"] : @"";
    NSString *digest = [selected[@"digest"] isKindOfClass:NSString.class] ? selected[@"digest"] : @"";
    if ([digest.lowercaseString hasPrefix:@"sha256:"]) digest = [digest substringFromIndex:7];
    NSCharacterSet *invalidDigestCharacters = [[NSCharacterSet characterSetWithCharactersInString:@"0123456789abcdefABCDEF"] invertedSet];
    NSCharacterSet *invalidVersionCharacters = [[NSCharacterSet characterSetWithCharactersInString:@"0123456789."] invertedSet];
    NSURL *pageURL = [NSURL URLWithString:page];
    NSURL *downloadURL = [NSURL URLWithString:assetURL];
    BOOL validDigest = digest.length == 64 && [digest rangeOfCharacterFromSet:invalidDigestCharacters].location == NSNotFound;
    BOOL validVersion = version.length > 0 && [version rangeOfCharacterFromSet:invalidVersionCharacters].location == NSNotFound;
    BOOL validURLs = [pageURL.scheme isEqualToString:@"https"] && [pageURL.host isEqualToString:@"github.com"] && [downloadURL.scheme isEqualToString:@"https"] && [downloadURL.host isEqualToString:@"github.com"];
    if (!validVersion || !validURLs || !validDigest) {
        if (error) *error = [NSError errorWithDomain:@"HUDUpdater" code:1 userInfo:@{NSLocalizedDescriptionKey: HUDL(@"最新版缺少应用更新包或安全摘要")}];
        return nil;
    }
    HUDReleaseInfo *release = [HUDReleaseInfo new];
    release.tagName = tag;
    release.version = version;
    release.releaseNotes = [dictionary[@"body"] isKindOfClass:NSString.class] ? dictionary[@"body"] : @"";
    release.assetURL = downloadURL;
    release.assetDigest = digest.lowercaseString;
    release.releasePageURL = pageURL;
    return release;
}

HUDReleaseInfo *HUDLatestMacReleaseInfoFromArray(NSArray *releases, NSError **error) {
    if (![releases isKindOfClass:NSArray.class]) {
        if (error) *error = [NSError errorWithDomain:@"HUDUpdater" code:1 userInfo:@{NSLocalizedDescriptionKey: HUDL(@"更新列表格式错误")}];
        return nil;
    }
    HUDReleaseInfo *best = nil;
    for (id item in releases) {
        if (![item isKindOfClass:NSDictionary.class] || [item[@"draft"] boolValue] || [item[@"prerelease"] boolValue]) continue;
        HUDReleaseInfo *candidate = HUDReleaseInfoFromDictionary(item, nil);
        if (candidate && (!best || HUDCompareVersions(candidate.version, best.version) == NSOrderedDescending)) best = candidate;
    }
    if (!best && error) *error = [NSError errorWithDomain:@"HUDUpdater" code:1 userInfo:@{NSLocalizedDescriptionKey: HUDL(@"暂未找到macOS更新版本")}];
    return best;
}

static BOOL HUDRunTask(NSString *path, NSArray<NSString *> *arguments, NSString **errorText) {
    NSTask *task = [NSTask new];
    task.executableURL = [NSURL fileURLWithPath:path];
    task.arguments = arguments;
    NSPipe *pipe = [NSPipe pipe];
    task.standardError = pipe;
    NSError *launchError = nil;
    if (![task launchAndReturnError:&launchError]) {
        if (errorText) *errorText = launchError.localizedDescription;
        return NO;
    }
    [task waitUntilExit];
    if (task.terminationStatus == 0) return YES;
    NSData *data = [pipe.fileHandleForReading readDataToEndOfFile];
    NSString *details = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
    if (errorText) *errorText = details.length ? details : [NSString stringWithFormat:HUDL(@"命令退出码 %d"), task.terminationStatus];
    return NO;
}

BOOL HUDPrepareInstallProbe(NSURL *trustedBundleURL, NSURL *workURL, NSError **error) {
    // Developer ID executables retain a signed Info.plist binding. Copy the entire
    // trusted bundle so the temporary probe remains valid outside its original path.
    NSURL *probeBundleURL = [workURL URLByAppendingPathComponent:@"health-probe.app" isDirectory:YES];
    if (![NSFileManager.defaultManager copyItemAtURL:trustedBundleURL toURL:probeBundleURL error:error]) return NO;
    NSString *details = nil;
    if (!HUDRunTask(@"/usr/bin/codesign", @[@"--verify", @"--deep", @"--strict", probeBundleURL.path], &details)) {
        [NSFileManager.defaultManager removeItemAtURL:probeBundleURL error:nil];
        if (error) *error = [NSError errorWithDomain:@"HUDUpdater" code:2 userInfo:@{NSLocalizedDescriptionKey: HUDL(@"升级恢复检查程序的签名无效，未替换当前版本")}];
        return NO;
    }
    return YES;
}

@interface HUDUpdateManager ()
@property(nonatomic, strong, readwrite) HUDReleaseInfo *latestRelease;
@property(nonatomic, copy, readwrite) NSString *currentVersion;
@property(nonatomic, strong) NSURLSession *session;
@end

@implementation HUDUpdateManager

- (instancetype)init {
    self = [super init];
    if (!self) return nil;
    _currentVersion = [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"] ?: @"0";
    NSURLSessionConfiguration *configuration = NSURLSessionConfiguration.ephemeralSessionConfiguration;
    configuration.timeoutIntervalForRequest = 15.0;
    configuration.timeoutIntervalForResource = 180.0;
    _session = [NSURLSession sessionWithConfiguration:configuration];
    return self;
}

- (void)finishOnMain:(void (^)(HUDUpdateCheckResult, HUDReleaseInfo *, NSString *))completion result:(HUDUpdateCheckResult)result release:(HUDReleaseInfo *)release message:(NSString *)message {
    dispatch_async(dispatch_get_main_queue(), ^{ if (completion) completion(result, release, message); });
}

- (void)checkForUpdates:(void (^)(HUDUpdateCheckResult result, HUDReleaseInfo *release, NSString *message))completion {
    NSURL *url = [NSURL URLWithString:[NSString stringWithFormat:@"https://api.github.com/repos/%@/releases?per_page=100", HUDRepository]];
    NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:url];
    [request setValue:@"application/vnd.github+json" forHTTPHeaderField:@"Accept"];
    [request setValue:@"Codex-Monitor-HUD-Updater" forHTTPHeaderField:@"User-Agent"];
    [request setValue:@"2022-11-28" forHTTPHeaderField:@"X-GitHub-Api-Version"];
    __weak typeof(self) weakSelf = self;
    [[self.session dataTaskWithRequest:request completionHandler:^(NSData *data, NSURLResponse *response, NSError *networkError) {
        NSHTTPURLResponse *http = (NSHTTPURLResponse *)response;
        if (networkError || ![http isKindOfClass:NSHTTPURLResponse.class] || http.statusCode != 200) {
            NSString *message = http.statusCode == 404 ? HUDL(@"仓库还没有发布可更新版本") : (networkError.localizedDescription ?: HUDL(@"GitHub更新检查失败"));
            [weakSelf finishOnMain:completion result:HUDUpdateCheckResultFailed release:nil message:message];
            return;
        }
        NSError *jsonError = nil;
        id object = [NSJSONSerialization JSONObjectWithData:data options:0 error:&jsonError];
        if (![object isKindOfClass:NSArray.class]) {
            [weakSelf finishOnMain:completion result:HUDUpdateCheckResultFailed release:nil message:jsonError.localizedDescription ?: HUDL(@"更新信息格式错误")];
            return;
        }
        NSError *parseError = nil;
        HUDReleaseInfo *release = HUDLatestMacReleaseInfoFromArray(object, &parseError);
        if (!release) {
            [weakSelf finishOnMain:completion result:HUDUpdateCheckResultFailed release:nil message:parseError.localizedDescription];
            return;
        }
        weakSelf.latestRelease = release;
        BOOL available = HUDCompareVersions(release.version, weakSelf.currentVersion) == NSOrderedDescending;
        NSString *message = available ? [NSString stringWithFormat:HUDL(@"发现新版 %@"), release.version] : [NSString stringWithFormat:HUDL(@"当前 %@ 已是最新版"), weakSelf.currentVersion];
        [weakSelf finishOnMain:completion result:(available ? HUDUpdateCheckResultAvailable : HUDUpdateCheckResultUpToDate) release:release message:message];
    }] resume];
}

- (void)installRelease:(HUDReleaseInfo *)release completion:(void (^)(BOOL prepared, NSString *message))completion {
    if (!release.assetURL || release.assetDigest.length != 64) {
        dispatch_async(dispatch_get_main_queue(), ^{ if (completion) completion(NO, HUDL(@"更新包信息不完整")); });
        return;
    }
    NSURL *bundleURL = NSBundle.mainBundle.bundleURL;
    NSString *parent = bundleURL.URLByDeletingLastPathComponent.path;
    if (![bundleURL.pathExtension.lowercaseString isEqualToString:@"app"] || ![NSFileManager.defaultManager isWritableFileAtPath:parent]) {
        dispatch_async(dispatch_get_main_queue(), ^{ if (completion) completion(NO, HUDL(@"当前安装位置不可写，请从发布页面手动更新")); });
        return;
    }
    __weak typeof(self) weakSelf = self;
    [[self.session downloadTaskWithURL:release.assetURL completionHandler:^(NSURL *location, NSURLResponse *response, NSError *downloadError) {
        (void)response;
        if (downloadError || !location) {
            [weakSelf finishInstall:completion success:NO message:downloadError.localizedDescription ?: HUDL(@"更新包下载失败")];
            return;
        }
        NSString *workName = [@"codex-monitor-update-" stringByAppendingString:NSUUID.UUID.UUIDString];
        NSURL *workURL = [NSURL fileURLWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:workName] isDirectory:YES];
        NSURL *archiveURL = [workURL URLByAppendingPathComponent:HUDReleaseAssetName];
        NSError *fileError = nil;
        if (![NSFileManager.defaultManager createDirectoryAtURL:workURL withIntermediateDirectories:YES attributes:nil error:&fileError] || ![NSFileManager.defaultManager copyItemAtURL:location toURL:archiveURL error:&fileError]) {
            [weakSelf finishInstall:completion success:NO message:fileError.localizedDescription ?: HUDL(@"无法保存更新包")];
            return;
        }
        NSString *actualDigest = HUDSHA256ForFile(archiveURL);
        if (![actualDigest isEqualToString:release.assetDigest.lowercaseString]) {
            [NSFileManager.defaultManager removeItemAtURL:workURL error:nil];
            [weakSelf finishInstall:completion success:NO message:HUDL(@"更新包安全摘要不匹配，已停止更新")];
            return;
        }
        NSURL *extractURL = [workURL URLByAppendingPathComponent:@"extracted" isDirectory:YES];
        [NSFileManager.defaultManager createDirectoryAtURL:extractURL withIntermediateDirectories:YES attributes:nil error:nil];
        NSString *taskError = nil;
        if (!HUDRunTask(@"/usr/bin/ditto", @[@"-x", @"-k", archiveURL.path, extractURL.path], &taskError)) {
            [NSFileManager.defaultManager removeItemAtURL:workURL error:nil];
            [weakSelf finishInstall:completion success:NO message:[NSString stringWithFormat:HUDL(@"更新包无法解压：%@"), taskError ?: HUDL(@"未知错误")]];
            return;
        }
        NSURL *newBundle = [extractURL URLByAppendingPathComponent:@"Codex Monitor HUD.app" isDirectory:YES];
        NSDictionary *info = [NSDictionary dictionaryWithContentsOfURL:[newBundle URLByAppendingPathComponent:@"Contents/Info.plist"]];
        NSString *identifier = [info[@"CFBundleIdentifier"] isKindOfClass:NSString.class] ? info[@"CFBundleIdentifier"] : nil;
        NSString *version = [info[@"CFBundleShortVersionString"] isKindOfClass:NSString.class] ? info[@"CFBundleShortVersionString"] : nil;
        if (![identifier isEqualToString:HUDBundleIdentifier] || ![version isEqualToString:release.version]) {
            [NSFileManager.defaultManager removeItemAtURL:workURL error:nil];
            [weakSelf finishInstall:completion success:NO message:HUDL(@"更新包的应用身份或版本不正确")];
            return;
        }
        NSString *minimumOS = [info[@"LSMinimumSystemVersion"] isKindOfClass:NSString.class] ? info[@"LSMinimumSystemVersion"] : nil;
        NSOperatingSystemVersion os = NSProcessInfo.processInfo.operatingSystemVersion;
        NSString *currentOS = [NSString stringWithFormat:@"%ld.%ld.%ld", (long)os.majorVersion, (long)os.minorVersion, (long)os.patchVersion];
        if (minimumOS && HUDCompareVersions(minimumOS, currentOS) == NSOrderedDescending) {
            [NSFileManager.defaultManager removeItemAtURL:workURL error:nil];
            [weakSelf finishInstall:completion success:NO message:HUDL(@"新版要求更高的macOS版本，已保留当前版本")];
            return;
        }
        if (!HUDRunTask(@"/usr/bin/codesign", @[@"--verify", @"--deep", @"--strict", newBundle.path], &taskError)) {
            [NSFileManager.defaultManager removeItemAtURL:workURL error:nil];
            [weakSelf finishInstall:completion success:NO message:[NSString stringWithFormat:HUDL(@"更新包签名校验失败：%@"), taskError ?: HUDL(@"未知错误")]];
            return;
        }
        NSTask *identityTask = [NSTask new];
        identityTask.executableURL = [NSURL fileURLWithPath:@"/usr/bin/codesign"];
        identityTask.arguments = @[@"-dv", @"--verbose=4", newBundle.path];
        NSPipe *identityPipe = [NSPipe pipe];
        identityTask.standardOutput = identityPipe;
        identityTask.standardError = identityPipe;
        NSError *identityError = nil;
        BOOL identityStarted = [identityTask launchAndReturnError:&identityError];
        if (identityStarted) [identityTask waitUntilExit];
        NSData *identityData = identityStarted ? [identityPipe.fileHandleForReading readDataToEndOfFile] : nil;
        NSString *identityText = identityData ? [[NSString alloc] initWithData:identityData encoding:NSUTF8StringEncoding] : @"";
        NSString *teamNeedle = [NSString stringWithFormat:@"TeamIdentifier=%@", HUDExpectedTeamIdentifier];
        if (!identityStarted || identityTask.terminationStatus != 0 || ![identityText containsString:teamNeedle]) {
            [NSFileManager.defaultManager removeItemAtURL:workURL error:nil];
            [weakSelf finishInstall:completion success:NO message:HUDL(@"更新包不是本项目的官方签名，已停止更新")];
            return;
        }
        if (!HUDRunTask(@"/usr/sbin/spctl", @[@"--assess", @"--type", @"execute", newBundle.path], &taskError)) {
            [NSFileManager.defaultManager removeItemAtURL:workURL error:nil];
            [weakSelf finishInstall:completion success:NO message:[NSString stringWithFormat:HUDL(@"更新包未通过Apple安全验证：%@"), taskError ?: HUDL(@"未知错误")]];
            return;
        }
        NSURL *helperURL = [workURL URLByAppendingPathComponent:@"install-update.zsh"];
        if (!HUDPrepareInstallProbe(NSBundle.mainBundle.bundleURL, workURL, &fileError)) {
            [NSFileManager.defaultManager removeItemAtURL:workURL error:nil];
            [weakSelf finishInstall:completion success:NO message:HUDL(@"无法准备升级恢复检查，未替换当前版本")];
            return;
        }
        NSString *script = HUDInstallHelperScript();
        if (![script writeToURL:helperURL atomically:YES encoding:NSUTF8StringEncoding error:&fileError] || chmod(helperURL.fileSystemRepresentation, 0700) != 0) {
            [NSFileManager.defaultManager removeItemAtURL:workURL error:nil];
            [weakSelf finishInstall:completion success:NO message:fileError.localizedDescription ?: HUDL(@"无法准备更新安装程序")];
            return;
        }
        NSTask *helper = [NSTask new];
        helper.executableURL = helperURL;
        helper.arguments = @[[NSString stringWithFormat:@"%d", NSProcessInfo.processInfo.processIdentifier], newBundle.path, bundleURL.path, workURL.path];
        NSError *launchError = nil;
        if (![helper launchAndReturnError:&launchError]) {
            [NSFileManager.defaultManager removeItemAtURL:workURL error:nil];
            [weakSelf finishInstall:completion success:NO message:launchError.localizedDescription ?: HUDL(@"无法启动更新安装程序")];
            return;
        }
        [weakSelf finishInstall:completion success:YES message:HUDL(@"更新已验证，正在重启应用")];
    }] resume];
}

- (void)finishInstall:(void (^)(BOOL, NSString *))completion success:(BOOL)success message:(NSString *)message {
    dispatch_async(dispatch_get_main_queue(), ^{ if (completion) completion(success, message); });
}

@end
