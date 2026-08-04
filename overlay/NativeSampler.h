#import <Cocoa/Cocoa.h>

double NativeRawCPUPercentFromAbsoluteTime(uint64_t deltaCPUTime, NSTimeInterval elapsed, uint32_t timebaseNumer, uint32_t timebaseDenom);
double NativeUsedMemoryGiBFromPageCounts(uint64_t active, uint64_t inactive, uint64_t speculative, uint64_t wired, uint64_t compressed, uint64_t purgeable, uint64_t external, uint64_t pageSize);

@interface NativeTopApp : NSObject
@property(nonatomic, copy) NSString *name;
@property(nonatomic) double cpuPercent;
@property(nonatomic) double memoryGiB;
@property(nonatomic) double diskReadMBps;
@property(nonatomic) double diskWriteMBps;
@end

@interface NativeSnapshot : NSObject
@property(nonatomic) NSTimeInterval timestamp;
@property(nonatomic) double systemCPUPercent;
@property(nonatomic) double codexCPUPercent;
@property(nonatomic) double codexCPUCores;
@property(nonatomic) double codexMemoryGiB;
@property(nonatomic) double codexMemoryPercent;
@property(nonatomic) double totalMemoryGiB;
@property(nonatomic) double systemMemoryUsedGiB;
@property(nonatomic) double systemMemoryUsedPercent;
@property(nonatomic) NSInteger codexProcessCount;
@property(nonatomic) NSInteger codexRendererCount;
@property(nonatomic) NSInteger codexHelperCount;
@property(nonatomic) double codexLargestGiB;
@property(nonatomic) double compressedGiB;
@property(nonatomic) double swapUsedGiB;
@property(nonatomic) double swapDelta10MinMiB;
@property(nonatomic) NSInteger memoryPressureLevel;
@property(nonatomic, copy) NSString *memoryPressureText;
@property(nonatomic) NSInteger thermalLevel;
@property(nonatomic, copy) NSString *thermalText;
@property(nonatomic) double networkDownMBps;
@property(nonatomic) double networkUpMBps;
@property(nonatomic) double codexDiskReadMBps;
@property(nonatomic) double codexDiskWriteMBps;
@property(nonatomic, copy) NSArray<NativeTopApp *> *topCPUApps;
@property(nonatomic, copy) NSArray<NativeTopApp *> *topMemoryApps;
@end

@interface NativeSampler : NSObject
@property(nonatomic) BOOL collectTopApps;
@property(nonatomic) BOOL collectSecondaryMetrics;
@property(nonatomic) BOOL collectThermalMetrics;
- (NativeSnapshot *)sample;
@end
