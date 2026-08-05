#import <Cocoa/Cocoa.h>

typedef NS_ENUM(NSInteger, HUDUpdateCheckResult) {
    HUDUpdateCheckResultUpToDate = 0,
    HUDUpdateCheckResultAvailable = 1,
    HUDUpdateCheckResultFailed = 2,
};

@interface HUDReleaseInfo : NSObject
@property(nonatomic, copy) NSString *version;
@property(nonatomic, copy) NSString *tagName;
@property(nonatomic, copy) NSString *releaseNotes;
@property(nonatomic, strong) NSURL *assetURL;
@property(nonatomic, copy) NSString *assetDigest;
@property(nonatomic, strong) NSURL *releasePageURL;
@end

NSComparisonResult HUDCompareVersions(NSString *left, NSString *right);
NSString *HUDSHA256ForFile(NSURL *fileURL);
NSString *HUDInstallHelperScript(void);
HUDReleaseInfo *HUDReleaseInfoFromDictionary(NSDictionary *dictionary, NSError **error);

@interface HUDUpdateManager : NSObject
@property(nonatomic, strong, readonly) HUDReleaseInfo *latestRelease;
@property(nonatomic, copy, readonly) NSString *currentVersion;
- (void)checkForUpdates:(void (^)(HUDUpdateCheckResult result, HUDReleaseInfo *release, NSString *message))completion;
- (void)installRelease:(HUDReleaseInfo *)release completion:(void (^)(BOOL prepared, NSString *message))completion;
@end
