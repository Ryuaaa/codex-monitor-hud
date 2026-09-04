#import <Foundation/Foundation.h>

// Only small, allowlisted protocol metadata may leave this compatibility layer.
FOUNDATION_EXPORT NSNumber *CodexProtocolNumber(id value);
FOUNDATION_EXPORT NSNumber *CodexProtocolBoolean(id value);
FOUNDATION_EXPORT NSDictionary *CodexQuotaReadRequest(NSNumber *requestID, BOOL background);
FOUNDATION_EXPORT NSString *CodexProtocolErrorKind(id error);
FOUNDATION_EXPORT BOOL CodexQuotaNeedsLegacyRetry(id error, BOOL lightweight, BOOL retried);
FOUNDATION_EXPORT NSString *CodexProtocolFailureText(NSString *kind, BOOL hasPrevious);
