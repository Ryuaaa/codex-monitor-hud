// Documentation-only renderer: production HUDView, synthetic values, no sampler or account calls.
#import "HUDView.h"
#import "HUDLocalization.h"
#import <ImageIO/ImageIO.h>

static NSString *Pick(NSArray<NSString *> *values, NSUInteger language) { return values[language]; }
static void Metric(HUDMetricCard *card, NSString *value, NSString *detail) {
    card.valueLabel.stringValue = value; card.subtitleLabel.stringValue = detail;
}
// Offscreen bitmap capture has no desktop vibrancy backdrop. Resolve the same
// semantic text colors in Dark Aqua before drawing, without editing product code.
static void ResolveTextColors(NSView *view) {
    if ([view isKindOfClass:NSTextField.class]) {
        NSTextField *label = (NSTextField *)view;
        NSColor *resolved = [label.textColor colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
        if (resolved) label.textColor = resolved;
    }
    for (NSView *child in view.subviews) ResolveTextColors(child);
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        if (argc != 3) return 2;
        NSArray *languages = @[@"zh-Hans", @"zh-Hant", @"en", @"ja", @"ko"];
        NSString *language = @(argv[1]); NSUInteger lang = [languages indexOfObject:language];
        if (lang == NSNotFound) return 2;
        [NSUserDefaults.standardUserDefaults setVolatileDomain:@{@"displayLanguage":language} forName:NSArgumentDomain];
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
        NSApp.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
        NSView *canvas = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 520, 580)];
        canvas.wantsLayer = YES; canvas.layer.backgroundColor = [NSColor colorWithRed:.055 green:.07 blue:.10 alpha:1].CGColor;
        canvas.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
        NSTextField *caption = [NSTextField labelWithString:Pick(@[@"原生界面 · 演示数据（非真实账户）", @"原生介面 · 示範資料（非真實帳戶）", @"Native UI · Demo data, not a real account", @"ネイティブUI · デモデータ（実アカウントではありません）", @"네이티브 UI · 예시 데이터 (실제 계정 아님)"], lang)];
        caption.font = [NSFont systemFontOfSize:12]; caption.textColor = NSColor.lightGrayColor;
        caption.frame = NSMakeRect(16, 548, 488, 22); [canvas addSubview:caption];
        HUDView *hud = [[HUDView alloc] initWithFrame:NSMakeRect(10, 10, 500, 528)];
        hud.appearance = [NSAppearance appearanceNamed:NSAppearanceNameVibrantDark];
        [canvas addSubview:hud]; [hud setCompact:YES]; [hud setBackgroundOpacity:1];
        [hud setUsageVisible:NO]; [hud setPlanVisible:NO]; [hud setModelQuotaVisible:NO];
        [hud setLongestTurnVisible:NO]; [hud setLongestStreakVisible:NO]; [hud setPeakDailyTokensVisible:NO];
        [hud setQuotaDetailsVisible:NO]; [hud setServiceStatusVisible:NO]; [hud setQuotaForecastVisible:NO];
        [hud setTokenWindowsVisible:YES]; [hud setLocalCostVisible:YES];
        [hud setTaskActivityVisible:YES]; [hud setRecentTasksVisible:YES];
        [hud setHomePlanVisible:NO]; [hud setHomeUsageVisible:NO]; [hud setHomeModelQuotaVisible:NO];
        [hud setHomeLocalCostVisible:NO]; [hud setHomeTokenWindowsVisible:NO]; [hud setHomeQuotaForecastVisible:NO];
        [hud setHomeServiceStatusVisible:NO]; [hud setHomeQuotaDetailsVisible:NO]; [hud setHomeRecentTasksVisible:NO];
        [hud setHomeLongestTurnVisible:NO]; [hud setHomeLongestStreakVisible:NO]; [hud setHomePeakDailyTokensVisible:NO];
        [hud setHomeMemoryAppsVisible:NO]; [hud setMemoryAppsVisible:NO];
        NSString *sample = Pick(@[@"演示数据", @"示範資料", @"Demo data", @"デモデータ", @"예시 데이터"], lang);
        NSString *normal = Pick(@[@"正常 · 目前没有明显瓶颈", @"正常 · 目前沒有明顯瓶頸", @"Normal · No clear bottleneck", @"正常 · 明らかなボトルネックなし", @"정상 · 뚜렷한 병목 없음"], lang);
        NSString *inferred = Pick(@[@"本机活动推测 · 非精确运行状态", @"本機活動推測 · 非精確執行狀態", @"Inferred activity, not exact runtime state", @"ローカルの推定活動（正確な実行状態ではありません）", @"로컬 활동 추정 · 정확한 실행 상태 아님"], lang);
        hud.codexStatusLabel.stringValue = hud.homeCodexStatusLabel.stringValue = @"Codex · Pro · DEMO";
        for (HUDQuotaCard *card in @[hud.fiveHourCard, hud.homeFiveHourCard])
            [card showAvailable:YES remaining:72 reset:Pick(@[@"2小时后恢复", @"2小時後恢復", @"Resets in 2h", @"2時間後にリセット", @"2시간 후 초기화"], lang) accent:NSColor.systemTealColor];
        for (HUDQuotaCard *card in @[hud.weeklyCard, hud.homeWeeklyCard])
            [card showAvailable:YES remaining:58 reset:Pick(@[@"3天后恢复", @"3天後恢復", @"Resets in 3d", @"3日後にリセット", @"3일 후 초기화"], lang) accent:NSColor.systemTealColor];
        for (HUDMetricCard *card in @[hud.taskActivityCard, hud.homeTaskActivityCard])
            Metric(card, Pick(@[@"2个活跃 · 最长8分钟", @"2個活躍 · 最長8分鐘", @"2 active · Longest 8 min", @"2件が活動中 · 最長8分", @"활동 2개 · 최장 8분"], lang), inferred);
        [hud.recentTasksCard updateRows:@[Pick(@[@"演示任务：整理项目文档", @"示範任務：整理專案文件", @"Demo: organize project docs", @"デモ：プロジェクト文書の整理", @"예시: 프로젝트 문서 정리"], lang), Pick(@[@"演示任务：检查界面布局", @"示範任務：檢查介面配置", @"Demo: review interface layout", @"デモ：画面レイアウトの確認", @"예시: 화면 배치 점검"], lang)] footer:sample];
        Metric(hud.fiveHourTokensCard, @"120K", @"10% / 1.20M");
        Metric(hud.rollingDayTokensCard, @"360K", @"30% / 1.20M");
        Metric(hud.weeklyTokensCard, @"1.20M", @"100%");
        Metric(hud.localCostCard, @"1.20M · CNY 24.80", Pick(@[@"API等价估算 · 不是订阅账单", @"API等價估算 · 不是訂閱帳單", @"API-equivalent estimate, not your bill", @"API相当額の推定（請求額ではありません）", @"API 환산 추정액 · 실제 청구액 아님"], lang));
        hud.homeComputerStatusLabel.stringValue = hud.computerStatusLabel.stringValue = normal;
        Metric(hud.homeBottleneckCard, Pick(@[@"暂无明显瓶颈", @"暫無明顯瓶頸", @"No clear bottleneck", @"明らかなボトルネックなし", @"뚜렷한 병목 없음"],lang), sample);
        Metric(hud.homeSystemCard, @"CPU 24% · RAM 45%", @"14.4 / 32 GB");
        Metric(hud.bottleneckCard, normal, sample);
        Metric(hud.impactCard, @"Codex · 4.8 GB / 15%", @"32 GB RAM · CPU 9%");
        Metric(hud.cpuCard, @"24%", @"Codex 9%");
        Metric(hud.memoryPressureCard, @"14.4 / 32 GB · 45%", sample);
        hud.attributionLabel.stringValue = hud.homeAttributionLabel.stringValue = @"Codex RAM 4.8 / 32 GB · 15%";
        hud.healthLabel.stringValue = sample;
        hud.sparkline.values = hud.homeSparkline.values = @[@12,@18,@15,@25,@35,@28,@22,@24,@19,@30,@27,@24];
        hud.trendLabel.stringValue = hud.homeTrendLabel.stringValue = sample;
        hud.codexFreshnessLabel.stringValue = hud.homeFreshnessLabel.stringValue = hud.computerFreshnessLabel.stringValue = sample;
        [canvas.appearance performAsCurrentDrawingAppearance:^{ ResolveTextColors(canvas); }];
        NSString *directory = @(argv[2]);
        NSURL *gifURL = [NSURL fileURLWithPath:[directory stringByAppendingPathComponent:[NSString stringWithFormat:@"tour-%@.gif", language]]];
        CGImageDestinationRef gif = CGImageDestinationCreateWithURL((__bridge CFURLRef)gifURL, CFSTR("com.compuserve.gif"), 3, NULL);
        CGImageDestinationSetProperties(gif, (__bridge CFDictionaryRef)@{(__bridge NSString *)kCGImagePropertyGIFDictionary:@{(__bridge NSString *)kCGImagePropertyGIFLoopCount:@0}});
        for (NSInteger page=0; page<3; page++) {
            [hud setPage:page]; [canvas layoutSubtreeIfNeeded];
            NSBitmapImageRep *bitmap = [canvas bitmapImageRepForCachingDisplayInRect:canvas.bounds];
            if (!bitmap) return 3;
            [canvas.appearance performAsCurrentDrawingAppearance:^{
                [canvas cacheDisplayInRect:canvas.bounds toBitmapImageRep:bitmap];
            }];
            NSString *path = [directory stringByAppendingPathComponent:[NSString stringWithFormat:@"%@-%@.png", @[@"home", @"codex", @"computer"][page], language]];
            if (![[bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:path atomically:YES]) return 4;
            CGImageDestinationAddImage(gif, bitmap.CGImage, (__bridge CFDictionaryRef)@{(__bridge NSString *)kCGImagePropertyGIFDictionary:@{(__bridge NSString *)kCGImagePropertyGIFDelayTime:@3}});
        }
        BOOL ok = CGImageDestinationFinalize(gif); CFRelease(gif);
        CGImageSourceRef source = CGImageSourceCreateWithURL((__bridge CFURLRef)gifURL, NULL);
        ok = ok && source && CGImageSourceGetCount(source) == 3;
        if (source) {
            for (size_t index=0; index<CGImageSourceGetCount(source); index++) {
                NSDictionary *properties = CFBridgingRelease(CGImageSourceCopyPropertiesAtIndex(source, index, NULL));
                NSDictionary *timing = properties[(__bridge NSString *)kCGImagePropertyGIFDictionary];
                ok = ok && [timing[(__bridge NSString *)kCGImagePropertyGIFDelayTime] doubleValue] == 3;
            }
            CFRelease(source);
        }
        printf("native_docs_render=%s language=%s\n", ok ? "pass" : "fail", argv[1]);
        return ok ? 0 : 5;
    }
}
