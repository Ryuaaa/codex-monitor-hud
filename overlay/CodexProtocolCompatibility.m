#import "HUDLocalization.h"
#import "CodexProtocolCompatibility.h"
#import <CoreFoundation/CoreFoundation.h>
#import <math.h>
#import <limits.h>

NSNumber *CodexProtocolNumber(id value) {
    if (![value isKindOfClass:NSNumber.class] || CFGetTypeID((__bridge CFTypeRef)value) == CFBooleanGetTypeID()) return nil;
    return isfinite([value doubleValue]) ? value : nil;
}

NSNumber *CodexProtocolBoolean(id value) {
    return [value isKindOfClass:NSNumber.class] && CFGetTypeID((__bridge CFTypeRef)value) == CFBooleanGetTypeID() ? value : nil;
}

NSNumber *CodexProtocolTokenCount(id value) {
    NSNumber *number = CodexProtocolNumber(value);
    double count = number.doubleValue;
    if (!number || count < 0 || count >= 0x1p63 || floor(count) != count) return nil;
    return number;
}

long long CodexAddTokenCounts(long long left, long long right) {
    left = MAX(0LL, left); right = MAX(0LL, right);
    return left > LLONG_MAX - right ? LLONG_MAX : left + right;
}

NSDictionary *CodexQuotaReadRequest(NSNumber *requestID, BOOL background) {
    if (background) return @{ @"id": requestID, @"method": @"account/rateLimits/read", @"params": @{ @"excludeResetCreditDetails": @YES } };
    // Omit params entirely: older servers used a parameterless method.
    return @{ @"id": requestID, @"method": @"account/rateLimits/read" };
}

NSString *CodexProtocolErrorKind(id error) {
    if (![error isKindOfClass:NSDictionary.class]) return @"protocol";
    NSNumber *code = CodexProtocolNumber(error[@"code"]);
    if (code.integerValue == -32601) return @"unsupported";
    if (code.integerValue == -32602) return @"invalid_parameters";
    if (code.integerValue == -32700 || code.integerValue == -32600) return @"protocol";
    NSDictionary *data = [error[@"data"] isKindOfClass:NSDictionary.class] ? error[@"data"] : nil;
    id info = data[@"codexErrorInfo"];
    if ([info isKindOfClass:NSString.class]) {
        NSDictionary *kinds = @{ @"unauthorized": @"authentication", @"rateLimitExceeded": @"rate_limited",
            @"usageLimitExceeded": @"quota_exhausted", @"serverOverloaded": @"overloaded",
            @"internalServerError": @"server", @"misalignmentPolicyViolation": @"policy_blocked" };
        return kinds[info] ?: @"unknown";
    }
    if ([info isKindOfClass:NSDictionary.class]) {
        for (NSString *key in @[@"httpConnectionFailed", @"responseStreamConnectionFailed", @"responseStreamDisconnected", @"responseTooManyFailedAttempts"]) {
            NSDictionary *detail = [info[key] isKindOfClass:NSDictionary.class] ? info[key] : nil;
            if (!detail) continue;
            NSInteger http = [CodexProtocolNumber(detail[@"httpStatusCode"]) integerValue];
            if (http == 401 || http == 403) return @"authentication";
            if (http == 429) return @"rate_limited";
            if (http == 408 || http == 504) return @"timeout";
            if (http >= 500) return @"server";
            return @"network";
        }
    }
    // Never echo or classify by free-form messages, data._meta, or tool content.
    return @"unknown";
}

BOOL CodexQuotaNeedsLegacyRetry(id error, BOOL lightweight, BOOL retried) {
    if (!lightweight || retried || ![error isKindOfClass:NSDictionary.class]) return NO;
    NSNumber *code = CodexProtocolNumber(error[@"code"]);
    // Older app-servers reject a parameter object while deserializing the request
    // (-32600), before the handler can return Invalid Params (-32602).
    // Retry this one known read once, never other requests or malformed responses.
    return code && (code.integerValue == -32602 || code.integerValue == -32600);
}

NSString *CodexProtocolFailureText(NSString *kind, BOOL hasPrevious) {
    NSDictionary *labels = @{ @"unsupported": HUDL(@"当前Codex不支持此接口"), @"invalid_parameters": HUDL(@"接口参数不兼容"),
        @"protocol": HUDL(@"接口数据格式变化"), @"authentication": HUDL(@"需要检查Codex登录"),
        @"rate_limited": HUDL(@"请求被限速，稍后重试"), @"quota_exhausted": HUDL(@"官方报告用量限制"),
        @"overloaded": HUDL(@"Codex服务繁忙"), @"server": HUDL(@"Codex服务异常"), @"network": HUDL(@"连接中断，稍后重试"),
        @"timeout": HUDL(@"接口读取超时"), @"policy_blocked": HUDL(@"请求被策略阻止"), @"missing_executable": HUDL(@"未找到Codex本机接口"),
        @"launch_failed": HUDL(@"无法启动Codex本机接口"), @"unknown": HUDL(@"接口读取失败") };
    NSString *label = labels[kind] ?: labels[@"unknown"];
    return hasPrevious ? [label stringByAppendingString:HUDL(@"，显示上次数据")] : label;
}
