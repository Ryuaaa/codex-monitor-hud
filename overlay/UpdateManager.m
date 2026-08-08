#import "UpdateManager.h"

#import <CommonCrypto/CommonDigest.h>
#import <sys/stat.h>

static NSString *const HUDRepository = @"Ryuaaa/codex-monitor-hud";
static NSString *const HUDReleaseAssetName = @"Codex-Monitor-HUD.app.zip";
static NSString *const HUDBundleIdentifier = @"com.codexmonitorhud.app";

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
            "restart_app() {\n"
            "  label=\"com.codexmonitorhud.app\"\n"
            "  uid_value=\"$(/usr/bin/id -u)\"\n"
            "  launch_agent=\"$HOME/Library/LaunchAgents/$label.plist\"\n"
            "  if [[ -f \"$launch_agent\" ]]; then\n"
            "    if /bin/launchctl kickstart \"gui/$uid_value/$label\" 2>/dev/null; then return 0; fi\n"
            "    if /bin/launchctl bootstrap \"gui/$uid_value\" \"$launch_agent\" 2>/dev/null; then return 0; fi\n"
            "  fi\n"
            "  /usr/bin/open \"$target_app\"\n"
            "}\n"
            "while /bin/kill -0 \"$pid\" 2>/dev/null; do /bin/sleep 0.2; done\n"
            "backup_app=\"${target_app}.update-backup\"\n"
            "/bin/rm -rf \"$backup_app\"\n"
            "/bin/mv \"$target_app\" \"$backup_app\"\n"
            "if /usr/bin/ditto \"$source_app\" \"$target_app\" && /usr/bin/codesign --verify --deep --strict \"$target_app\"; then\n"
            "  /bin/rm -rf \"$backup_app\"\n"
            "  restart_app\n"
            "  /bin/rm -rf \"$work_dir\"\n"
            "else\n"
            "  /bin/rm -rf \"$target_app\"\n"
            "  /bin/mv \"$backup_app\" \"$target_app\"\n"
            "  restart_app\n"
            "  exit 1\n"
            "fi\n";
}

HUDReleaseInfo *HUDReleaseInfoFromDictionary(NSDictionary *dictionary, NSError **error) {
    NSString *tag = [dictionary[@"tag_name"] isKindOfClass:NSString.class] ? dictionary[@"tag_name"] : @"";
    NSString *page = [dictionary[@"html_url"] isKindOfClass:NSString.class] ? dictionary[@"html_url"] : @"";
    NSArray *assets = [dictionary[@"assets"] isKindOfClass:NSArray.class] ? dictionary[@"assets"] : @[];
    NSDictionary *selected = nil;
    for (id item in assets) {
        if ([item isKindOfClass:NSDictionary.class] && [item[@"name"] isEqualToString:HUDReleaseAssetName]) { selected = item; break; }
    }
    NSString *assetURL = [selected[@"browser_download_url"] isKindOfClass:NSString.class] ? selected[@"browser_download_url"] : @"";
    NSString *digest = [selected[@"digest"] isKindOfClass:NSString.class] ? selected[@"digest"] : @"";
    if ([digest.lowercaseString hasPrefix:@"sha256:"]) digest = [digest substringFromIndex:7];
    NSString *version = HUDNormalizedVersion(tag);
    NSCharacterSet *invalidDigestCharacters = [[NSCharacterSet characterSetWithCharactersInString:@"0123456789abcdefABCDEF"] invertedSet];
    NSCharacterSet *invalidVersionCharacters = [[NSCharacterSet characterSetWithCharactersInString:@"0123456789."] invertedSet];
    NSURL *pageURL = [NSURL URLWithString:page];
    NSURL *downloadURL = [NSURL URLWithString:assetURL];
    BOOL validDigest = digest.length == 64 && [digest rangeOfCharacterFromSet:invalidDigestCharacters].location == NSNotFound;
    BOOL validVersion = version.length > 0 && [version rangeOfCharacterFromSet:invalidVersionCharacters].location == NSNotFound;
    BOOL validURLs = [pageURL.scheme isEqualToString:@"https"] && [pageURL.host isEqualToString:@"github.com"] && [downloadURL.scheme isEqualToString:@"https"] && [downloadURL.host isEqualToString:@"github.com"];
    if (!validVersion || !validURLs || !validDigest) {
        if (error) *error = [NSError errorWithDomain:@"HUDUpdater" code:1 userInfo:@{NSLocalizedDescriptionKey: @"最新版缺少应用更新包或安全摘要"}];
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
    if (errorText) *errorText = details.length ? details : [NSString stringWithFormat:@"命令退出码 %d", task.terminationStatus];
    return NO;
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
    NSURL *url = [NSURL URLWithString:[NSString stringWithFormat:@"https://api.github.com/repos/%@/releases/latest", HUDRepository]];
    NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:url];
    [request setValue:@"application/vnd.github+json" forHTTPHeaderField:@"Accept"];
    [request setValue:@"Codex-Monitor-HUD-Updater" forHTTPHeaderField:@"User-Agent"];
    [request setValue:@"2022-11-28" forHTTPHeaderField:@"X-GitHub-Api-Version"];
    __weak typeof(self) weakSelf = self;
    [[self.session dataTaskWithRequest:request completionHandler:^(NSData *data, NSURLResponse *response, NSError *networkError) {
        NSHTTPURLResponse *http = (NSHTTPURLResponse *)response;
        if (networkError || ![http isKindOfClass:NSHTTPURLResponse.class] || http.statusCode != 200) {
            NSString *message = http.statusCode == 404 ? @"仓库还没有发布可更新版本" : (networkError.localizedDescription ?: @"GitHub更新检查失败");
            [weakSelf finishOnMain:completion result:HUDUpdateCheckResultFailed release:nil message:message];
            return;
        }
        NSError *jsonError = nil;
        id object = [NSJSONSerialization JSONObjectWithData:data options:0 error:&jsonError];
        if (![object isKindOfClass:NSDictionary.class]) {
            [weakSelf finishOnMain:completion result:HUDUpdateCheckResultFailed release:nil message:jsonError.localizedDescription ?: @"更新信息格式错误"];
            return;
        }
        NSError *parseError = nil;
        HUDReleaseInfo *release = HUDReleaseInfoFromDictionary(object, &parseError);
        if (!release) {
            [weakSelf finishOnMain:completion result:HUDUpdateCheckResultFailed release:nil message:parseError.localizedDescription];
            return;
        }
        weakSelf.latestRelease = release;
        BOOL available = HUDCompareVersions(release.version, weakSelf.currentVersion) == NSOrderedDescending;
        NSString *message = available ? [NSString stringWithFormat:@"发现新版 %@", release.version] : [NSString stringWithFormat:@"当前 %@ 已是最新版", weakSelf.currentVersion];
        [weakSelf finishOnMain:completion result:(available ? HUDUpdateCheckResultAvailable : HUDUpdateCheckResultUpToDate) release:release message:message];
    }] resume];
}

- (void)installRelease:(HUDReleaseInfo *)release completion:(void (^)(BOOL prepared, NSString *message))completion {
    if (!release.assetURL || release.assetDigest.length != 64) {
        dispatch_async(dispatch_get_main_queue(), ^{ if (completion) completion(NO, @"更新包信息不完整"); });
        return;
    }
    NSURL *bundleURL = NSBundle.mainBundle.bundleURL;
    NSString *parent = bundleURL.URLByDeletingLastPathComponent.path;
    if (![bundleURL.pathExtension.lowercaseString isEqualToString:@"app"] || ![NSFileManager.defaultManager isWritableFileAtPath:parent]) {
        dispatch_async(dispatch_get_main_queue(), ^{ if (completion) completion(NO, @"当前安装位置不可写，请从发布页面手动更新"); });
        return;
    }
    __weak typeof(self) weakSelf = self;
    [[self.session downloadTaskWithURL:release.assetURL completionHandler:^(NSURL *location, NSURLResponse *response, NSError *downloadError) {
        (void)response;
        if (downloadError || !location) {
            [weakSelf finishInstall:completion success:NO message:downloadError.localizedDescription ?: @"更新包下载失败"];
            return;
        }
        NSString *workName = [@"codex-monitor-update-" stringByAppendingString:NSUUID.UUID.UUIDString];
        NSURL *workURL = [NSURL fileURLWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:workName] isDirectory:YES];
        NSURL *archiveURL = [workURL URLByAppendingPathComponent:HUDReleaseAssetName];
        NSError *fileError = nil;
        if (![NSFileManager.defaultManager createDirectoryAtURL:workURL withIntermediateDirectories:YES attributes:nil error:&fileError] || ![NSFileManager.defaultManager copyItemAtURL:location toURL:archiveURL error:&fileError]) {
            [weakSelf finishInstall:completion success:NO message:fileError.localizedDescription ?: @"无法保存更新包"];
            return;
        }
        NSString *actualDigest = HUDSHA256ForFile(archiveURL);
        if (![actualDigest isEqualToString:release.assetDigest.lowercaseString]) {
            [NSFileManager.defaultManager removeItemAtURL:workURL error:nil];
            [weakSelf finishInstall:completion success:NO message:@"更新包安全摘要不匹配，已停止更新"];
            return;
        }
        NSURL *extractURL = [workURL URLByAppendingPathComponent:@"extracted" isDirectory:YES];
        [NSFileManager.defaultManager createDirectoryAtURL:extractURL withIntermediateDirectories:YES attributes:nil error:nil];
        NSString *taskError = nil;
        if (!HUDRunTask(@"/usr/bin/ditto", @[@"-x", @"-k", archiveURL.path, extractURL.path], &taskError)) {
            [NSFileManager.defaultManager removeItemAtURL:workURL error:nil];
            [weakSelf finishInstall:completion success:NO message:[NSString stringWithFormat:@"更新包无法解压：%@", taskError ?: @"未知错误"]];
            return;
        }
        NSURL *newBundle = [extractURL URLByAppendingPathComponent:@"Codex Monitor HUD.app" isDirectory:YES];
        NSDictionary *info = [NSDictionary dictionaryWithContentsOfURL:[newBundle URLByAppendingPathComponent:@"Contents/Info.plist"]];
        NSString *identifier = info[@"CFBundleIdentifier"];
        NSString *version = info[@"CFBundleShortVersionString"];
        if (![identifier isEqualToString:HUDBundleIdentifier] || ![version isEqualToString:release.version]) {
            [NSFileManager.defaultManager removeItemAtURL:workURL error:nil];
            [weakSelf finishInstall:completion success:NO message:@"更新包的应用身份或版本不正确"];
            return;
        }
        if (!HUDRunTask(@"/usr/bin/codesign", @[@"--verify", @"--deep", @"--strict", newBundle.path], &taskError)) {
            [NSFileManager.defaultManager removeItemAtURL:workURL error:nil];
            [weakSelf finishInstall:completion success:NO message:[NSString stringWithFormat:@"更新包签名校验失败：%@", taskError ?: @"未知错误"]];
            return;
        }
        if (!HUDRunTask(@"/usr/sbin/spctl", @[@"--assess", @"--type", @"execute", newBundle.path], &taskError)) {
            [NSFileManager.defaultManager removeItemAtURL:workURL error:nil];
            [weakSelf finishInstall:completion success:NO message:[NSString stringWithFormat:@"更新包未通过Apple安全验证：%@", taskError ?: @"未知错误"]];
            return;
        }
        NSURL *helperURL = [workURL URLByAppendingPathComponent:@"install-update.zsh"];
        NSString *script = HUDInstallHelperScript();
        if (![script writeToURL:helperURL atomically:YES encoding:NSUTF8StringEncoding error:&fileError] || chmod(helperURL.fileSystemRepresentation, 0700) != 0) {
            [NSFileManager.defaultManager removeItemAtURL:workURL error:nil];
            [weakSelf finishInstall:completion success:NO message:fileError.localizedDescription ?: @"无法准备更新安装程序"];
            return;
        }
        NSTask *helper = [NSTask new];
        helper.executableURL = helperURL;
        helper.arguments = @[[NSString stringWithFormat:@"%d", NSProcessInfo.processInfo.processIdentifier], newBundle.path, bundleURL.path, workURL.path];
        NSError *launchError = nil;
        if (![helper launchAndReturnError:&launchError]) {
            [NSFileManager.defaultManager removeItemAtURL:workURL error:nil];
            [weakSelf finishInstall:completion success:NO message:launchError.localizedDescription ?: @"无法启动更新安装程序"];
            return;
        }
        [weakSelf finishInstall:completion success:YES message:@"更新已验证，正在重启应用"];
    }] resume];
}

- (void)finishInstall:(void (^)(BOOL, NSString *))completion success:(BOOL)success message:(NSString *)message {
    dispatch_async(dispatch_get_main_queue(), ^{ if (completion) completion(success, message); });
}

@end
