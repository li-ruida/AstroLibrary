#include "core.hpp"
#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
@interface AstroNoRedirect : NSObject <NSURLSessionTaskDelegate>
@end
@implementation AstroNoRedirect
- (void)URLSession:(NSURLSession *)session
                          task:(NSURLSessionTask *)task
    willPerformHTTPRedirection:(NSHTTPURLResponse *)response
                    newRequest:(NSURLRequest *)request
             completionHandler:(void (^)(NSURLRequest *))completionHandler {
    completionHandler(nil);
}
@end
namespace astro {
static NSString *ns(const std::string &s) {
    return [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding];
}
void platformTrash(const fs::path &path) {
    @autoreleasepool {
        NSError *error = nil;
        BOOL ok = [[NSFileManager defaultManager] trashItemAtURL:[NSURL fileURLWithPath:ns(path.string())]
                                                resultingItemURL:nil
                                                           error:&error];
        require(ok, error ? std::string(error.localizedDescription.UTF8String) : "移到废纸篓失败");
    }
}
void platformBrowse(const std::string &url) {
    @autoreleasepool {
        require([[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:ns(url)]], "无法打开浏览器");
    }
}
void platformOpen(const fs::path &path) {
    @autoreleasepool {
        require([[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:ns(path.string())]],
                "无法在访达中打开目录");
    }
}
std::string platformChooseDirectory() {
    @autoreleasepool {
        NSTask *task = [NSTask new];
        task.executableURL = [NSURL fileURLWithPath:@"/usr/bin/osascript"];
        task.arguments = @[ @"-e", @"POSIX path of (choose folder with prompt \"选择 AstroLibrary 目录\")" ];
        NSPipe *pipe = [NSPipe pipe];
        task.standardOutput = pipe;
        task.standardError = [NSFileHandle fileHandleWithNullDevice];
        NSError *error = nil;
        require([task launchAndReturnError:&error], "无法启动目录选择器");
        NSData *data = [pipe.fileHandleForReading readDataToEndOfFile];
        [task waitUntilExit];
        require(task.terminationStatus == 0, "已取消目录选择");
        NSString *result = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
        require(result != nil, "目录路径无效");
        return trim(result.UTF8String);
    }
}
J platformHTTPS(const std::string &method, const std::string &url, const J &headers, const Bytes &body) {
    @autoreleasepool {
        NSURL *u = [NSURL URLWithString:ns(url)];
        require(u && [u.scheme isEqualToString:@"https"], "仅允许 HTTPS 请求");
        NSMutableURLRequest *req = [NSMutableURLRequest requestWithURL:u];
        req.HTTPMethod = ns(method);
        req.timeoutInterval = 30;
        for (auto it = headers.begin(); it != headers.end(); ++it)
            [req setValue:ns(it.value().get<std::string>()) forHTTPHeaderField:ns(it.key())];
        if (!body.empty())
            req.HTTPBody = [NSData dataWithBytes:body.data() length:body.size()];
        NSURLSessionConfiguration *config = [NSURLSessionConfiguration ephemeralSessionConfiguration];
        config.timeoutIntervalForRequest = 30;
        config.timeoutIntervalForResource = 35;
        config.HTTPCookieStorage = nil;
        config.URLCache = nil;
        AstroNoRedirect *delegate = [AstroNoRedirect new];
        NSURLSession *session = [NSURLSession sessionWithConfiguration:config
                                                              delegate:delegate
                                                         delegateQueue:nil];
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        __block NSData *result = nil;
        __block NSHTTPURLResponse *response = nil;
        __block NSError *failure = nil;
        NSURLSessionDataTask *task =
            [session dataTaskWithRequest:req
                       completionHandler:^(NSData *data, NSURLResponse *r, NSError *error) {
                         result = data;
                         response = (NSHTTPURLResponse *)r;
                         failure = error;
                         dispatch_semaphore_signal(done);
                       }];
        [task resume];
        bool timedOut =
            dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 40 * NSEC_PER_SEC)) != 0;
        if (timedOut)
            [task cancel];
        [session finishTasksAndInvalidate];
        require(!timedOut, "R2 请求超时");
        require(!failure, failure ? std::string(failure.localizedDescription.UTF8String) : "R2 请求失败");
        J h = J::object();
        for (NSString *key in response.allHeaderFields) {
            id value = response.allHeaderFields[key];
            h[lower(key.UTF8String)] = [[value description] UTF8String];
        }
        std::string data = result ? Bytes((const char *)result.bytes, result.length) : "";
        return {{"status", response.statusCode}, {"headers", h}, {"body", data}};
    }
}
Bytes platformJPEG(const fs::path &source) {
    @autoreleasepool {
        CGImageSourceRef input = CGImageSourceCreateWithURL(
            (__bridge CFURLRef)[NSURL fileURLWithPath:ns(source.string())], nullptr);
        require(input != nullptr, "无法读取预览图");
        NSDictionary *options = @{
            (id)kCGImageSourceCreateThumbnailFromImageAlways : @YES,
            (id)kCGImageSourceCreateThumbnailWithTransform : @YES,
            (id)kCGImageSourceThumbnailMaxPixelSize : @1600
        };
        CGImageRef thumbnail =
            CGImageSourceCreateThumbnailAtIndex(input, 0, (__bridge CFDictionaryRef)options);
        CFRelease(input);
        require(thumbnail != nullptr, "无法生成 JPEG 缩略图");
        CGColorSpaceRef color = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef context =
            CGBitmapContextCreate(nullptr, CGImageGetWidth(thumbnail), CGImageGetHeight(thumbnail), 8, 0,
                                  color, kCGImageAlphaNoneSkipLast);
        CGColorSpaceRelease(color);
        if (!context) {
            CGImageRelease(thumbnail);
            throw std::runtime_error("无法创建缩略图缓冲区");
        }
        CGContextDrawImage(context, CGRectMake(0, 0, CGImageGetWidth(thumbnail), CGImageGetHeight(thumbnail)),
                           thumbnail);
        CGImageRef image = CGBitmapContextCreateImage(context);
        CGContextRelease(context);
        CGImageRelease(thumbnail);
        NSMutableData *out = [NSMutableData data];
        CGImageDestinationRef dest = CGImageDestinationCreateWithData((__bridge CFMutableDataRef)out,
                                                                      CFSTR("public.jpeg"), 1, nullptr);
        if (!dest) {
            CGImageRelease(image);
            throw std::runtime_error("无法创建 JPEG 编码器");
        }
        CGImageDestinationAddImage(
            dest, image,
            (__bridge CFDictionaryRef)
                @{(id)kCGImageDestinationLossyCompressionQuality : @0.82});
        bool ok = CGImageDestinationFinalize(dest);
        CFRelease(dest);
        CGImageRelease(image);
        require(ok, "JPEG 编码失败");
        Bytes data((const char *)out.bytes, out.length), clean = data.substr(0, 2);
        size_t pos = 2;
        while (pos < data.size()) {
            size_t start = pos;
            require((unsigned char)data[pos] == 255, "JPEG 无效");
            while (pos < data.size() && (unsigned char)data[pos] == 255)
                pos++;
            require(pos < data.size(), "JPEG 不完整");
            unsigned char marker = data[pos++];
            if (marker == 0xda || marker == 0xd9) {
                clean += data.substr(start);
                return clean;
            }
            if (marker == 1 || (marker >= 0xd0 && marker <= 0xd7)) {
                clean += data.substr(start, pos - start);
                continue;
            }
            require(pos + 2 <= data.size(), "JPEG 段长度缺失");
            size_t n = (unsigned char)data[pos] * 256 + (unsigned char)data[pos + 1];
            require(n >= 2 && pos + n <= data.size(), "JPEG 段长度无效");
            if (!((marker >= 0xe1 && marker <= 0xed) || marker == 0xef || marker == 0xfe))
                clean += data.substr(start, pos + n - start);
            pos += n;
        }
        throw std::runtime_error("JPEG 缺少像素数据");
    }
}
} // namespace astro
