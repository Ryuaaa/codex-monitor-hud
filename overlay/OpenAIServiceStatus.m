#import "OpenAIServiceStatus.h"

static NSString *HUDServiceHeadline(NSString *componentStatus, NSString *overallIndicator) {
    NSDictionary<NSString *, NSString *> *componentLabels = @{
        @"operational": @"Codex服务正常",
        @"degraded_performance": @"Codex服务性能下降",
        @"partial_outage": @"Codex服务部分故障",
        @"major_outage": @"Codex服务严重故障",
        @"under_maintenance": @"Codex服务维护中"
    };
    if (componentLabels[componentStatus]) return componentLabels[componentStatus];
    NSDictionary<NSString *, NSString *> *overallLabels = @{
        @"none": @"OpenAI服务正常",
        @"minor": @"OpenAI部分服务异常",
        @"major": @"OpenAI服务发生故障",
        @"critical": @"OpenAI服务严重故障",
        @"maintenance": @"OpenAI服务维护中"
    };
    return overallLabels[overallIndicator] ?: @"OpenAI状态未知";
}

static NSString *HUDOverallDetail(NSString *indicator) {
    if ([indicator isEqualToString:@"none"]) return @"OpenAI整体正常";
    if ([indicator isEqualToString:@"minor"]) return @"OpenAI部分服务异常";
    if ([indicator isEqualToString:@"major"]) return @"OpenAI整体故障";
    if ([indicator isEqualToString:@"critical"]) return @"OpenAI整体严重故障";
    if ([indicator isEqualToString:@"maintenance"]) return @"OpenAI整体维护中";
    return @"OpenAI整体状态未知";
}

NSDictionary<NSString *, id> *HUDOpenAIServiceStatusFromJSONData(NSData *data) {
    if (data.length == 0) return @{};
    NSDictionary *object = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    if (![object isKindOfClass:NSDictionary.class]) return @{};
    NSDictionary *status = [object[@"status"] isKindOfClass:NSDictionary.class] ? object[@"status"] : nil;
    NSString *indicator = [status[@"indicator"] isKindOfClass:NSString.class] ? [status[@"indicator"] lowercaseString] : nil;
    if (indicator.length == 0) return @{};
    NSString *componentStatus = nil;
    for (id value in [object[@"components"] isKindOfClass:NSArray.class] ? object[@"components"] : @[]) {
        if (![value isKindOfClass:NSDictionary.class]) continue;
        NSDictionary *component = value;
        NSString *name = [component[@"name"] isKindOfClass:NSString.class] ? component[@"name"] : @"";
        if ([name localizedCaseInsensitiveContainsString:@"Codex in ChatGPT Desktop"]) {
            componentStatus = [component[@"status"] isKindOfClass:NSString.class] ? [component[@"status"] lowercaseString] : nil;
            break;
        }
    }
    return @{
        @"headline": HUDServiceHeadline(componentStatus, indicator),
        @"detail": HUDOverallDetail(indicator),
        @"overallIndicator": indicator,
        @"codexComponentStatus": componentStatus ?: @""
    };
}

@implementation HUDOpenAIServiceStatusSnapshot
@end

@interface HUDOpenAIServiceStatusProvider ()
@property(nonatomic, readwrite) HUDOpenAIServiceStatusSnapshot *snapshot;
@property(nonatomic, strong) NSURLSessionDataTask *task;
@end

@implementation HUDOpenAIServiceStatusProvider

- (instancetype)init {
    self = [super init];
    if (!self) return nil;
    _snapshot = [HUDOpenAIServiceStatusSnapshot new];
    _snapshot.headline = @"正在检查官方状态";
    _snapshot.detail = @"来源 OpenAI官方状态页";
    return self;
}

- (void)refresh {
    if (self.task) return;
    NSURL *url = [NSURL URLWithString:@"https://status.openai.com/api/v2/summary.json"];
    NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:url cachePolicy:NSURLRequestReloadIgnoringLocalCacheData timeoutInterval:8.0];
    [request setValue:@"application/json" forHTTPHeaderField:@"Accept"];
    __weak typeof(self) weakSelf = self;
    self.task = [NSURLSession.sharedSession dataTaskWithRequest:request completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
        dispatch_async(dispatch_get_main_queue(), ^{
            typeof(self) strongSelf = weakSelf;
            if (!strongSelf) return;
            strongSelf.task = nil;
            NSHTTPURLResponse *http = [response isKindOfClass:NSHTTPURLResponse.class] ? (NSHTTPURLResponse *)response : nil;
            NSDictionary<NSString *, id> *parsed = !error && http.statusCode == 200 ? HUDOpenAIServiceStatusFromJSONData(data) : @{};
            if (parsed.count == 0) {
                strongSelf.snapshot.errorText = strongSelf.snapshot.available ? @"更新失败，显示上次状态" : @"官方状态页暂不可达";
            } else {
                strongSelf.snapshot.available = YES;
                strongSelf.snapshot.updatedAt = NSDate.date.timeIntervalSince1970;
                strongSelf.snapshot.errorText = nil;
                strongSelf.snapshot.headline = parsed[@"headline"];
                strongSelf.snapshot.detail = parsed[@"detail"];
                strongSelf.snapshot.overallIndicator = parsed[@"overallIndicator"];
                strongSelf.snapshot.codexComponentStatus = parsed[@"codexComponentStatus"];
            }
            if (strongSelf.updateHandler) strongSelf.updateHandler();
        });
    }];
    [self.task resume];
}

- (void)stop {
    [self.task cancel];
    self.task = nil;
}

@end
