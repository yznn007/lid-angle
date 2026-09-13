// lid-angle: read a MacBook's built-in lid-angle HID sensor.
//
// The sensor is discovered by its standard HID Sensor Device Orientation
// usage. Product IDs, report IDs, field offsets and precision are read from
// the report descriptor so newer sensor hubs can be supported without a
// model-specific table.

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDDevice.h>
#include <IOKit/hid/IOHIDDeviceKeys.h>
#include <IOKit/hid/IOHIDManager.h>
#include <errno.h>
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
    SENSOR_USAGE_PAGE = 0x0020,
    SENSOR_ORIENTATION_USAGE = 0x008A,
    SENSOR_TILT_X_USAGE = 0x047F,
    SENSOR_CUSTOM_ANGLE_USAGE = 0x0545,
    MAX_ANGLE_FORMATS = 16,
    MAX_GLOBAL_STACK = 8,
    MAX_REPORT_IDS = 256,
    MAX_REPORT_BYTES = 256,
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
            "自动发现支持 Sensor Device Orientation 的 MacBook 角度传感器。\n"
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

static uint64_t item_unsigned(const UInt8 *bytes, size_t size)
{
    uint64_t value = 0;
    for (size_t i = 0; i < size && i < sizeof(value); ++i) {
        value |= (uint64_t)bytes[i] << (8U * i);
    }
    return value;
}

static int64_t item_signed(const UInt8 *bytes, size_t size)
{
    uint64_t value = item_unsigned(bytes, size);
    if (size == 0 || size > sizeof(value) || size * 8U == 64U) {
        return (int64_t)value;
    }
    uint64_t sign_bit = UINT64_C(1) << (size * 8U - 1U);
    if ((value & sign_bit) != 0) {
        value |= UINT64_MAX << (size * 8U);
    }
    return (int64_t)value;
}

static int32_t unit_exponent_value(uint64_t value)
{
    int32_t exponent = (int32_t)(value & 0x0FU);
    return (exponent & 0x08) != 0 ? exponent - 16 : exponent;
}

static double decimal_scale(int32_t exponent)
{
    if (exponent < -8 || exponent > 8) {
        return 0.0;
    }
    double scale = 1.0;
    if (exponent < 0) {
        for (int32_t i = exponent; i < 0; ++i) scale /= 10.0;
    } else {
        for (int32_t i = 0; i < exponent; ++i) scale *= 10.0;
    }
    return scale;
}

static unsigned bits_required(uint64_t value)
{
    unsigned bits = 1;
    while (value > 1) {
        value >>= 1;
        ++bits;
    }
    return bits;
}

typedef enum {
    REPORT_INPUT = 0,
    REPORT_FEATURE = 1
} DescriptorReportKind;

typedef struct {
    uint8_t report_id;
    unsigned bit_offset;
    unsigned value_bits;
    uint64_t logical_max;
    double scale;
    unsigned decimals;
    int score;
    DescriptorReportKind descriptor_kind;
} AngleFormat;

typedef struct {
    AngleFormat values[MAX_ANGLE_FORMATS];
    size_t count;
} DescriptorFormats;

typedef struct {
    uint32_t usage_page;
    int64_t logical_min;
    int64_t logical_max;
    int64_t physical_min;
    int64_t physical_max;
    uint32_t report_size;
    uint32_t report_count;
    uint32_t report_id;
    uint32_t unit;
    int32_t unit_exponent;
} HidGlobalState;

typedef struct {
    bool has_usage;
    uint32_t usage_page;
    uint32_t usage;
} HidLocalState;

static void descriptor_error(char *reason, size_t reason_size, const char *message)
{
    if (reason_size != 0) {
        (void)snprintf(reason, reason_size, "%s", message);
    }
}

static unsigned format_decimals(double scale)
{
    unsigned decimals = 0;
    while (scale > 0.0 && scale < 1.0 && decimals < 6) {
        scale *= 10.0;
        ++decimals;
    }
    return decimals;
}

static bool append_angle_format(DescriptorFormats *formats,
                                const HidGlobalState *global,
                                uint32_t usage_page,
                                uint32_t usage,
                                uint32_t flags,
                                uint64_t bit_offset,
                                DescriptorReportKind kind)
{
    if (formats->count >= MAX_ANGLE_FORMATS || usage_page != SENSOR_USAGE_PAGE ||
        global->report_id > UINT8_MAX || global->report_size == 0 ||
        global->report_size > 64 || global->report_count != 1 ||
        (flags & 0x03U) != 0x02U || global->logical_min < 0 ||
        global->logical_max <= 0) {
        return false;
    }

    double scale = decimal_scale(global->unit_exponent);
    if (scale == 0.0) {
        return false;
    }
    /* Some Apple descriptors identify the hundredths field with this usage
       but omit Unit Exponent. Its declared range still disambiguates it. */
    if (usage == SENSOR_CUSTOM_ANGLE_USAGE && global->logical_max > 360 &&
        global->unit_exponent == 0) {
        scale = 0.01;
    }
    double max_angle = (double)global->logical_max * scale;
    bool known_usage = usage == SENSOR_TILT_X_USAGE || usage == SENSOR_CUSTOM_ANGLE_USAGE;
    bool sensor_data_usage = usage >= 0x0400U;
    if ((!known_usage && !sensor_data_usage) || max_angle < 90.0 || max_angle > 360.0) {
        return false;
    }

    unsigned required_bits = bits_required((uint64_t)global->logical_max);
    if (required_bits > global->report_size || required_bits > 64 ||
        bit_offset > UINT32_MAX || bit_offset + required_bits > UINT32_MAX) {
        return false;
    }
    AngleFormat format = {
        .report_id = (uint8_t)global->report_id,
        .bit_offset = (unsigned)bit_offset,
        .value_bits = required_bits,
        .logical_max = (uint64_t)global->logical_max,
        .scale = scale,
        .decimals = format_decimals(scale),
        .score = (usage == SENSOR_CUSTOM_ANGLE_USAGE ? 1000 : 0) +
                 (usage == SENSOR_TILT_X_USAGE ? 500 : 0) +
                 (scale < 1.0 ? 100 : 0) + (kind == REPORT_FEATURE ? 10 : 0),
        .descriptor_kind = kind
    };
    for (size_t i = 0; i < formats->count; ++i) {
        const AngleFormat *old = &formats->values[i];
        if (old->report_id == format.report_id && old->bit_offset == format.bit_offset &&
            old->value_bits == format.value_bits && old->scale == format.scale) {
            return true;
        }
    }
    size_t position = formats->count++;
    while (position > 0 && formats->values[position - 1].score < format.score) {
        formats->values[position] = formats->values[position - 1];
        --position;
    }
    formats->values[position] = format;
    return true;
}

static bool parse_report_descriptor(CFDataRef descriptor,
                                    DescriptorFormats *formats,
                                    char *reason,
                                    size_t reason_size)
{
    memset(formats, 0, sizeof(*formats));
    if (descriptor == NULL || CFGetTypeID(descriptor) != CFDataGetTypeID()) {
        descriptor_error(reason, reason_size, "缺少 HID ReportDescriptor");
        return false;
    }
    const UInt8 *data = CFDataGetBytePtr(descriptor);
    size_t length = (size_t)CFDataGetLength(descriptor);
    HidGlobalState global = {
        .usage_page = 0, .logical_min = 0, .logical_max = 0,
        .physical_min = 0, .physical_max = 0, .report_size = 0,
        .report_count = 0, .report_id = 0, .unit = 0, .unit_exponent = 0
    };
    HidGlobalState stack[MAX_GLOBAL_STACK];
    size_t stack_depth = 0;
    HidLocalState local = { .has_usage = false, .usage_page = 0, .usage = 0 };
    uint64_t report_bits[2][MAX_REPORT_IDS] = {{0}};
    size_t offset = 0;
    unsigned collections = 0;

    while (offset < length) {
        UInt8 prefix = data[offset++];
        if (prefix == 0xFE) {
            if (offset + 2 > length) {
                descriptor_error(reason, reason_size, "ReportDescriptor 长项目被截断");
                return false;
            }
            size_t long_size = data[offset++];
            ++offset;
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
        const UInt8 *item = data + offset;
        uint64_t unsigned_value = item_unsigned(item, item_size);
        int64_t signed_value = item_signed(item, item_size);
        unsigned type = (unsigned)((prefix >> 2) & 0x03U);
        unsigned tag = (unsigned)((prefix >> 4) & 0x0FU);
        offset += item_size;

        if (type == 1) { /* Global item. */
            switch (tag) {
            case 0: global.usage_page = (uint32_t)unsigned_value; break;
            case 1: global.logical_min = signed_value; break;
            case 2: global.logical_max = signed_value; break;
            case 3: global.physical_min = signed_value; break;
            case 4: global.physical_max = signed_value; break;
            case 5: global.unit_exponent = unit_exponent_value(unsigned_value); break;
            case 6: global.unit = (uint32_t)unsigned_value; break;
            case 7: global.report_size = (uint32_t)unsigned_value; break;
            case 8: global.report_id = (uint32_t)unsigned_value; break;
            case 9: global.report_count = (uint32_t)unsigned_value; break;
            case 10:
                if (stack_depth >= MAX_GLOBAL_STACK) {
                    descriptor_error(reason, reason_size, "ReportDescriptor Global 堆栈溢出");
                    return false;
                }
                stack[stack_depth++] = global;
                break;
            case 11:
                if (stack_depth == 0) {
                    descriptor_error(reason, reason_size, "ReportDescriptor Global 堆栈下溢");
                    return false;
                }
                global = stack[--stack_depth];
                break;
            default: break;
            }
        } else if (type == 2) { /* Local item. */
            if (tag == 0) {
                local.has_usage = true;
                if (item_size == 4) {
                    local.usage_page = (uint32_t)(unsigned_value >> 16);
                    local.usage = (uint32_t)(unsigned_value & UINT32_C(0xFFFF));
                } else {
                    local.usage_page = global.usage_page;
                    local.usage = (uint32_t)unsigned_value;
                }
            }
        } else if (type == 0) { /* Main item. */
            if (tag == 10) {
                ++collections;
                local.has_usage = false;
            } else if (tag == 12) {
                if (collections == 0) {
                    descriptor_error(reason, reason_size, "ReportDescriptor Collection 层级异常");
                    return false;
                }
                --collections;
                local.has_usage = false;
            } else if (tag == 8 || tag == 11) { /* Input or Feature. */
                DescriptorReportKind kind = tag == 8 ? REPORT_INPUT : REPORT_FEATURE;
                if (global.report_id > UINT8_MAX || global.report_size == 0 ||
                    global.report_count == 0 ||
                    global.report_count > UINT64_MAX / global.report_size) {
                    descriptor_error(reason, reason_size, "ReportDescriptor 报告大小异常");
                    return false;
                }
                uint64_t report_id = global.report_id;
                uint64_t bit_offset = report_bits[kind][report_id];
                if (local.has_usage) {
                    (void)append_angle_format(formats, &global, local.usage_page,
                                              local.usage, (uint32_t)unsigned_value,
                                              bit_offset, kind);
                }
                uint64_t bits = (uint64_t)global.report_size * global.report_count;
                if (UINT64_MAX - report_bits[kind][report_id] < bits) {
                    descriptor_error(reason, reason_size, "ReportDescriptor 报告位数溢出");
                    return false;
                }
                report_bits[kind][report_id] += bits;
                local.has_usage = false;
            } else if (tag == 9) { /* Output consumes no input/feature offset. */
                local.has_usage = false;
            }
        }
    }
    if (collections != 0 || stack_depth != 0) {
        descriptor_error(reason, reason_size, "ReportDescriptor 结构未闭合");
        return false;
    }
    return true;
}

static void default_formats(DescriptorFormats *formats)
{
    memset(formats, 0, sizeof(*formats));
    formats->values[0] = (AngleFormat){
        .report_id = 7, .bit_offset = 0, .value_bits = 16,
        .logical_max = 36000, .scale = 0.01, .decimals = 2,
        .score = 100, .descriptor_kind = REPORT_INPUT
    };
    formats->values[1] = (AngleFormat){
        .report_id = 1, .bit_offset = 0, .value_bits = 9,
        .logical_max = 360, .scale = 1.0, .decimals = 0,
        .score = 50, .descriptor_kind = REPORT_INPUT
    };
    formats->count = 2;
}

typedef struct {
    IOHIDManagerRef manager;
    CFDictionaryRef matching;
    CFDictionaryRef fallback_matching;
    IOHIDDeviceRef device;
    AngleFormat format;
    bool device_open;
    bool manager_open;
    char last_error[192];
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
    if (sensor->device != NULL) CFRelease(sensor->device);
    sensor->device = NULL;
    memset(&sensor->format, 0, sizeof(sensor->format));
}

static bool orientation_device(IOHIDDeviceRef device)
{
    long page = property_number(device, CFSTR(kIOHIDDeviceUsagePageKey));
    long usage = property_number(device, CFSTR(kIOHIDDeviceUsageKey));
    if (page == 0 && usage == 0) {
        page = property_number(device, CFSTR(kIOHIDPrimaryUsagePageKey));
        usage = property_number(device, CFSTR(kIOHIDPrimaryUsageKey));
    }
    return page == SENSOR_USAGE_PAGE && usage == SENSOR_ORIENTATION_USAGE;
}

static CFDictionaryRef create_usage_matching(bool primary)
{
    const void *keys[] = {
        primary ? CFSTR(kIOHIDPrimaryUsagePageKey) : CFSTR(kIOHIDDeviceUsagePageKey),
        primary ? CFSTR(kIOHIDPrimaryUsageKey) : CFSTR(kIOHIDDeviceUsageKey)
    };
    int values[] = { SENSOR_USAGE_PAGE, SENSOR_ORIENTATION_USAGE };
    CFNumberRef numbers[] = {
        CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &values[0]),
        CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &values[1])
    };
    if (numbers[0] == NULL || numbers[1] == NULL) {
        if (numbers[0] != NULL) CFRelease(numbers[0]);
        if (numbers[1] != NULL) CFRelease(numbers[1]);
        return NULL;
    }
    CFDictionaryRef result = CFDictionaryCreate(
        kCFAllocatorDefault, keys, (const void **)numbers, 2,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(numbers[0]);
    CFRelease(numbers[1]);
    return result;
}

static CFSetRef copy_matching_devices(Sensor *sensor)
{
    IOHIDManagerSetDeviceMatching(sensor->manager, sensor->matching);
    CFSetRef devices = IOHIDManagerCopyDevices(sensor->manager);
    if (devices != NULL && CFSetGetCount(devices) != 0) return devices;
    if (devices != NULL) CFRelease(devices);
    IOHIDManagerSetDeviceMatching(sensor->manager, sensor->fallback_matching);
    return IOHIDManagerCopyDevices(sensor->manager);
}

static bool extract_bits(const UInt8 *report,
                         size_t length,
                         const AngleFormat *format,
                         uint64_t *value)
{
    size_t prefix = format->report_id == 0 ? 0 : 1;
    if (format->report_id != 0 && (length == 0 || report[0] != format->report_id)) {
        return false;
    }
    if (format->value_bits == 0 || format->value_bits > 64 ||
        format->bit_offset > SIZE_MAX - (size_t)format->value_bits) {
        return false;
    }
    size_t end_bit = (size_t)format->bit_offset + format->value_bits;
    size_t needed = prefix + (end_bit + 7U) / 8U;
    if (needed < prefix || length < needed) return false;
    uint64_t result = 0;
    for (unsigned bit = 0; bit < format->value_bits; ++bit) {
        size_t absolute_bit = prefix * 8U + (size_t)format->bit_offset + bit;
        if (((report[absolute_bit / 8U] >> (absolute_bit % 8U)) & 1U) != 0) {
            result |= UINT64_C(1) << bit;
        }
    }
    *value = result;
    return true;
}

static bool read_angle_format(IOHIDDeviceRef device,
                              const AngleFormat *format,
                              double *angle)
{
    UInt8 report[MAX_REPORT_BYTES] = {0};
    /* Follow the descriptor first. Apple exposes these fields through
       GetReport(Feature), even when their descriptor item is Input, so the
       other report type remains a compatibility fallback. */
    IOHIDReportType order[] = {
        format->descriptor_kind == REPORT_FEATURE ? kIOHIDReportTypeFeature
                                                   : kIOHIDReportTypeInput,
        format->descriptor_kind == REPORT_FEATURE ? kIOHIDReportTypeInput
                                                   : kIOHIDReportTypeFeature
    };
    for (size_t attempt = 0; attempt < 2; ++attempt) {
        CFIndex length = (CFIndex)sizeof(report);
        IOReturn result = IOHIDDeviceGetReport(
            device, order[attempt], format->report_id, report, &length);
        if (result != kIOReturnSuccess || length <= 0 || length > (CFIndex)sizeof(report)) {
            continue;
        }
        uint64_t raw = 0;
        if (!extract_bits(report, (size_t)length, format, &raw) || raw > format->logical_max) {
            continue;
        }
        double value = (double)raw * format->scale;
        if (value >= 0.0 && value <= 360.0) {
            *angle = value;
            return true;
        }
    }
    return false;
}

static bool attach_device(Sensor *sensor)
{
    detach_device(sensor);
    sensor->last_error[0] = '\0';
    CFSetRef devices = copy_matching_devices(sensor);
    if (devices == NULL) {
        set_error(sensor, "找不到支持 Sensor Device Orientation 的 HID 设备");
        return false;
    }
    CFIndex count = CFSetGetCount(devices);
    if (count <= 0) {
        CFRelease(devices);
        set_error(sensor, "找不到支持 Sensor Device Orientation 的 HID 设备");
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
        if (!orientation_device(candidate)) continue;
        DescriptorFormats formats;
        char descriptor_reason[128] = {0};
        CFDataRef descriptor = (CFDataRef)IOHIDDeviceGetProperty(
            candidate, CFSTR(kIOHIDReportDescriptorKey));
        if (descriptor == NULL) {
            default_formats(&formats);
        } else if (!parse_report_descriptor(descriptor, &formats,
                                            descriptor_reason, sizeof(descriptor_reason))) {
            set_error(sensor, descriptor_reason);
            continue;
        } else if (formats.count == 0) {
            default_formats(&formats);
        }
        IOHIDDeviceRef retained = (IOHIDDeviceRef)CFRetain(candidate);
        IOReturn result = IOHIDDeviceOpen(retained, kIOHIDOptionsTypeNone);
        if (result != kIOReturnSuccess) {
            CFRelease(retained);
            (void)snprintf(sensor->last_error, sizeof(sensor->last_error),
                           "打开 HID 角度设备失败（IOReturn 0x%08x）", result);
            continue;
        }
        for (size_t j = 0; j < formats.count; ++j) {
            double angle = 0.0;
            if (read_angle_format(retained, &formats.values[j], &angle)) {
                sensor->device = retained;
                sensor->device_open = true;
                sensor->format = formats.values[j];
                attached = true;
                break;
            }
        }
        if (attached) break;
        (void)IOHIDDeviceClose(retained, kIOHIDOptionsTypeNone);
        CFRelease(retained);
        set_error(sensor, "HID 角度设备没有可读取的角度报告");
    }
    free(values);
    CFRelease(devices);
    if (!attached && sensor->last_error[0] == '\0') {
        set_error(sensor, "找不到可读取的 HID 角度报告");
    }
    return attached;
}

static bool read_angle(Sensor *sensor, double *angle)
{
    if (read_angle_format(sensor->device, &sensor->format, angle)) return true;
    set_error(sensor, "读取 HID 角度报告失败或数据超出范围");
    return false;
}

static void close_sensor(Sensor *sensor)
{
    detach_device(sensor);
    if (sensor->manager != NULL && sensor->manager_open) {
        (void)IOHIDManagerClose(sensor->manager, kIOHIDOptionsTypeNone);
    }
    sensor->manager_open = false;
    if (sensor->manager != NULL) CFRelease(sensor->manager);
    sensor->manager = NULL;
    if (sensor->matching != NULL) CFRelease(sensor->matching);
    if (sensor->fallback_matching != NULL) CFRelease(sensor->fallback_matching);
    sensor->matching = NULL;
    sensor->fallback_matching = NULL;
}

static int open_sensor(Sensor *sensor)
{
    memset(sensor, 0, sizeof(*sensor));
    sensor->manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (sensor->manager == NULL) {
        set_error(sensor, "创建 HID 管理器失败");
        return 3;
    }
    sensor->matching = create_usage_matching(false);
    sensor->fallback_matching = create_usage_matching(true);
    if (sensor->matching == NULL || sensor->fallback_matching == NULL) {
        set_error(sensor, "创建 HID 匹配条件失败");
        close_sensor(sensor);
        return 3;
    }
    IOHIDManagerSetDeviceMatching(sensor->manager, sensor->matching);
    IOReturn result = IOHIDManagerOpen(sensor->manager, kIOHIDOptionsTypeNone);
    if (result != kIOReturnSuccess) {
        (void)snprintf(sensor->last_error, sizeof(sensor->last_error),
                       "打开 HID 管理器失败（IOReturn 0x%08x）", result);
        close_sensor(sensor);
        return 3;
    }
    sensor->manager_open = true;
    return 0;
}

static bool sleep_interval(unsigned interval_ms)
{
    struct timespec remaining = {
        .tv_sec = (time_t)(interval_ms / 1000U),
        .tv_nsec = (long)(interval_ms % 1000U) * 1000000L
    };
    while (!stop_requested && nanosleep(&remaining, &remaining) != 0) {
        if (errno != EINTR) return false;
    }
    return !stop_requested;
}

static double monotonic_seconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0.0;
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

static bool print_tty_status(double angle, const AngleFormat *format, bool valid)
{
    int result = valid
        ? printf("\r\033[2K%.*f\302\260", (int)format->decimals, angle)
        : printf("\r\033[2K读取失败");
    return result >= 0 && fflush(stdout) == 0;
}

static bool finish_tty(void)
{
    return printf("\r\033[2K\n") >= 0 && fflush(stdout) == 0;
}

static bool print_plain(double angle, const AngleFormat *format)
{
    return printf("%.*f\n", (int)format->decimals, angle) >= 0 && fflush(stdout) == 0;
}

int main(int argc, char **argv)
{
    (void)setlocale(LC_NUMERIC, "C");
    Options options;
    int option_result = parse_options(argc, argv, &options);
    if (option_result == 1) return 0;
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
        if (stop_requested) break;
        if (sensor.device == NULL && !attach_device(&sensor)) {
            if (tty) {
                if (!print_tty_status(0.0, &sensor.format, false)) {
                    exit_code = 4;
                    break;
                }
                had_tty_status = true;
            }
            if (failure_started < 0.0) failure_started = monotonic_seconds();
            if (options.once || monotonic_seconds() - failure_started >= RECONNECT_TIMEOUT_SECONDS) {
                fprintf(stderr, "错误：%s。\n", sensor.last_error);
                exit_code = options.once ? 3 : 4;
                break;
            }
            if (!sleep_interval(RECONNECT_INTERVAL_MS)) break;
            continue;
        }

        double angle = 0.0;
        if (!read_angle(&sensor, &angle)) {
            detach_device(&sensor);
            if (tty) {
                if (!print_tty_status(0.0, &sensor.format, false)) {
                    exit_code = 4;
                    break;
                }
                had_tty_status = true;
            }
            if (failure_started < 0.0) failure_started = monotonic_seconds();
            if (options.once || monotonic_seconds() - failure_started >= RECONNECT_TIMEOUT_SECONDS) {
                fprintf(stderr, "错误：%s。\n", sensor.last_error);
                exit_code = 4;
                break;
            }
        } else {
            failure_started = -1.0;
            if (options.once) {
                if (!print_plain(angle, &sensor.format)) {
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
                if (!print_tty_status(angle, &sensor.format, true)) {
                    exit_code = 4;
                    break;
                }
                had_tty_status = true;
            } else if (!print_plain(angle, &sensor.format)) {
                int write_errno = errno;
                if (write_errno != EPIPE) {
                    fprintf(stderr, "错误：写入 stdout 失败：%s。\n", strerror(write_errno));
                    exit_code = 4;
                }
                break;
            }
        }
        if (!sleep_interval(sensor.device == NULL ? RECONNECT_INTERVAL_MS : options.interval_ms)) {
            break;
        }
    }
    if (tty && had_tty_status && !finish_tty() && exit_code == 0) exit_code = 4;
    close_sensor(&sensor);
    return exit_code;
}
