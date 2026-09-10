#import "HUDLocalization.h"
#import <math.h>

NSString *HUDLanguage(void) {
    static NSString *active;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSString *value = [NSUserDefaults.standardUserDefaults stringForKey:@"displayLanguage"];
        active = [@[@"zh-Hans", @"zh-Hant", @"en", @"ja", @"ko"] containsObject:value] ? value : @"zh-Hans";
    });
    return active;
}

NSString *HUDL(NSString *key) {
    NSString *language = HUDLanguage();
    if ([language isEqualToString:@"zh-Hans"]) return key;
    static NSDictionary *catalog;
    static NSCache *traditional;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSData *data = [NSData dataWithContentsOfURL:[NSBundle.mainBundle URLForResource:@"HUDLocalizations" withExtension:@"json"]];
        id parsed = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
        catalog = [parsed isKindOfClass:NSDictionary.class] ? parsed : @{};
        traditional = [NSCache new]; traditional.countLimit = 1024;
    });
    if ([language isEqualToString:@"zh-Hant"]) {
        NSString *cached = [traditional objectForKey:key];
        if (cached) return cached;
        NSMutableString *text = [key mutableCopy];
        CFStringTransform((__bridge CFMutableStringRef)text, NULL, CFSTR("Simplified-Traditional"), false);
        [traditional setObject:[text copy] forKey:key];
        return text;
    }
    NSString *translation = catalog[key][language];
    return translation ?: key;
}

NSString *HUDCurrency(void) {
    NSString *value = [NSUserDefaults.standardUserDefaults stringForKey:@"displayCurrency"];
    return [@[@"CNY", @"USD", @"EUR", @"JPY", @"KRW"] containsObject:value] ? value : @"CNY";
}

static NSDictionary *HUDRates(void) {
    NSDictionary *saved = [NSUserDefaults.standardUserDefaults dictionaryForKey:@"exchangeRateSnapshot"];
    BOOL valid = [saved[@"date"] isKindOfClass:NSString.class] && [saved[@"rates"] isKindOfClass:NSDictionary.class];
    if (valid) for (NSString *code in @[@"CNY", @"EUR", @"JPY", @"KRW"]) {
        id rate = saved[@"rates"][code];
        if (![rate isKindOfClass:NSNumber.class] || CFGetTypeID((__bridge CFTypeRef)rate) == CFBooleanGetTypeID() ||
            !isfinite([rate doubleValue]) || [rate doubleValue] <= 0 || [rate doubleValue] > 1000000) { valid = NO; break; }
    }
    if (valid) return saved;
    // Public Frankfurter daily reference; no account data is sent.
    return @{ @"date": @"2026-09-10", @"rates": @{@"CNY": @6.7063, @"EUR": @0.86088, @"JPY": @154.18, @"KRW": @1343.8} };
}

NSString *HUDExchangeRateDate(void) { return HUDRates()[@"date"] ?: @"2026-09-10"; }

NSString *HUDMoney(double usd) {
    if (!isfinite(usd) || usd < 0) return @"--";
    NSString *currency = HUDCurrency();
    double rate = [currency isEqualToString:@"USD"] ? 1 : [HUDRates()[@"rates"][currency] doubleValue];
    if (!isfinite(rate) || rate <= 0 || !isfinite(usd * rate)) return @"--";
    NSNumberFormatter *f = [NSNumberFormatter new];
    f.locale = [NSLocale localeWithLocaleIdentifier:@{@"zh-Hans": @"zh_CN", @"zh-Hant": @"zh_TW", @"en": @"en_US", @"ja": @"ja_JP", @"ko": @"ko_KR"}[HUDLanguage()]];
    f.numberStyle = NSNumberFormatterDecimalStyle;
    f.minimumFractionDigits = ([currency isEqualToString:@"JPY"] || [currency isEqualToString:@"KRW"]) ? 0 : 2;
    f.maximumFractionDigits = (usd > 0 && usd * rate < 0.01) ? 4 : f.minimumFractionDigits;
    return [NSString stringWithFormat:@"%@ %@", currency, [f stringFromNumber:@(usd * rate)]];
}

void HUDRefreshExchangeRates(void) {
    // One bounded asynchronous fetch per day, including failed attempts. No extra timer.
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    NSTimeInterval now = NSDate.date.timeIntervalSince1970;
    if (now - [defaults doubleForKey:@"exchangeRateLastAttempt"] < 86400) return;
    [defaults setDouble:now forKey:@"exchangeRateLastAttempt"];
    NSURLSessionConfiguration *config = NSURLSessionConfiguration.ephemeralSessionConfiguration;
    config.timeoutIntervalForRequest = 8; config.timeoutIntervalForResource = 12;
    config.HTTPCookieStorage = nil; config.URLCredentialStorage = nil; config.URLCache = nil;
    NSURLSession *session = [NSURLSession sessionWithConfiguration:config];
    NSURL *url = [NSURL URLWithString:@"https://api.frankfurter.dev/v1/latest?base=USD&symbols=CNY,EUR,JPY,KRW"];
    [[session dataTaskWithURL:url completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
        if (!error && [(NSHTTPURLResponse *)response statusCode] == 200 && data.length < 16384) {
            id json = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
            if ([json isKindOfClass:NSDictionary.class] && [json[@"base"] isEqual:@"USD"] &&
                [json[@"date"] isKindOfClass:NSString.class] && [json[@"rates"] isKindOfClass:NSDictionary.class]) {
                NSDateFormatter *f = [NSDateFormatter new]; f.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
                f.dateFormat = @"yyyy-MM-dd"; f.lenient = NO;
                NSDate *date = [f dateFromString:json[@"date"]];
                BOOL valid = date && [date timeIntervalSinceNow] <= 86400 && [date timeIntervalSinceNow] > -14 * 86400;
                NSMutableDictionary *rates = [NSMutableDictionary dictionary];
                for (NSString *code in @[@"CNY", @"EUR", @"JPY", @"KRW"]) {
                    id n = json[@"rates"][code];
                    if (![n isKindOfClass:NSNumber.class] || CFGetTypeID((__bridge CFTypeRef)n) == CFBooleanGetTypeID() ||
                        !isfinite([n doubleValue]) || [n doubleValue] <= 0 || [n doubleValue] > 1000000) { valid = NO; break; }
                    rates[code] = n;
                }
                if (valid) [defaults setObject:@{@"date": json[@"date"], @"rates": rates} forKey:@"exchangeRateSnapshot"];
            }
        }
        [session finishTasksAndInvalidate];
    }] resume];
}
