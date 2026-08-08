#import <Foundation/Foundation.h>

FOUNDATION_EXPORT NSDictionary<NSString *, id> *HUDOpenAIServiceStatusFromJSONData(NSData *data);

@interface HUDOpenAIServiceStatusSnapshot : NSObject
@property(nonatomic) BOOL available;
@property(nonatomic) NSTimeInterval updatedAt;
@property(nonatomic, copy) NSString *errorText;
@property(nonatomic, copy) NSString *headline;
@property(nonatomic, copy) NSString *detail;
@property(nonatomic, copy) NSString *overallIndicator;
@property(nonatomic, copy) NSString *codexComponentStatus;
@end

@interface HUDOpenAIServiceStatusProvider : NSObject
@property(nonatomic, readonly) HUDOpenAIServiceStatusSnapshot *snapshot;
@property(nonatomic, copy) void (^updateHandler)(void);
- (void)refresh;
- (void)stop;
@end
