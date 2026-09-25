#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

static NSString *const SyncDefaultsDomain = @"com.codexmonitorhud.subscription-data";
static NSURL *BillingURL(void) { return [NSURL URLWithString:@"https://chatgpt.com/?no_universal_links=1#settings/Billing"]; }

static BOOL IsBillingPage(NSURL *url) {
    if (![url.host isEqual:@"chatgpt.com"]) return NO;
    return [url.path hasPrefix:@"/settings/billing"] ||
           [url.fragment.lowercaseString isEqual:@"settings/billing"];
}

static BOOL LoginHostAllowed(NSString *host) {
    if (!host.length) return NO;
    return [host isEqual:@"chatgpt.com"] || [host isEqual:@"openai.com"] || [host hasSuffix:@".openai.com"] ||
           [host isEqual:@"accounts.google.com"] || [host isEqual:@"appleid.apple.com"] ||
           [host isEqual:@"challenges.cloudflare.com"];
}

static NSDictionary<NSString *, NSString *> *NormalizeResult(id result) {
    if (![result isKindOfClass:NSDictionary.class]) return nil;
    NSString *date = result[@"date"], *state = result[@"renewal"];
    if (![date isKindOfClass:NSString.class] || ![state isKindOfClass:NSString.class] ||
        ![@[@"renewing", @"cancelled", @"unknown"] containsObject:state]) return nil;
    NSDateFormatter *formatter = [NSDateFormatter new];
    formatter.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
    formatter.calendar = [[NSCalendar alloc] initWithCalendarIdentifier:NSCalendarIdentifierGregorian];
    formatter.timeZone = [NSTimeZone timeZoneForSecondsFromGMT:0];
    formatter.dateFormat = @"yyyy-MM-dd";
    formatter.lenient = NO;
    NSDate *parsed = [formatter dateFromString:date];
    return parsed && [[formatter stringFromDate:parsed] isEqual:date] ? @{ @"date":date, @"renewal":state } : nil;
}

@interface SubscriptionSync : NSObject <NSApplicationDelegate, WKNavigationDelegate, WKUIDelegate>
@property(nonatomic, strong) NSWindow *window;
@property(nonatomic, strong) WKWebView *web;
@property(nonatomic, strong) NSTextField *status;
@property(nonatomic, copy) NSString *reader;
@property(nonatomic, strong) NSUserDefaults *defaults;
@property(nonatomic) BOOL silent;
@property(nonatomic) BOOL fixture;
@property(nonatomic) BOOL redirectedAfterLogin;
@property(nonatomic) BOOL finished;
@property(nonatomic) NSUInteger generation;
@property(nonatomic) NSUInteger readAttempts;
@property(nonatomic) BOOL diagnostic;
@end

@implementation SubscriptionSync
- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    (void)notification;
    NSArray<NSString *> *arguments = NSProcessInfo.processInfo.arguments;
    self.silent = [arguments containsObject:@"--silent"];
    self.fixture = [arguments containsObject:@"--fixture"];
    self.diagnostic = [arguments containsObject:@"--diagnostic"];
    self.defaults = [[NSUserDefaults alloc] initWithSuiteName:self.fixture ? @"com.codexmonitorhud.subscription-data.fixture" : SyncDefaultsDomain];
    if ([arguments containsObject:@"--clear-session"]) {
        WKWebsiteDataStore *store = WKWebsiteDataStore.defaultDataStore;
        NSSet<NSString *> *types = WKWebsiteDataStore.allWebsiteDataTypes;
        [store fetchDataRecordsOfTypes:types completionHandler:^(NSArray<WKWebsiteDataRecord *> *records) {
            [store removeDataOfTypes:types forDataRecords:records completionHandler:^{
                [self.defaults setObject:@"login-required" forKey:@"subscriptionLastError"];
                [self.defaults synchronize];
                [NSApp terminate:nil];
            }];
        }];
        return;
    }
    self.reader = [NSString stringWithContentsOfFile:[NSBundle.mainBundle pathForResource:@"SubscriptionBillingReader" ofType:@"js"] encoding:NSUTF8StringEncoding error:nil];
    if (!self.reader.length) { [self finishWithError:@"reader-missing"]; return; }
    if (!self.fixture) [self.defaults setDouble:NSDate.date.timeIntervalSince1970 forKey:@"subscriptionLastAttemptAt"];
    self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1020, 760)
        styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskResizable
        backing:NSBackingStoreBuffered defer:NO];
    self.window.title = @"Codex Monitor HUD · 订阅日期同步";
    [self.window center];
    self.status = [NSTextField wrappingLabelWithString:@"正在打开 ChatGPT 官方账单页。首次使用需要在这里登录；不会读取其他浏览器的 Cookie。"];
    self.status.frame = NSMakeRect(16, 712, 988, 32);
    self.status.autoresizingMask = NSViewWidthSizable|NSViewMinYMargin;
    [self.window.contentView addSubview:self.status];
    NSButton *retry = [NSButton buttonWithTitle:@"重新打开账单" target:self action:@selector(retry:)];
    retry.frame = NSMakeRect(16, 674, 150, 30);
    retry.autoresizingMask = NSViewMinYMargin;
    [self.window.contentView addSubview:retry];
    NSButton *close = [NSButton buttonWithTitle:@"关闭" target:self action:@selector(close:)];
    close.frame = NSMakeRect(176, 674, 90, 30);
    close.autoresizingMask = NSViewMinYMargin;
    [self.window.contentView addSubview:close];
    NSView *host = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 1020, 664)];
    host.autoresizingMask = NSViewWidthSizable|NSViewHeightSizable;
    [self.window.contentView addSubview:host];
    WKWebViewConfiguration *configuration = [WKWebViewConfiguration new];
    configuration.websiteDataStore = self.fixture ? WKWebsiteDataStore.nonPersistentDataStore : WKWebsiteDataStore.defaultDataStore;
    self.web = [[WKWebView alloc] initWithFrame:host.bounds configuration:configuration];
    self.web.navigationDelegate = self;
    self.web.UIDelegate = self;
    self.web.autoresizingMask = NSViewWidthSizable|NSViewHeightSizable;
    [host addSubview:self.web];
    if (self.silent) {
        self.window.alphaValue = 0.01;
        self.window.ignoresMouseEvents = YES;
        [self.window orderBack:nil];
    } else [self.window orderFrontRegardless];
    [self loadBilling];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (self.silent ? 60 : 15*60)*NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        if (!self.finished) [self finishWithError:@"timeout"];
    });
}

- (void)loadBilling {
    self.generation++;
    self.readAttempts = 0;
    if (self.fixture) {
        NSArray<NSString *> *arguments = NSProcessInfo.processInfo.arguments;
        NSUInteger caseIndex = [arguments indexOfObject:@"--fixture-case"];
        NSString *name = caseIndex != NSNotFound && caseIndex + 1 < arguments.count ? arguments[caseIndex + 1] : @"zh";
        NSDictionary *texts = @{
            @"zh": @"你的套餐已取消，将不再续订。你可以继续使用服务，直至 2030年4月15日。",
            @"zh-Hant": @"方案已取消，將不再續訂。你可以繼續使用服務，直至 2030年4月15日。",
            @"en": @"Your plan has been canceled and will not renew. You can continue using the service until April 15, 2030.",
            @"ja": @"契約はキャンセルされ、更新されません。2030年4月15日まで利用できます。",
            @"ko": @"구독이 취소되어 자동 갱신되지 않습니다. 2030년 4월 15일까지 사용할 수 있습니다.",
            @"invoice": @"<table><tr><td>Billing date April 15, 2030</td></tr></table>",
            @"ambiguous": @"Your plans renew on April 15, 2030 and May 15, 2030.",
            @"invalid": @"Your plan renews automatically on February 30, 2026.",
            @"appstore": @"Your subscription is managed by Apple. Check your subscription in the App Store."
        };
        NSString *text = texts[name] ?: @"no fixture";
        NSString *html = [NSString stringWithFormat:@"<!doctype html><meta charset='utf-8'><style>body{font:16px sans-serif}</style><p>%@</p>",text];
        [self.web loadHTMLString:html baseURL:BillingURL()];
    } else [self.web loadRequest:[NSURLRequest requestWithURL:BillingURL()]];
}
- (void)retry:(id)sender { (void)sender; self.finished = NO; self.status.stringValue = @"正在重新打开官方账单页…"; [self loadBilling]; }
- (void)close:(id)sender { (void)sender; [NSApp terminate:nil]; }
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender { (void)sender; return YES; }
- (void)applicationWillTerminate:(NSNotification *)notification {
    (void)notification;
    [self.web stopLoading]; self.web.navigationDelegate = nil; self.web.UIDelegate = nil;
    [self.web removeFromSuperview]; self.web = nil;
}

- (void)finishWithError:(NSString *)reason {
    if (self.finished) return;
    self.finished = YES;
    if (self.fixture) { printf("fixture_error=%s\n", reason.UTF8String); fflush(stdout); }
    [self.defaults setObject:reason forKey:@"subscriptionLastError"];
    if (self.silent) { [NSApp terminate:nil]; return; }
    NSDictionary *messages = @{
        @"login-required": @"需要登录或验证身份。旧日期未改变；请在此窗口完成登录后重新打开账单。",
        @"timeout": @"本次同步超时。旧日期未改变。",
        @"load-failed": @"官方账单页加载失败。旧日期未改变。",
        @"reader-missing": @"日期读取组件缺失。旧日期未改变。",
        @"parse-failed": @"页面没有唯一且明确的订阅日期。旧日期未改变，请手动核对。"
    };
    self.status.stringValue = messages[reason] ?: @"同步失败，旧日期未改变。";
    if ([reason isEqual:@"timeout"]) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3*NSEC_PER_SEC), dispatch_get_main_queue(), ^{ [NSApp terminate:nil]; });
}

- (void)attemptRead:(NSUInteger)generation {
    if (self.finished || generation != self.generation ||
        (!self.fixture && !IsBillingPage(self.web.URL))) return;
    self.readAttempts++;
    [self.web evaluateJavaScript:self.reader completionHandler:^(id result, NSError *error) {
        if (self.finished || generation != self.generation) return;
        NSDictionary *good = error ? nil : NormalizeResult(result);
        if (!good) {
            if (self.fixture) [self finishWithError:@"parse-failed"];
            else if (self.readAttempts < 8) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2*NSEC_PER_SEC), dispatch_get_main_queue(), ^{ [self attemptRead:generation]; });
            else [self finishWithError:@"parse-failed"];
            return;
        }
        [self.defaults setObject:good forKey:@"subscriptionLastVerified"];
        [self.defaults setDouble:NSDate.date.timeIntervalSince1970 forKey:@"subscriptionLastVerifiedAt"];
        [self.defaults removeObjectForKey:@"subscriptionLastError"];
        [self.defaults synchronize];
        self.finished = YES;
        NSString *kind = [good[@"renewal"] isEqual:@"cancelled"] ? @"本期到期（已取消自动续订）" :
                         [good[@"renewal"] isEqual:@"renewing"] ? @"下次续费" : @"本期日期（续订状态未知）";
        self.status.stringValue = [NSString stringWithFormat:@"读取成功：%@ %@。网页将在退出时释放。", kind, good[@"date"]];
        if (self.fixture) printf("fixture_read=%s:%s\n", [good[@"date"] UTF8String], [good[@"renewal"] UTF8String]);
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, self.silent ? NSEC_PER_SEC : 3*NSEC_PER_SEC), dispatch_get_main_queue(), ^{ [NSApp terminate:nil]; });
    }];
}

- (void)webView:(WKWebView *)web didFinishNavigation:(WKNavigation *)navigation {
    (void)navigation;
    if (self.finished) return;
    if (self.diagnostic) {
        const char *page = [web.URL.host isEqual:@"auth.openai.com"] ? "auth" :
                           IsBillingPage(web.URL) ? "billing" :
                           [web.URL.host isEqual:@"chatgpt.com"] ? "chatgpt-other" : "other";
        fprintf(stderr, "subscription_navigation=%s\n", page);
    }
    if (self.fixture) {
        NSUInteger generation = self.generation;
        [self attemptRead:generation];
        return;
    }
    if ([web.URL.host isEqual:@"auth.openai.com"]) {
        self.redirectedAfterLogin = NO;
        if (self.silent) [self finishWithError:@"login-required"];
        else self.status.stringValue = @"请在官方页面完成登录或身份验证。";
        return;
    }
    if (IsBillingPage(web.URL)) {
        NSUInteger generation = self.generation;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 4*NSEC_PER_SEC), dispatch_get_main_queue(), ^{ [self attemptRead:generation]; });
    } else if (!self.redirectedAfterLogin && [web.URL.host isEqual:@"chatgpt.com"]) {
        self.redirectedAfterLogin = YES;
        [self loadBilling];
    } else if ([web.URL.host isEqual:@"chatgpt.com"]) {
        if (self.silent) [self finishWithError:@"login-required"];
        else self.status.stringValue = @"如果已登录，请点击“重新打开账单”；否则请先在页面登录。";
    }
}
- (void)webView:(WKWebView *)web decidePolicyForNavigationAction:(WKNavigationAction *)action decisionHandler:(void (^)(WKNavigationActionPolicy))handler {
    (void)web;
    NSURL *url = action.request.URL;
    BOOL allowed = ([url.scheme isEqual:@"https"] && LoginHostAllowed(url.host)) ||
                   ([url.scheme isEqual:@"about"] && [url.absoluteString isEqual:@"about:blank"]);
    if (self.diagnostic) {
        const char *page = [url.host isEqual:@"auth.openai.com"] ? "auth" :
                           IsBillingPage(url) ? "billing" :
                           [url.host isEqual:@"chatgpt.com"] ? "chatgpt-other" : "other";
        fprintf(stderr, "subscription_navigation_action=%s allowed=%d\n", page, allowed);
    }
    handler(allowed ? WKNavigationActionPolicyAllow : WKNavigationActionPolicyCancel);
}
- (WKWebView *)webView:(WKWebView *)web createWebViewWithConfiguration:(WKWebViewConfiguration *)configuration forNavigationAction:(WKNavigationAction *)action windowFeatures:(WKWindowFeatures *)features {
    (void)configuration; (void)features;
    NSURL *url = action.request.URL;
    if (!action.targetFrame && [url.scheme isEqual:@"https"] && LoginHostAllowed(url.host)) [web loadRequest:action.request];
    return nil;
}
- (void)webView:(WKWebView *)web didFailProvisionalNavigation:(WKNavigation *)navigation withError:(NSError *)error {
    (void)web; (void)navigation;
    if (error.code != NSURLErrorCancelled) [self finishWithError:@"load-failed"];
}
- (void)webView:(WKWebView *)web didFailNavigation:(WKNavigation *)navigation withError:(NSError *)error {
    [self webView:web didFailProvisionalNavigation:navigation withError:error];
}
- (void)webViewWebContentProcessDidTerminate:(WKWebView *)web { (void)web; [self finishWithError:@"load-failed"]; }
@end

int main(int argc, const char **argv) {
    (void)argc; (void)argv;
    @autoreleasepool {
        NSApplication *app = NSApplication.sharedApplication;
        SubscriptionSync *delegate = [SubscriptionSync new];
        app.delegate = delegate;
        [app setActivationPolicy:NSApplicationActivationPolicyAccessory];
        [app run];
    }
    return 0;
}
