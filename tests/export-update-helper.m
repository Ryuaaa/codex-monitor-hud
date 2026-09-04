#import <Foundation/Foundation.h>
#import "UpdateManager.h"

// Export the unchanged production helper for an explicitly supervised upgrade test.
// This command only creates a script; it never runs it or alters an application.
int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc != 2) return 2;
        NSString *path = [NSString stringWithUTF8String:argv[1]];
        if ([NSFileManager.defaultManager fileExistsAtPath:path]) return 3;
        NSError *error = nil;
        if (![HUDInstallHelperScript() writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:&error]) return 4;
        return [NSFileManager.defaultManager setAttributes:@{NSFilePosixPermissions: @0700} ofItemAtPath:path error:&error] ? 0 : 5;
    }
}
