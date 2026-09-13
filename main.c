// lid-angle: read Apple's built-in lid-angle HID sensor.
//
// The HID report layout is validated at runtime against the device's report
// descriptor before a report is interpreted. Only report reads are issued.

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDDevice.h>
#include <IOKit/hid/IOHIDDeviceKeys.h>
#include <IOKit/hid/IOHIDManager.h>
#include <errno.h>
#include <inttypes.h>
#include <locale.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum {
    APPLE_VENDOR_ID = 0x05AC,
    LID_ANGLE_PRODUCT_ID = 0x8104,
    LID_ANGLE_USAGE_PAGE = 0x0020,
    LID_ANGLE_USAGE = 0x008A,
    LID_ANGLE_REPORT_ID = 1,
    LID_ANGLE_REPORT_BYTES = 3,
    MAX_REPORT_BYTES = 64,
    DEFAULT_INTERVAL_MS = 100,
    MIN_INTERVAL_MS = 20,
    MAX_INTERVAL_MS = 60000,
    RECONNECT_TIMEOUT_SECONDS = 5,
    RECONNECT_INTERVAL_MS = 500
};

static volatile sig_atomic_t stop_requested = 0;

static void on_signal(int signo)
{
    (void)signo;
    stop_requested = 1;
}

static void usage(FILE *stream)
{
    fprintf(stream,
            "用法：lid-angle [--once] [--interval 毫秒]\n"
            "\n"
            "读取 MacBook 屏幕相对键盘底座的开合角度。\n"
            "  --once             读取一次，输出纯数字后退出\n"
            "  --interval N       采样间隔，20 到 60000 毫秒（默认 100）\n"
            "  --help             显示帮助\n");
}

static bool parse_interval(const char *text, unsigned *value)
{
    uint64_t parsed = 0;

    if (text == NULL || text[0] == '\0') {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        parsed = parsed * 10U + (uint64_t)(*p - '0');
        if (parsed > MAX_INTERVAL_MS) {
            return false;
        }
    }
    if (parsed < MIN_INTERVAL_MS) {
        return false;
    }
    *value = (unsigned)parsed;
    return true;
}

typedef struct {
    bool once;
    unsigned interval_ms;
} Options;

static int parse_options(int argc, char **argv, Options *options)
{
    options->once = false;
    options->interval_ms = DEFAULT_INTERVAL_MS;
    bool interval_seen = false;

    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (strcmp(arg, "--help") == 0) {
            usage(stdout);
            return 1;
        }
        if (strcmp(arg, "--once") == 0) {
            if (options->once) {
                fprintf(stderr, "错误：--once 只能指定一次。\n");
                return -1;
            }
            options->once = true;
            continue;
        }
        if (strcmp(arg, "--interval") == 0) {
            if (interval_seen) {
                fprintf(stderr, "错误：--interval 只能指定一次。\n");
                return -1;
            }
            if (i + 1 >= argc) {
                fprintf(stderr, "错误：--interval 缺少毫秒数。\n");
                return -1;
            }
            const char *text = argv[++i];
            if (!parse_interval(text, &options->interval_ms)) {
                fprintf(stderr, "错误：间隔必须是 20 到 60000 之间的十进制整数毫秒。\n");
                return -1;
            }
            interval_seen = true;
            continue;
        }
        fprintf(stderr, "错误：未知参数或位置参数：%s\n", arg);
        return -1;
    }
    return 0;
}

static long property_number(IOHIDDeviceRef device, CFStringRef key)
{
    CFTypeRef property = IOHIDDeviceGetProperty(device, key);
    long result = 0;
    if (property != NULL && CFGetTypeID(property) == CFNumberGetTypeID()) {
        (void)CFNumberGetValue((CFNumberRef)property, kCFNumberLongType, &result);
    }
    return result;
}

static uint32_t item_value(const UInt8 *bytes, size_t size)
{
    uint32_t value = 0;
    for (size_t i = 0; i < size && i < sizeof(value); ++i) {
        value |= (uint32_t)bytes[i] << (8U * i);
    }
    return value;
}

static void descriptor_error(char *reason, size_t reason_size, const char *message)
{
    if (reason_size != 0) {
        (void)snprintf(reason, reason_size, "%s", message);
    }
}

/*
 * Validate the relevant HID short items.  The current descriptor contains
 * several other sensor fields, so validation is tied to the report-1 field:
 * Usage Page 0x20, Usage 0x047f, one 9-bit variable input, logical and
 * physical 0..360, with no unit scaling on this field.
 */
static bool valid_report_descriptor(CFDataRef descriptor, char *reason, size_t reason_size)
{
    if (descriptor == NULL || CFGetTypeID(descriptor) != CFDataGetTypeID()) {
        descriptor_error(reason, reason_size, "缺少 HID ReportDescriptor");
        return false;
    }
    const UInt8 *data = CFDataGetBytePtr(descriptor);
    size_t length = (size_t)CFDataGetLength(descriptor);
    uint32_t usage_page = 0;
    uint32_t usage = 0;
    uint32_t report_size = 0;
    uint32_t report_count = 0;
    uint32_t report_id = 0;
    uint32_t logical_min = 0;
    uint32_t logical_max = 0;
    uint32_t physical_min = 0;
    uint32_t physical_max = 0;
    uint32_t unit = 0;
    int32_t unit_exponent = 0;
    bool top_page = false;
    bool top_usage = false;
    bool field = false;
    bool report1_malformed = false;
    uint64_t report1_bits = 0;
    unsigned collection_depth = 0;
    size_t offset = 0;

    while (offset < length) {
        UInt8 prefix = data[offset++];
        if (prefix == 0xFE) {
            if (offset + 2 > length) {
                descriptor_error(reason, reason_size, "ReportDescriptor 长项目被截断");
                return false;
            }
            size_t long_size = data[offset++];
            (void)data[offset++];
            if (offset + long_size > length) {
                descriptor_error(reason, reason_size, "ReportDescriptor 长项目超出长度");
                return false;
            }
            offset += long_size;
            continue;
        }

        size_t size_code = (size_t)(prefix & 0x03U);
        size_t item_size = size_code == 3 ? 4 : size_code;
        if (offset + item_size > length) {
            descriptor_error(reason, reason_size, "ReportDescriptor 项目被截断");
            return false;
        }
        uint32_t value = item_value(data + offset, item_size);
        unsigned type = (unsigned)((prefix >> 2) & 0x03U);
        unsigned tag = (unsigned)((prefix >> 4) & 0x0FU);
        offset += item_size;

        if (type == 1) { /* Global */
            switch (tag) {
            case 0: usage_page = value; break;
            case 1: logical_min = value; break;
            case 2: logical_max = value; break;
            case 3: physical_min = value; break;
            case 4: physical_max = value; break;
            case 5:
                unit_exponent = (int32_t)(value & 0x0FU);
                if ((unit_exponent & 0x08) != 0) unit_exponent -= 16;
                if (report_id == LID_ANGLE_REPORT_ID && !field) report1_malformed = true;
                break;
            case 6:
                unit = value;
                if (report_id == LID_ANGLE_REPORT_ID && !field) report1_malformed = true;
                break;
            case 7: report_size = value; break;
            case 8: report_id = value; break;
            case 9: report_count = value; break;
            case 10: /* Push */
            case 11: /* Pop */
                report1_malformed = true;
                break;
            default: break;
            }
        } else if (type == 2) { /* Local */
            if (tag == 0) {
                usage = value;
            }
        } else if (type == 0) { /* Main */
            switch (tag) {
            case 10: /* Collection */
                if (collection_depth == 0 && usage_page == LID_ANGLE_USAGE_PAGE &&
                    usage == LID_ANGLE_USAGE) {
                    top_page = true;
                    top_usage = true;
                }
                ++collection_depth;
                break;
            case 12: /* End Collection */
                if (collection_depth == 0) {
                    descriptor_error(reason, reason_size, "ReportDescriptor Collection 层级异常");
                    return false;
                }
                --collection_depth;
                break;
            case 8: /* Input */
                if (report_id == LID_ANGLE_REPORT_ID && field) {
                    report1_malformed = true;
                } else if (report_id == LID_ANGLE_REPORT_ID && usage_page == LID_ANGLE_USAGE_PAGE &&
                    usage == 0x047FU && report_size == 9 && report_count == 1 &&
                    logical_min == 0 && logical_max == 360 && physical_min == 0 &&
                    physical_max == 360 && unit == 0 && unit_exponent == 0 && value == 0x02U &&
                    report1_bits == 0) {
                    field = true;
                } else if (report_id == LID_ANGLE_REPORT_ID) {
                    report1_malformed = true;
                }
                if (report_id == LID_ANGLE_REPORT_ID && report_size != 0 &&
                    report_count != 0 && report_size <= UINT64_MAX / report_count) {
                    report1_bits += (uint64_t)report_size * report_count;
                }
                usage = 0;
                break;
            case 9: /* Output */
            case 11: /* Feature */
                if (report_id == LID_ANGLE_REPORT_ID) {
                    report1_malformed = true;
                }
                break;
            default:
                break;
            }
        }
    }
    if (collection_depth != 0) {
        descriptor_error(reason, reason_size, "ReportDescriptor Collection 未闭合");
        return false;
    }
    if (!top_page || !top_usage || !field || report1_malformed || report1_bits != 9) {
        descriptor_error(reason, reason_size, "ReportDescriptor 不符合 lid-angle Report 1 格式");
        return false;
    }
    return true;
}

typedef struct {
    IOHIDManagerRef manager;
    CFDictionaryRef matching;
    IOHIDDeviceRef device;
    bool device_open;
    char last_error[160];
} Sensor;

static void set_error(Sensor *sensor, const char *message)
{
    (void)snprintf(sensor->last_error, sizeof(sensor->last_error), "%s", message);
}

static void detach_device(Sensor *sensor)
{
    if (sensor->device != NULL && sensor->device_open) {
        (void)IOHIDDeviceClose(sensor->device, kIOHIDOptionsTypeNone);
    }
    sensor->device_open = false;
    if (sensor->device != NULL) {
        CFRelease(sensor->device);
    }
    sensor->device = NULL;
}

static bool attach_device(Sensor *sensor)
{
    detach_device(sensor);
    sensor->last_error[0] = '\0';
    /* Refresh enumeration on reconnect; this synchronous CLI has no HID run loop. */
    IOHIDManagerSetDeviceMatching(sensor->manager, sensor->matching);
    CFSetRef devices = IOHIDManagerCopyDevices(sensor->manager);
    if (devices == NULL) {
        set_error(sensor, "找不到 Apple lid-angle HID 设备");
        return false;
    }
    CFIndex count = CFSetGetCount(devices);
    if (count == 0) {
        CFRelease(devices);
        set_error(sensor, "找不到 Apple lid-angle HID 设备");
        return false;
    }
    IOHIDDeviceRef *values = calloc((size_t)count, sizeof(*values));
    if (values == NULL) {
        CFRelease(devices);
        set_error(sensor, "分配 HID 设备列表失败");
        return false;
    }
    CFSetGetValues(devices, (const void **)values);
    bool attached = false;
    for (CFIndex i = 0; i < count; ++i) {
        IOHIDDeviceRef candidate = values[i];
        if (property_number(candidate, CFSTR(kIOHIDVendorIDKey)) != APPLE_VENDOR_ID ||
            property_number(candidate, CFSTR(kIOHIDProductIDKey)) != LID_ANGLE_PRODUCT_ID ||
            property_number(candidate, CFSTR(kIOHIDPrimaryUsagePageKey)) != LID_ANGLE_USAGE_PAGE ||
            property_number(candidate, CFSTR(kIOHIDPrimaryUsageKey)) != LID_ANGLE_USAGE) {
            continue;
        }
        char descriptor_reason[128] = {0};
        CFDataRef descriptor = (CFDataRef)IOHIDDeviceGetProperty(
            candidate, CFSTR(kIOHIDReportDescriptorKey));
        if (!valid_report_descriptor(descriptor, descriptor_reason, sizeof(descriptor_reason))) {
            set_error(sensor, descriptor_reason);
            continue;
        }
        IOHIDDeviceRef retained = (IOHIDDeviceRef)CFRetain(candidate);
        IOReturn result = IOHIDDeviceOpen(retained, kIOHIDOptionsTypeNone);
        if (result != kIOReturnSuccess) {
            CFRelease(retained);
            (void)snprintf(sensor->last_error, sizeof(sensor->last_error),
                           "打开 lid-angle HID 设备失败（IOReturn 0x%08x）", result);
            continue;
        }
        sensor->device = retained;
        sensor->device_open = true;
        attached = true;
        break;
    }
    free(values);
    CFRelease(devices);
    if (!attached && sensor->last_error[0] == '\0') {
        set_error(sensor, "找不到匹配的 Apple lid-angle HID 设备");
    }
    return attached;
}

static bool read_angle(Sensor *sensor, unsigned *angle)
{
    UInt8 report[MAX_REPORT_BYTES] = {0};
    CFIndex length = (CFIndex)sizeof(report);
    IOReturn result = IOHIDDeviceGetReport(sensor->device, kIOHIDReportTypeFeature,
                                           LID_ANGLE_REPORT_ID, report, &length);
    if (result != kIOReturnSuccess) {
        (void)snprintf(sensor->last_error, sizeof(sensor->last_error),
                       "读取 lid-angle HID 报告失败（IOReturn 0x%08x）", result);
        return false;
    }
    if (length != LID_ANGLE_REPORT_BYTES || report[0] != LID_ANGLE_REPORT_ID) {
        set_error(sensor, "HID 报告长度或 Report ID 异常");
        return false;
    }
    uint16_t raw = (uint16_t)report[1] | (uint16_t)((uint16_t)report[2] << 8);
    if ((raw & (uint16_t)~0x01FFU) != 0 || raw > 360U) {
        set_error(sensor, "HID 角度字段超出 0 到 360 度范围");
        return false;
    }
    *angle = raw;
    return true;
}

static void close_sensor(Sensor *sensor)
{
    detach_device(sensor);
    if (sensor->manager != NULL) {
        CFRelease(sensor->manager);
    }
    sensor->manager = NULL;
    if (sensor->matching != NULL) {
        CFRelease(sensor->matching);
    }
    sensor->matching = NULL;
}

static int open_sensor(Sensor *sensor)
{
    memset(sensor, 0, sizeof(*sensor));
    sensor->manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (sensor->manager == NULL) {
        set_error(sensor, "创建 HID 管理器失败");
        return 3;
    }
    /* Use an exact match so opening this utility does not enumerate all HID devices. */
    const void *keys[] = {
        CFSTR(kIOHIDVendorIDKey), CFSTR(kIOHIDProductIDKey),
        CFSTR(kIOHIDPrimaryUsagePageKey), CFSTR(kIOHIDPrimaryUsageKey)
    };
    int values[] = { APPLE_VENDOR_ID, LID_ANGLE_PRODUCT_ID,
                     LID_ANGLE_USAGE_PAGE, LID_ANGLE_USAGE };
    CFNumberRef numbers[4] = {0};
    for (size_t i = 0; i < 4; ++i) {
        numbers[i] = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &values[i]);
        if (numbers[i] == NULL) {
            for (size_t j = 0; j < i; ++j) CFRelease(numbers[j]);
            set_error(sensor, "创建 HID 匹配条件失败");
            close_sensor(sensor);
            return 3;
        }
    }
    CFDictionaryRef matching = CFDictionaryCreate(
        kCFAllocatorDefault, keys, (const void **)numbers, 4,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    for (size_t i = 0; i < 4; ++i) CFRelease(numbers[i]);
    if (matching == NULL) {
        set_error(sensor, "创建 HID 匹配条件失败");
        close_sensor(sensor);
        return 3;
    }
    sensor->matching = matching;
    return 0;
}

static bool sleep_interval(unsigned interval_ms)
{
    struct timespec remaining = {
        .tv_sec = (time_t)(interval_ms / 1000U),
        .tv_nsec = (long)(interval_ms % 1000U) * 1000000L
    };
    while (!stop_requested && nanosleep(&remaining, &remaining) != 0) {
        if (errno != EINTR) {
            return false;
        }
    }
    return !stop_requested;
}

static double monotonic_seconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0.0;
    }
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

static bool tty_status(double angle, bool valid)
{
    int result;
    if (valid) {
        result = printf("\r\033[2K%.1f\302\260", angle);
    } else {
        result = printf("\r\033[2K读取失败");
    }
    return result >= 0 && fflush(stdout) == 0;
}

static bool tty_finish(void)
{
    return printf("\r\033[2K\n") >= 0 && fflush(stdout) == 0;
}

int main(int argc, char **argv)
{
    (void)setlocale(LC_NUMERIC, "C");
    Options options;
    int option_result = parse_options(argc, argv, &options);
    if (option_result == 1) {
        return 0;
    }
    if (option_result < 0) {
        usage(stderr);
        return 2;
    }

    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    (void)sigaction(SIGINT, &action, NULL);
    (void)sigaction(SIGTERM, &action, NULL);
    (void)signal(SIGPIPE, SIG_IGN);

    Sensor sensor;
    int open_result = open_sensor(&sensor);
    if (open_result != 0) {
        fprintf(stderr, "错误：%s。\n", sensor.last_error);
        close_sensor(&sensor);
        return open_result;
    }

    bool tty = !options.once && isatty(STDOUT_FILENO) != 0;
    bool had_tty_status = false;
    double failure_started = -1.0;
    int exit_code = 0;
    for (;;) {
        if (stop_requested) {
            break;
        }
        if (sensor.device == NULL && !attach_device(&sensor)) {
            if (tty) {
                if (!tty_status(0, false)) {
                    exit_code = 4;
                    break;
                }
                had_tty_status = true;
            }
            if (failure_started < 0.0) {
                failure_started = monotonic_seconds();
            }
            if (options.once || monotonic_seconds() - failure_started >= RECONNECT_TIMEOUT_SECONDS) {
                fprintf(stderr, "错误：%s。\n", sensor.last_error);
                exit_code = options.once ? 3 : 4;
                break;
            }
            if (!sleep_interval(RECONNECT_INTERVAL_MS)) {
                break;
            }
            continue;
        }

        unsigned angle = 0;
        if (!read_angle(&sensor, &angle)) {
            detach_device(&sensor);
            if (tty) {
                if (!tty_status(0, false)) {
                    exit_code = 4;
                    break;
                }
                had_tty_status = true;
            }
            if (failure_started < 0.0) {
                failure_started = monotonic_seconds();
            }
            if (options.once || monotonic_seconds() - failure_started >= RECONNECT_TIMEOUT_SECONDS) {
                fprintf(stderr, "错误：%s。\n", sensor.last_error);
                exit_code = 4;
                break;
            }
        } else {
            failure_started = -1.0;
            if (options.once) {
                if (printf("%u\n", angle) < 0 || fflush(stdout) != 0) {
                    int write_errno = errno;
                    if (write_errno != EPIPE) {
                        fprintf(stderr, "错误：写入 stdout 失败：%s。\n", strerror(write_errno));
                        exit_code = 4;
                    }
                    break;
                }
                break;
            }
            if (tty) {
                if (!tty_status((double)angle, true)) {
                    exit_code = 4;
                    break;
                }
                had_tty_status = true;
            } else {
                if (printf("%u\n", angle) < 0 || fflush(stdout) != 0) {
                    int write_errno = errno;
                    if (write_errno != EPIPE) {
                        fprintf(stderr, "错误：写入 stdout 失败：%s。\n", strerror(write_errno));
                        exit_code = 4;
                    }
                    break;
                }
            }
        }
        if (!sleep_interval(sensor.device == NULL ? RECONNECT_INTERVAL_MS : options.interval_ms)) {
            break;
        }
    }
    if (tty && had_tty_status) {
        if (!tty_finish() && exit_code == 0) {
            exit_code = 4;
        }
    }
    close_sensor(&sensor);
    return exit_code;
}
