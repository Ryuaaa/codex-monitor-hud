#import "NativeSampler.h"

#import <dispatch/dispatch.h>
#import <ifaddrs.h>
#import <libproc.h>
#import <mach/mach.h>
#import <net/if.h>
#import <net/if_var.h>
#import <sys/proc_info.h>
#import <sys/resource.h>
#import <sys/socket.h>
#import <sys/sysctl.h>
#import <unistd.h>

@implementation NativeTopApp
@end

@implementation NativeSnapshot
@end

@interface ProcessMeta : NSObject
@property(nonatomic) pid_t pid;
@property(nonatomic) pid_t ppid;
@property(nonatomic) uint64_t startSeconds;
@property(nonatomic, copy) NSString *path;
@property(nonatomic, copy) NSString *name;
@end

@implementation ProcessMeta
@end

@interface AppAggregate : NSObject
@property(nonatomic, copy) NSString *name;
@property(nonatomic) double cpuRawPercent;
@property(nonatomic) uint64_t physicalBytes;
@property(nonatomic) double diskReadMBps;
@property(nonatomic) double diskWriteMBps;
@end

@implementation AppAggregate
@end

@interface NativeSampler ()
@property(nonatomic, strong) NSMutableDictionary<NSString *, NSDictionary *> *previousUsage;
@property(nonatomic, strong) NSDictionary<NSNumber *, ProcessMeta *> *processInventory;
@property(nonatomic, strong) NSSet<NSNumber *> *codexPIDs;
@property(nonatomic, strong) NSArray<NativeTopApp *> *cachedTopCPUApps;
@property(nonatomic, strong) NSArray<NativeTopApp *> *cachedTopMemoryApps;
@property(nonatomic) NSTimeInterval lastInventoryTime;
@property(nonatomic) NSTimeInterval lastAllAppsTime;
@property(nonatomic) NSTimeInterval lastSecondaryTime;
@property(nonatomic) NSTimeInterval lastThermalTime;
@property(nonatomic) uint64_t previousSystemBusy;
@property(nonatomic) uint64_t previousSystemTotal;
@property(nonatomic) BOOL hasPreviousSystemCPU;
@property(nonatomic) uint64_t previousNetworkIn;
@property(nonatomic) uint64_t previousNetworkOut;
@property(nonatomic) NSTimeInterval previousNetworkTime;
@property(nonatomic) double cachedNetworkDownMBps;
@property(nonatomic) double cachedNetworkUpMBps;
@property(nonatomic) double cachedSwapUsedGiB;
@property(nonatomic) double cachedSwapDelta10MinMiB;
@property(nonatomic, strong) NSMutableArray<NSDictionary *> *swapHistory;
@property(nonatomic) NSInteger cachedThermalLevel;
@property(nonatomic, copy) NSString *cachedThermalText;
@property(nonatomic) NSInteger memoryPressureLevel;
@property(nonatomic, copy) NSString *memoryPressureText;
@property(nonatomic) dispatch_source_t memoryPressureSource;
@end

@implementation NativeSampler

- (instancetype)init {
    self = [super init];
    if (!self) return nil;

    _previousUsage = [NSMutableDictionary dictionary];
    _processInventory = @{};
    _codexPIDs = [NSSet set];
    _cachedTopCPUApps = @[];
    _cachedTopMemoryApps = @[];
    _swapHistory = [NSMutableArray array];
    _cachedThermalText = @"正常";
    _memoryPressureText = @"正常";
    _memoryPressureLevel = 0;

    _memoryPressureSource = dispatch_source_create(
        DISPATCH_SOURCE_TYPE_MEMORYPRESSURE,
        0,
        DISPATCH_MEMORYPRESSURE_NORMAL | DISPATCH_MEMORYPRESSURE_WARN | DISPATCH_MEMORYPRESSURE_CRITICAL,
        dispatch_get_main_queue()
    );
    if (_memoryPressureSource) {
        __weak typeof(self) weakSelf = self;
        dispatch_source_set_event_handler(_memoryPressureSource, ^{
            unsigned long data = dispatch_source_get_data(weakSelf.memoryPressureSource);
            if (data & DISPATCH_MEMORYPRESSURE_CRITICAL) {
                weakSelf.memoryPressureLevel = 2;
                weakSelf.memoryPressureText = @"严重";
            } else if (data & DISPATCH_MEMORYPRESSURE_WARN) {
                weakSelf.memoryPressureLevel = 1;
                weakSelf.memoryPressureText = @"注意";
            } else {
                weakSelf.memoryPressureLevel = 0;
                weakSelf.memoryPressureText = @"正常";
            }
        });
        dispatch_resume(_memoryPressureSource);
    }
    return self;
}

- (void)dealloc {
    if (_memoryPressureSource) dispatch_source_cancel(_memoryPressureSource);
}

- (NSString *)usageKeyForMeta:(ProcessMeta *)meta {
    return [NSString stringWithFormat:@"%d:%llu", meta.pid, meta.startSeconds];
}

- (NSString *)applicationNameForMeta:(ProcessMeta *)meta codex:(BOOL)isCodex {
    if (isCodex) return @"Codex/ChatGPT";
    NSArray<NSString *> *components = meta.path.pathComponents;
    for (NSString *component in components) {
        if ([component.pathExtension.lowercaseString isEqualToString:@"app"]) {
            return component.stringByDeletingPathExtension;
        }
    }
    if (meta.name.length > 0) return meta.name;
    return meta.path.lastPathComponent.length > 0 ? meta.path.lastPathComponent : [NSString stringWithFormat:@"PID %d", meta.pid];
}

- (NSDictionary *)readUsageForMeta:(ProcessMeta *)meta now:(NSTimeInterval)now {
    struct rusage_info_v4 usage = {0};
    if (proc_pid_rusage(meta.pid, RUSAGE_INFO_V4, (rusage_info_t *)&usage) != 0) return nil;

    NSString *key = [self usageKeyForMeta:meta];
    NSDictionary *previous = self.previousUsage[key];
    uint64_t cpuTime = usage.ri_user_time + usage.ri_system_time;
    double cpuRawPercent = 0;
    double readMBps = 0;
    double writeMBps = 0;
    if (previous) {
        NSTimeInterval elapsed = now - [previous[@"time"] doubleValue];
        uint64_t previousCPU = [previous[@"cpu"] unsignedLongLongValue];
        uint64_t previousRead = [previous[@"read"] unsignedLongLongValue];
        uint64_t previousWrite = [previous[@"write"] unsignedLongLongValue];
        if (elapsed > 0.2) {
            if (cpuTime >= previousCPU) cpuRawPercent = (double)(cpuTime - previousCPU) / (elapsed * 1e9) * 100.0;
            if (usage.ri_diskio_bytesread >= previousRead) readMBps = (double)(usage.ri_diskio_bytesread - previousRead) / elapsed / 1048576.0;
            if (usage.ri_diskio_byteswritten >= previousWrite) writeMBps = (double)(usage.ri_diskio_byteswritten - previousWrite) / elapsed / 1048576.0;
        }
    }
    self.previousUsage[key] = @{
        @"time": @(now),
        @"cpu": @(cpuTime),
        @"read": @(usage.ri_diskio_bytesread),
        @"write": @(usage.ri_diskio_byteswritten)
    };
    return @{
        @"cpu": @(MAX(0, cpuRawPercent)),
        @"memory": @(usage.ri_resident_size),
        @"readRate": @(MAX(0, readMBps)),
        @"writeRate": @(MAX(0, writeMBps))
    };
}

- (void)refreshInventoryAtTime:(NSTimeInterval)now {
    int capacity = proc_listallpids(NULL, 0);
    if (capacity <= 0) return;
    pid_t *pids = calloc((size_t)capacity + 64, sizeof(pid_t));
    int count = proc_listallpids(pids, (capacity + 64) * (int)sizeof(pid_t));
    if (count <= 0) {
        free(pids);
        return;
    }

    NSMutableDictionary<NSNumber *, ProcessMeta *> *inventory = [NSMutableDictionary dictionary];
    NSMutableSet<NSNumber *> *roots = [NSMutableSet set];
    NSString *codexHome = [NSHomeDirectory() stringByAppendingPathComponent:@".codex/"];

    for (int index = 0; index < count; index++) {
        pid_t pid = pids[index];
        if (pid <= 0 || pid == getpid()) continue;

        struct proc_bsdinfo bsd = {0};
        int bsdSize = proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bsd, sizeof(bsd));
        if (bsdSize != sizeof(bsd)) continue;

        char pathBuffer[PROC_PIDPATHINFO_MAXSIZE] = {0};
        int pathLength = proc_pidpath(pid, pathBuffer, sizeof(pathBuffer));
        NSString *path = pathLength > 0 ? [NSString stringWithUTF8String:pathBuffer] : @"";
        NSString *name = bsd.pbi_name[0] ? [NSString stringWithUTF8String:bsd.pbi_name] : (bsd.pbi_comm[0] ? [NSString stringWithUTF8String:bsd.pbi_comm] : @"");

        ProcessMeta *meta = [ProcessMeta new];
        meta.pid = pid;
        meta.ppid = (pid_t)bsd.pbi_ppid;
        meta.startSeconds = bsd.pbi_start_tvsec;
        meta.path = path ?: @"";
        meta.name = name ?: @"";
        inventory[@(pid)] = meta;

        BOOL monitorOwnedHelper = meta.ppid == getpid();
        if (!monitorOwnedHelper && ([meta.path hasPrefix:@"/Applications/ChatGPT.app/"] || [meta.path hasPrefix:codexHome])) {
            [roots addObject:@(pid)];
        }
    }
    free(pids);

    NSMutableSet<NSNumber *> *included = [roots mutableCopy];
    BOOL changed = YES;
    while (changed) {
        changed = NO;
        for (NSNumber *pidNumber in inventory) {
            if ([included containsObject:pidNumber]) continue;
            ProcessMeta *meta = inventory[pidNumber];
            if ([included containsObject:@(meta.ppid)]) {
                [included addObject:pidNumber];
                changed = YES;
            }
        }
    }

    self.processInventory = inventory;
    self.codexPIDs = included;
    self.lastInventoryTime = now;

    NSMutableSet<NSString *> *liveKeys = [NSMutableSet set];
    for (ProcessMeta *meta in inventory.allValues) [liveKeys addObject:[self usageKeyForMeta:meta]];
    for (NSString *key in self.previousUsage.allKeys.copy) {
        if (![liveKeys containsObject:key]) [self.previousUsage removeObjectForKey:key];
    }
}

- (double)readSystemCPUPercent {
    natural_t processorCount = 0;
    processor_info_array_t cpuInfo = NULL;
    mach_msg_type_number_t cpuInfoCount = 0;
    kern_return_t result = host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO, &processorCount, &cpuInfo, &cpuInfoCount);
    if (result != KERN_SUCCESS || !cpuInfo) return 0;

    uint64_t busy = 0;
    uint64_t total = 0;
    for (natural_t cpu = 0; cpu < processorCount; cpu++) {
        natural_t offset = CPU_STATE_MAX * cpu;
        uint64_t user = cpuInfo[offset + CPU_STATE_USER];
        uint64_t system = cpuInfo[offset + CPU_STATE_SYSTEM];
        uint64_t nice = cpuInfo[offset + CPU_STATE_NICE];
        uint64_t idle = cpuInfo[offset + CPU_STATE_IDLE];
        busy += user + system + nice;
        total += user + system + nice + idle;
    }
    vm_deallocate(mach_task_self(), (vm_address_t)cpuInfo, cpuInfoCount * sizeof(integer_t));

    double percent = 0;
    if (self.hasPreviousSystemCPU && total > self.previousSystemTotal && busy >= self.previousSystemBusy) {
        percent = (double)(busy - self.previousSystemBusy) / (double)(total - self.previousSystemTotal) * 100.0;
    }
    self.previousSystemBusy = busy;
    self.previousSystemTotal = total;
    self.hasPreviousSystemCPU = YES;
    return MIN(100, MAX(0, percent));
}

- (double)readCompressedGiB {
    vm_statistics64_data_t stats = {0};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&stats, &count) != KERN_SUCCESS) return 0;
    vm_size_t pageSize = 0;
    host_page_size(mach_host_self(), &pageSize);
    return (double)stats.compressor_page_count * (double)pageSize / 1073741824.0;
}

- (void)refreshSwapAtTime:(NSTimeInterval)now {
    struct xsw_usage swap = {0};
    size_t size = sizeof(swap);
    if (sysctlbyname("vm.swapusage", &swap, &size, NULL, 0) != 0) return;
    self.cachedSwapUsedGiB = (double)swap.xsu_used / 1073741824.0;
    [self.swapHistory addObject:@{@"time": @(now), @"mib": @((double)swap.xsu_used / 1048576.0)}];
    while (self.swapHistory.count > 0 && now - [self.swapHistory.firstObject[@"time"] doubleValue] > 600) {
        [self.swapHistory removeObjectAtIndex:0];
    }
    if (self.swapHistory.count >= 2) {
        self.cachedSwapDelta10MinMiB = [self.swapHistory.lastObject[@"mib"] doubleValue] - [self.swapHistory.firstObject[@"mib"] doubleValue];
    } else {
        self.cachedSwapDelta10MinMiB = 0;
    }
}

- (void)refreshNetworkAtTime:(NSTimeInterval)now {
    struct ifaddrs *interfaces = NULL;
    if (getifaddrs(&interfaces) != 0) return;
    uint64_t totalIn = 0;
    uint64_t totalOut = 0;
    for (struct ifaddrs *cursor = interfaces; cursor; cursor = cursor->ifa_next) {
        if (!cursor->ifa_addr || cursor->ifa_addr->sa_family != AF_LINK || !cursor->ifa_data) continue;
        if (!(cursor->ifa_flags & IFF_UP) || (cursor->ifa_flags & IFF_LOOPBACK)) continue;
        NSString *name = [NSString stringWithUTF8String:cursor->ifa_name ?: ""];
        if (![name hasPrefix:@"en"]) continue;
        struct if_data *data = (struct if_data *)cursor->ifa_data;
        totalIn += data->ifi_ibytes;
        totalOut += data->ifi_obytes;
    }
    freeifaddrs(interfaces);

    if (self.previousNetworkTime > 0 && now > self.previousNetworkTime) {
        NSTimeInterval elapsed = now - self.previousNetworkTime;
        uint64_t deltaIn = totalIn >= self.previousNetworkIn ? totalIn - self.previousNetworkIn : (UINT32_MAX - self.previousNetworkIn) + totalIn + 1;
        uint64_t deltaOut = totalOut >= self.previousNetworkOut ? totalOut - self.previousNetworkOut : (UINT32_MAX - self.previousNetworkOut) + totalOut + 1;
        self.cachedNetworkDownMBps = (double)deltaIn / elapsed / 1048576.0;
        self.cachedNetworkUpMBps = (double)deltaOut / elapsed / 1048576.0;
    }
    self.previousNetworkIn = totalIn;
    self.previousNetworkOut = totalOut;
    self.previousNetworkTime = now;
}

- (void)refreshThermalState {
    switch (NSProcessInfo.processInfo.thermalState) {
        case NSProcessInfoThermalStateFair:
            self.cachedThermalLevel = 1;
            self.cachedThermalText = @"略高";
            break;
        case NSProcessInfoThermalStateSerious:
            self.cachedThermalLevel = 2;
            self.cachedThermalText = @"较高";
            break;
        case NSProcessInfoThermalStateCritical:
            self.cachedThermalLevel = 3;
            self.cachedThermalText = @"严重";
            break;
        default:
            self.cachedThermalLevel = 0;
            self.cachedThermalText = @"正常";
            break;
    }
}

- (NSArray<NativeTopApp *> *)topAppsFromAggregates:(NSDictionary<NSString *, AppAggregate *> *)aggregates byCPU:(BOOL)byCPU {
    NSArray<AppAggregate *> *sorted = [aggregates.allValues sortedArrayUsingComparator:^NSComparisonResult(AppAggregate *left, AppAggregate *right) {
        double leftValue = byCPU ? left.cpuRawPercent : (double)left.physicalBytes;
        double rightValue = byCPU ? right.cpuRawPercent : (double)right.physicalBytes;
        if (leftValue > rightValue) return NSOrderedAscending;
        if (leftValue < rightValue) return NSOrderedDescending;
        return NSOrderedSame;
    }];
    NSMutableArray<NativeTopApp *> *result = [NSMutableArray array];
    NSInteger logicalCPUs = MAX(1, NSProcessInfo.processInfo.processorCount);
    for (AppAggregate *aggregate in sorted) {
        if (result.count >= (byCPU ? 3 : 5)) break;
        NativeTopApp *app = [NativeTopApp new];
        app.name = aggregate.name;
        app.cpuPercent = aggregate.cpuRawPercent / logicalCPUs;
        app.memoryGiB = (double)aggregate.physicalBytes / 1073741824.0;
        app.diskReadMBps = aggregate.diskReadMBps;
        app.diskWriteMBps = aggregate.diskWriteMBps;
        [result addObject:app];
    }
    return result;
}

- (NativeSnapshot *)sample {
    NSTimeInterval now = NSDate.date.timeIntervalSince1970;
    BOOL refreshInventory = self.processInventory.count == 0 || now - self.lastInventoryTime >= 10.0;
    if (refreshInventory) [self refreshInventoryAtTime:now];
    BOOL refreshAllApps = self.cachedTopCPUApps.count == 0 || now - self.lastAllAppsTime >= 20.0;

    NSInteger logicalCPUs = MAX(1, NSProcessInfo.processInfo.processorCount);
    uint64_t codexPhysicalBytes = 0;
    uint64_t codexLargestBytes = 0;
    double codexCPURaw = 0;
    double codexReadMBps = 0;
    double codexWriteMBps = 0;
    NSInteger rendererCount = 0;
    NSInteger helperCount = 0;
    NSMutableDictionary<NSString *, AppAggregate *> *appAggregates = [NSMutableDictionary dictionary];

    NSArray<NSNumber *> *pidsToSample = refreshAllApps ? self.processInventory.allKeys : self.codexPIDs.allObjects;
    for (NSNumber *pidNumber in pidsToSample) {
        ProcessMeta *meta = self.processInventory[pidNumber];
        if (!meta) continue;
        BOOL isCodex = [self.codexPIDs containsObject:pidNumber];
        NSDictionary *usage = [self readUsageForMeta:meta now:now];
        if (!usage) continue;

        double cpuRaw = [usage[@"cpu"] doubleValue];
        uint64_t physical = [usage[@"memory"] unsignedLongLongValue];
        double readRate = [usage[@"readRate"] doubleValue];
        double writeRate = [usage[@"writeRate"] doubleValue];
        if (isCodex) {
            codexCPURaw += cpuRaw;
            codexPhysicalBytes += physical;
            codexLargestBytes = MAX(codexLargestBytes, physical);
            codexReadMBps += readRate;
            codexWriteMBps += writeRate;
            if ([meta.path containsString:@"Codex (Renderer)"]) rendererCount++;
            if ([meta.path containsString:@"/Resources/codex"] || [meta.path containsString:@"/Resources/cua_node/"] || [meta.path containsString:@"codex-code-mode-host"] || [meta.path containsString:@"computer-use"]) helperCount++;
        }

        if (refreshAllApps) {
            NSString *appName = [self applicationNameForMeta:meta codex:isCodex];
            AppAggregate *aggregate = appAggregates[appName];
            if (!aggregate) {
                aggregate = [AppAggregate new];
                aggregate.name = appName;
                appAggregates[appName] = aggregate;
            }
            aggregate.cpuRawPercent += cpuRaw;
            aggregate.physicalBytes += physical;
            aggregate.diskReadMBps += readRate;
            aggregate.diskWriteMBps += writeRate;
        }
    }

    if (refreshAllApps) {
        self.cachedTopCPUApps = [self topAppsFromAggregates:appAggregates byCPU:YES];
        self.cachedTopMemoryApps = [self topAppsFromAggregates:appAggregates byCPU:NO];
        self.lastAllAppsTime = now;
    }

    if (now - self.lastSecondaryTime >= 10.0) {
        [self refreshSwapAtTime:now];
        [self refreshNetworkAtTime:now];
        self.lastSecondaryTime = now;
    }
    if (now - self.lastThermalTime >= 20.0) {
        [self refreshThermalState];
        self.lastThermalTime = now;
    }

    double totalMemoryGiB = (double)NSProcessInfo.processInfo.physicalMemory / 1073741824.0;
    NativeSnapshot *snapshot = [NativeSnapshot new];
    snapshot.timestamp = now;
    snapshot.systemCPUPercent = [self readSystemCPUPercent];
    snapshot.codexCPUCores = codexCPURaw / 100.0;
    snapshot.codexCPUPercent = codexCPURaw / logicalCPUs;
    snapshot.codexMemoryGiB = (double)codexPhysicalBytes / 1073741824.0;
    snapshot.totalMemoryGiB = totalMemoryGiB;
    snapshot.codexMemoryPercent = totalMemoryGiB > 0 ? snapshot.codexMemoryGiB / totalMemoryGiB * 100.0 : 0;
    snapshot.codexProcessCount = self.codexPIDs.count;
    snapshot.codexRendererCount = rendererCount;
    snapshot.codexHelperCount = helperCount;
    snapshot.codexLargestGiB = (double)codexLargestBytes / 1073741824.0;
    snapshot.compressedGiB = [self readCompressedGiB];
    snapshot.swapUsedGiB = self.cachedSwapUsedGiB;
    snapshot.swapDelta10MinMiB = self.cachedSwapDelta10MinMiB;
    snapshot.memoryPressureLevel = self.memoryPressureLevel;
    snapshot.memoryPressureText = self.memoryPressureText ?: @"正常";
    snapshot.thermalLevel = self.cachedThermalLevel;
    snapshot.thermalText = self.cachedThermalText ?: @"正常";
    snapshot.networkDownMBps = self.cachedNetworkDownMBps;
    snapshot.networkUpMBps = self.cachedNetworkUpMBps;
    snapshot.codexDiskReadMBps = codexReadMBps;
    snapshot.codexDiskWriteMBps = codexWriteMBps;
    snapshot.topCPUApps = self.cachedTopCPUApps ?: @[];
    snapshot.topMemoryApps = self.cachedTopMemoryApps ?: @[];
    return snapshot;
}

@end
