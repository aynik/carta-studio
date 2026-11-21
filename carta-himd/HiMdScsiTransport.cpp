/**
 * @file HiMdScsiTransport.cpp
 * Cross-platform native SCSI pass-through + USB Mass Storage Bulk-Only
 * Transport (`libusb-1.0`) for physical Sony Hi-MD devices.
 */

#include "HiMdScsiTransport.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#include <ntddscsi.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <malloc.h>
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <DiskArbitration/DiskArbitration.h>
#include <IOKit/IOBSD.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/scsi/SCSITaskLib.h>
#include <IOKit/storage/IOMedia.h>
#include <mach/mach_port.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <scsi/sg.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#endif

#if __has_include(<libusb-1.0/libusb.h>)
#include <libusb-1.0/libusb.h>
#else
#include <libusb.h>
#endif

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <utility>

namespace carta {
namespace {

constexpr KnownHiMdProduct kKnownHiMdProducts[] = {
    {0x5341u, 0x5256u, "Exploit-Unrestricted"},
    {0x054cu, 0x017fu, "Sony MZ-NH1"},
    {0x054cu, 0x0181u, "Sony MZ-NH3D"},
    {0x054cu, 0x0183u, "Sony MZ-NH900"},
    {0x054cu, 0x0185u, "Sony MZ-NH700"},
    {0x054cu, 0x0187u, "Sony MZ-NH600"},
    {0x054cu, 0x018bu, "Sony LAM"},
    {0x054cu, 0x01eau, "Sony MZ-DH10P"},
    {0x054cu, 0x021au, "Sony MZ-RH10"},
    {0x054cu, 0x021cu, "Sony MZ-RH910"},
    {0x054cu, 0x022du, "Sony CMT-AH10"},
    {0x054cu, 0x023du, "Sony DS-HMD1"},
    {0x054cu, 0x0287u, "Sony MZ-RH1"},
    {0x0411u, 0x007fu, "Buffalo MD-HUSB"},
};

constexpr std::uint32_t kCbwSignature = 0x43425355u; // 'USBC'
constexpr std::uint32_t kCswSignature = 0x53425355u; // 'USBS'
constexpr int kMaxUnitReadyRetries = 6;
constexpr int kUsbTimeoutMs = 30000;

std::uint16_t readBe16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>((static_cast<unsigned>(p[0]) << 8) | p[1]);
}

std::uint32_t readBe32(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

void writeBe16(std::uint8_t* p, std::uint16_t v) noexcept {
    p[0] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
    p[1] = static_cast<std::uint8_t>(v & 0xFFu);
}

void writeBe32(std::uint8_t* p, std::uint32_t v) noexcept {
    p[0] = static_cast<std::uint8_t>((v >> 24) & 0xFFu);
    p[1] = static_cast<std::uint8_t>((v >> 16) & 0xFFu);
    p[2] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
    p[3] = static_cast<std::uint8_t>(v & 0xFFu);
}

std::uint32_t readLe32(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

void writeLe32(std::uint8_t* p, std::uint32_t v) noexcept {
    p[0] = static_cast<std::uint8_t>(v & 0xFFu);
    p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
    p[2] = static_cast<std::uint8_t>((v >> 16) & 0xFFu);
    p[3] = static_cast<std::uint8_t>((v >> 24) & 0xFFu);
}

bool bytesAllZero(const std::uint8_t* p, int start, int end) noexcept {
    for (int i = start; i < end; ++i) {
        if (p[i] != 0) {
            return false;
        }
    }
    return true;
}

std::string formatUsbDeviceId(int bus, int addr) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "usb:%03d:%03d", bus, addr);
    return std::string(buf);
}

bool parseUsbDeviceId(const std::string& id, int& outBus, int& outAddr) noexcept {
    if (id.rfind("usb:", 0) != 0) {
        return false;
    }
    unsigned b = 0;
    unsigned a = 0;
    if (std::sscanf(id.c_str(), "usb:%u:%u", &b, &a) != 2) {
        return false;
    }
    outBus = static_cast<int>(b);
    outAddr = static_cast<int>(a);
    return true;
}

struct ScsiBackend {
    virtual ~ScsiBackend() = default;
    virtual int reset(std::string& error) = 0;
    virtual int execute(const std::uint8_t* cdb, int cdbLen, int direction,
                        const std::uint8_t* dataIn, int dataInLen, std::uint8_t* dataOut,
                        int dataOutMaxLen, std::string& error) = 0;
};

// ============================================================================
// Shared libusb-1.0 USB Mass Storage BOT Backend
// ============================================================================

struct LibusbEndpointPair {
    std::uint8_t interfaceNumber = 0;
    std::uint8_t bulkInEp = 0;
    std::uint8_t bulkOutEp = 0;
    std::uint8_t maxLun = 0;
};

bool findBotInterface(libusb_device* dev, LibusbEndpointPair& outPair) noexcept {
    libusb_config_descriptor* cfg = nullptr;
    if (libusb_get_active_config_descriptor(dev, &cfg) != 0 || !cfg) {
        if (libusb_get_config_descriptor(dev, 0, &cfg) != 0 || !cfg) {
            return false;
        }
    }
    bool found = false;
    for (std::uint8_t i = 0; i < cfg->bNumInterfaces && !found; ++i) {
        const libusb_interface& iface = cfg->interface[i];
        for (int a = 0; a < iface.num_altsetting && !found; ++a) {
            const libusb_interface_descriptor& alt = iface.altsetting[a];
            if (alt.bInterfaceClass != 0x08u || alt.bInterfaceProtocol != 0x50u) {
                continue;
            }
            std::uint8_t epIn = 0;
            std::uint8_t epOut = 0;
            for (std::uint8_t e = 0; e < alt.bNumEndpoints; ++e) {
                const libusb_endpoint_descriptor& ep = alt.endpoint[e];
                if ((ep.bmAttributes & 0x03u) != LIBUSB_TRANSFER_TYPE_BULK) {
                    continue;
                }
                if ((ep.bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN) {
                    if (epIn == 0) {
                        epIn = ep.bEndpointAddress;
                    }
                } else {
                    if (epOut == 0) {
                        epOut = ep.bEndpointAddress;
                    }
                }
            }
            if (epIn != 0 && epOut != 0) {
                outPair.interfaceNumber = alt.bInterfaceNumber;
                outPair.bulkInEp = epIn;
                outPair.bulkOutEp = epOut;
                found = true;
            }
        }
    }
    libusb_free_config_descriptor(cfg);
    return found;
}

class LibusbBotPipe final : public IUsbBotPipe {
  public:
    LibusbBotPipe(libusb_context* ctx, libusb_device_handle* handle,
                  const LibusbEndpointPair& eps) noexcept
        : ctx_(ctx), handle_(handle), eps_(eps) {}

    ~LibusbBotPipe() override {
        if (handle_) {
            libusb_release_interface(handle_, static_cast<int>(eps_.interfaceNumber));
            libusb_close(handle_);
        }
        if (ctx_) {
            libusb_exit(ctx_);
        }
    }

    UsbPipeStatus bulkOut(const std::uint8_t* data, int len, int& actualOut, int timeoutMs,
                          std::string& error) override {
        actualOut = 0;
        if (!handle_) {
            error = "USB device handle is closed.";
            return UsbPipeStatus::IoError;
        }
        const int rc =
            libusb_bulk_transfer(handle_, eps_.bulkOutEp, const_cast<unsigned char*>(data), len,
                                 &actualOut, static_cast<unsigned int>(timeoutMs));
        return mapStatus("USB bulk OUT", rc, error);
    }

    UsbPipeStatus bulkIn(std::uint8_t* data, int maxLen, int& actualOut, int timeoutMs,
                         std::string& error) override {
        actualOut = 0;
        if (!handle_) {
            error = "USB device handle is closed.";
            return UsbPipeStatus::IoError;
        }
        const int rc = libusb_bulk_transfer(handle_, eps_.bulkInEp, data, maxLen, &actualOut,
                                            static_cast<unsigned int>(timeoutMs));
        return mapStatus("USB bulk IN", rc, error);
    }

    bool clearHaltIn(std::string& error) override { return clearEp(eps_.bulkInEp, error); }
    bool clearHaltOut(std::string& error) override { return clearEp(eps_.bulkOutEp, error); }

    bool bomsReset(std::string& error) override {
        if (!handle_) {
            error = "USB device handle is closed.";
            return false;
        }
        const int rc = libusb_control_transfer(
            handle_,
            static_cast<std::uint8_t>(LIBUSB_ENDPOINT_OUT) |
                static_cast<std::uint8_t>(LIBUSB_REQUEST_TYPE_CLASS) |
                static_cast<std::uint8_t>(LIBUSB_RECIPIENT_INTERFACE),
            0xFFu, 0u, static_cast<std::uint16_t>(eps_.interfaceNumber), nullptr, 0u, 5000u);
        if (rc == LIBUSB_ERROR_PIPE) {
            return true;
        }
        if (rc < 0) {
            error = std::string("USB Bulk-Only Mass Storage Reset failed: ") +
                    libusb_strerror(static_cast<libusb_error>(rc));
            return false;
        }
        return true;
    }

    void sleepMs(int ms) override {
        if (ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        }
    }

  private:
    UsbPipeStatus mapStatus(const char* op, int rc, std::string& error) {
        if (rc == 0) {
            return UsbPipeStatus::Ok;
        }
        if (rc == LIBUSB_ERROR_PIPE) {
            error = std::string(op) + " stalled (PIPE)";
            return UsbPipeStatus::Stall;
        }
        if (rc == LIBUSB_ERROR_TIMEOUT) {
            error = std::string(op) + " timed out";
            return UsbPipeStatus::Timeout;
        }
        error = std::string(op) + " failed: " + libusb_strerror(static_cast<libusb_error>(rc));
        return UsbPipeStatus::IoError;
    }

    bool clearEp(std::uint8_t ep, std::string& error) {
        const int rc = libusb_clear_halt(handle_, ep);
        if (rc != 0) {
            error = std::string("libusb_clear_halt failed: ") +
                    libusb_strerror(static_cast<libusb_error>(rc));
            return false;
        }
        return true;
    }

    libusb_context* ctx_ = nullptr;
    libusb_device_handle* handle_ = nullptr;
    LibusbEndpointPair eps_{};
};

class LibusbBotBackend final : public ScsiBackend {
  public:
    LibusbBotBackend(libusb_context* ctx, libusb_device_handle* handle,
                     const LibusbEndpointPair& eps)
        : pipe_(ctx, handle, eps), session_(pipe_, eps.maxLun) {}

    int reset(std::string& error) override { return session_.reset(error) ? 0 : -1; }

    int execute(const std::uint8_t* cdb, int cdbLen, int direction, const std::uint8_t* dataIn,
                int dataInLen, std::uint8_t* dataOut, int dataOutMaxLen,
                std::string& error) override {
        return executeChunkedScsiCommand(
            cdb, cdbLen, direction, dataIn, dataInLen, dataOut, dataOutMaxLen,
            kDefaultMaxScsiTransferBytes,
            [this](const std::uint8_t* chunkCdb, int chunkCdbLen, int dir, const std::uint8_t* in,
                   int inLen, std::uint8_t* out, int outMaxLen, std::string& err) {
                return session_.executeCommand(chunkCdb, chunkCdbLen, dir, in, inLen, out,
                                               outMaxLen, err);
            },
            error);
    }

  private:
    LibusbBotPipe pipe_;
    UsbBotSession session_;
};

#if defined(__APPLE__)
std::uint32_t macLocationFromLibusbDevice(libusb_device* dev) noexcept {
    const std::uint32_t bus = static_cast<std::uint32_t>(libusb_get_bus_number(dev));
    std::uint8_t ports[8]{};
    const int n = libusb_get_port_numbers(dev, ports, 6);
    std::uint32_t loc = (bus & 0xFFu) << 24;
    for (int i = 0; i < n && i < 6; ++i) {
        loc |= (static_cast<std::uint32_t>(ports[i] & 0x0Fu) << (20 - i * 4));
    }
    return loc;
}
#endif

std::vector<HiMdPhysicalDeviceInfo> enumerateLibusbHiMdDevices(
    const std::set<std::pair<int, int>>& skipBusAddrs) {
    std::vector<HiMdPhysicalDeviceInfo> out;
    libusb_context* ctx = nullptr;
    if (libusb_init(&ctx) != 0 || !ctx) {
        return out;
    }
    libusb_device** list = nullptr;
    const ssize_t count = libusb_get_device_list(ctx, &list);
    if (count > 0 && list) {
        for (ssize_t i = 0; i < count; ++i) {
            libusb_device* dev = list[i];
            libusb_device_descriptor desc{};
            if (libusb_get_device_descriptor(dev, &desc) != 0) {
                continue;
            }
            const KnownHiMdProduct* known =
                findKnownHiMdProduct(desc.idVendor, desc.idProduct);
            if (!known) {
                continue;
            }
            const int bus = static_cast<int>(libusb_get_bus_number(dev));
            const int addr = static_cast<int>(libusb_get_device_address(dev));
            if (skipBusAddrs.count({bus, addr}) > 0) {
                continue;
            }
            LibusbEndpointPair eps;
            if (!findBotInterface(dev, eps)) {
                continue;
            }
            HiMdPhysicalDeviceInfo info;
            info.id = formatUsbDeviceId(bus, addr);
            info.vendorId = desc.idVendor;
            info.productId = desc.idProduct;
            info.name = known->name;
            info.transport = "libusb-bot";
            info.systemPath = info.id;
            out.push_back(std::move(info));
        }
    }
    if (list) {
        libusb_free_device_list(list, 1);
    }
    libusb_exit(ctx);
    return out;
}

std::unique_ptr<ScsiBackend> openLibusbBotBackend(std::uint16_t wantVid, std::uint16_t wantPid,
                                                  int wantBus, int wantAddr,
                                                  std::uint32_t wantLocationId,
                                                  const std::string& bsdHint,
                                                  std::string& error) {
    libusb_context* ctx = nullptr;
    if (libusb_init(&ctx) != 0 || !ctx) {
        error = "libusb_init failed while opening Hi-MD device.";
        return nullptr;
    }

    libusb_device** list = nullptr;
    const ssize_t count = libusb_get_device_list(ctx, &list);
    if (count < 0 || !list) {
        libusb_exit(ctx);
        error = "libusb_get_device_list failed while opening Hi-MD device.";
        return nullptr;
    }

    libusb_device* fallbackVidPid = nullptr;
    int vidPidMatches = 0;
    libusb_device* chosen = nullptr;

    for (ssize_t i = 0; i < count; ++i) {
        libusb_device* dev = list[i];
        libusb_device_descriptor desc{};
        if (libusb_get_device_descriptor(dev, &desc) != 0) {
            continue;
        }
        if (!findKnownHiMdProduct(desc.idVendor, desc.idProduct)) {
            continue;
        }
        const int bus = static_cast<int>(libusb_get_bus_number(dev));
        const int addr = static_cast<int>(libusb_get_device_address(dev));
        if (wantBus > 0 && wantAddr > 0 && bus == wantBus && addr == wantAddr) {
            chosen = dev;
            break;
        }
        if (wantVid != 0 && wantPid != 0 && desc.idVendor == wantVid &&
            desc.idProduct == wantPid) {
            ++vidPidMatches;
            fallbackVidPid = dev;
#if defined(__APPLE__)
            if (wantLocationId != 0 && macLocationFromLibusbDevice(dev) == wantLocationId) {
                chosen = dev;
                break;
            }
#else
            (void)wantLocationId;
#endif
        }
    }

    if (!chosen && wantBus <= 0 && vidPidMatches == 1) {
        chosen = fallbackVidPid;
    }

    if (!chosen) {
        libusb_free_device_list(list, 1);
        libusb_exit(ctx);
        error = "Physical Hi-MD USB device is no longer connected.";
        return nullptr;
    }

    LibusbEndpointPair eps;
    if (!findBotInterface(chosen, eps)) {
        libusb_free_device_list(list, 1);
        libusb_exit(ctx);
        error = "Hi-MD device does not expose a USB Mass Storage Bulk-Only interface.";
        return nullptr;
    }

    libusb_device_handle* handle = nullptr;
    const int openRc = libusb_open(chosen, &handle);
    libusb_free_device_list(list, 1);

    if (openRc != 0 || !handle) {
        libusb_exit(ctx);
        std::ostringstream oss;
        oss << "Failed opening Hi-MD USB device ("
            << libusb_strerror(static_cast<libusb_error>(openRc)) << ").";
#if defined(__APPLE__)
        if (!bsdHint.empty()) {
            oss << " macOS mass storage (" << bsdHint << ") may still hold exclusive ownership.";
        }
#else
        (void)bsdHint;
#endif
        error = oss.str();
        return nullptr;
    }

    (void)libusb_set_auto_detach_kernel_driver(handle, 1);
    const int claimRc = libusb_claim_interface(handle, static_cast<int>(eps.interfaceNumber));
    if (claimRc != 0) {
        std::ostringstream oss;
        oss << "Failed claiming Hi-MD USB Mass Storage interface #"
            << static_cast<int>(eps.interfaceNumber) << ": "
            << libusb_strerror(static_cast<libusb_error>(claimRc)) << ".";
#if defined(__APPLE__)
        (void)bsdHint;
        oss << " On macOS, IOUSBMassStorageDriver locks the interface exclusively; "
               "run with sudo (or grant admin privileges) to capture the Hi-MD device.";
#endif
        libusb_close(handle);
        libusb_exit(ctx);
        error = oss.str();
        return nullptr;
    }

    std::uint8_t maxLunByte = 0;
    const int lunRc = libusb_control_transfer(
        handle,
        static_cast<std::uint8_t>(LIBUSB_ENDPOINT_IN) |
            static_cast<std::uint8_t>(LIBUSB_REQUEST_TYPE_CLASS) |
            static_cast<std::uint8_t>(LIBUSB_RECIPIENT_INTERFACE),
        0xFEu, 0u, static_cast<std::uint16_t>(eps.interfaceNumber), &maxLunByte, 1u, 2000u);
    if (lunRc == 1 && maxLunByte <= 15u) {
        eps.maxLun = 0; // Hi-MD always uses LUN 0 even if maxLun >= 0
    }

    return std::make_unique<LibusbBotBackend>(ctx, handle, eps);
}

// ============================================================================
// Platform-Specific OS SCSI Pass-Through Backends
// ============================================================================

#if defined(__APPLE__)

std::string cfStringToStdString(CFStringRef s) {
    if (!s) {
        return {};
    }
    const char* fast = CFStringGetCStringPtr(s, kCFStringEncodingUTF8);
    if (fast) {
        return std::string(fast);
    }
    const CFIndex len = CFStringGetLength(s);
    const CFIndex maxBytes = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
    std::string out(static_cast<std::size_t>(maxBytes), '\0');
    if (CFStringGetCString(s, out.data(), maxBytes, kCFStringEncodingUTF8)) {
        out.resize(std::strlen(out.c_str()));
        return out;
    }
    return {};
}

bool readRegistryU32(io_registry_entry_t entry, CFStringRef key, std::uint32_t& out) noexcept {
    CFTypeRef val = IORegistryEntryCreateCFProperty(entry, key, kCFAllocatorDefault, 0);
    if (!val) {
        return false;
    }
    bool ok = false;
    if (CFGetTypeID(val) == CFNumberGetTypeID()) {
        std::int64_t tmp = 0;
        if (CFNumberGetValue(static_cast<CFNumberRef>(val), kCFNumberSInt64Type, &tmp)) {
            out = static_cast<std::uint32_t>(tmp);
            ok = true;
        }
    }
    CFRelease(val);
    return ok;
}

bool serviceHasScsiTaskUserClient(io_service_t service) noexcept {
    CFTypeRef prop = IORegistryEntryCreateCFProperty(service, CFSTR("IOCFPlugInTypes"),
                                                     kCFAllocatorDefault, 0);
    if (!prop) {
        return false;
    }
    bool found = false;
    if (CFGetTypeID(prop) == CFDictionaryGetTypeID()) {
        found = CFDictionaryContainsKey(
            static_cast<CFDictionaryRef>(prop),
            CFSTR("7D66678E-08A2-11D5-A1B8-0030657D052A")) != 0;
    }
    CFRelease(prop);
    return found;
}

struct MacHiMdRegistryMatch {
    std::uint16_t vendorId = 0;
    std::uint16_t productId = 0;
    std::uint32_t locationId = 0;
    std::string id;
    std::string name;
    std::string bsdWholeDisk;
    io_service_t scsiService = IO_OBJECT_NULL;

    MacHiMdRegistryMatch() = default;
    MacHiMdRegistryMatch(const MacHiMdRegistryMatch&) = delete;
    MacHiMdRegistryMatch& operator=(const MacHiMdRegistryMatch&) = delete;
    MacHiMdRegistryMatch(MacHiMdRegistryMatch&& other) noexcept
        : vendorId(other.vendorId),
          productId(other.productId),
          locationId(other.locationId),
          id(std::move(other.id)),
          name(std::move(other.name)),
          bsdWholeDisk(std::move(other.bsdWholeDisk)),
          scsiService(other.scsiService) {
        other.scsiService = IO_OBJECT_NULL;
    }
    ~MacHiMdRegistryMatch() {
        if (scsiService != IO_OBJECT_NULL) {
            IOObjectRelease(scsiService);
        }
    }
};

std::string formatMacDeviceId(std::uint16_t vid, std::uint16_t pid, std::uint32_t locId) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "macos:%04x:%04x:%08x", static_cast<unsigned>(vid),
                  static_cast<unsigned>(pid), static_cast<unsigned>(locId));
    return std::string(buf);
}

std::vector<MacHiMdRegistryMatch> scanMacHiMdUsbServices() {
    std::vector<MacHiMdRegistryMatch> matches;
    std::set<std::string> seenIds;

    const char* classes[2] = {"IOUSBHostDevice", "IOUSBDevice"};
    for (const char* cls : classes) {
        CFMutableDictionaryRef matching = IOServiceMatching(cls);
        if (!matching) {
            continue;
        }
        io_iterator_t iter = IO_OBJECT_NULL;
        if (IOServiceGetMatchingServices(MACH_PORT_NULL, matching, &iter) != KERN_SUCCESS ||
            iter == IO_OBJECT_NULL) {
            continue;
        }
        io_service_t usbDev = IO_OBJECT_NULL;
        while ((usbDev = IOIteratorNext(iter)) != IO_OBJECT_NULL) {
            std::uint32_t vid32 = 0;
            std::uint32_t pid32 = 0;
            std::uint32_t loc32 = 0;
            if (readRegistryU32(usbDev, CFSTR("idVendor"), vid32) &&
                readRegistryU32(usbDev, CFSTR("idProduct"), pid32)) {
                const auto vid = static_cast<std::uint16_t>(vid32 & 0xFFFFu);
                const auto pid = static_cast<std::uint16_t>(pid32 & 0xFFFFu);
                const KnownHiMdProduct* known = findKnownHiMdProduct(vid, pid);
                if (known) {
                    (void)readRegistryU32(usbDev, CFSTR("locationID"), loc32);
                    const std::string devId = formatMacDeviceId(vid, pid, loc32);
                    if (seenIds.insert(devId).second) {
                        MacHiMdRegistryMatch m;
                        m.vendorId = vid;
                        m.productId = pid;
                        m.locationId = loc32;
                        m.id = devId;
                        m.name = known->name;

                        io_iterator_t childIter = IO_OBJECT_NULL;
                        if (IORegistryEntryCreateIterator(
                                usbDev, kIOServicePlane, kIORegistryIterateRecursively,
                                &childIter) == KERN_SUCCESS &&
                            childIter != IO_OBJECT_NULL) {
                            io_registry_entry_t child = IO_OBJECT_NULL;
                            while ((child = IOIteratorNext(childIter)) != IO_OBJECT_NULL) {
                                if (m.scsiService == IO_OBJECT_NULL &&
                                    serviceHasScsiTaskUserClient(child)) {
                                    IOObjectRetain(child);
                                    m.scsiService = child;
                                }
                                if (m.bsdWholeDisk.empty() &&
                                    IOObjectConformsTo(child, kIOMediaClass)) {
                                    CFTypeRef wholeProp = IORegistryEntryCreateCFProperty(
                                        child, CFSTR(kIOMediaWholeKey), kCFAllocatorDefault, 0);
                                    const bool isWhole = wholeProp &&
                                                         CFGetTypeID(wholeProp) == CFBooleanGetTypeID() &&
                                                         CFBooleanGetValue(
                                                             static_cast<CFBooleanRef>(wholeProp));
                                    if (wholeProp) {
                                        CFRelease(wholeProp);
                                    }
                                    if (isWhole) {
                                        CFTypeRef bsdProp = IORegistryEntryCreateCFProperty(
                                            child, CFSTR(kIOBSDNameKey), kCFAllocatorDefault, 0);
                                        if (bsdProp &&
                                            CFGetTypeID(bsdProp) == CFStringGetTypeID()) {
                                            m.bsdWholeDisk =
                                                cfStringToStdString(static_cast<CFStringRef>(bsdProp));
                                        }
                                        if (bsdProp) {
                                            CFRelease(bsdProp);
                                        }
                                    }
                                }
                                IOObjectRelease(child);
                            }
                            IOObjectRelease(childIter);
                        }
                        matches.push_back(std::move(m));
                    }
                }
            }
            IOObjectRelease(usbDev);
        }
        IOObjectRelease(iter);
    }
    return matches;
}

struct DaOpWaitContext {
    bool done = false;
    bool ok = false;
    std::string message;
};

void onDaOpCompleted(DADiskRef /*disk*/, DADissenterRef dissenter, void* ctxPtr) {
    auto* ctx = static_cast<DaOpWaitContext*>(ctxPtr);
    if (!ctx) {
        return;
    }
    if (!dissenter) {
        ctx->ok = true;
    } else {
        ctx->ok = false;
        CFStringRef msg = DADissenterGetStatusString(dissenter);
        const std::string detail = cfStringToStdString(msg);
        std::ostringstream oss;
        oss << "DiskArbitration declined operation (status 0x" << std::hex
            << static_cast<unsigned>(DADissenterGetStatus(dissenter)) << ")";
        if (!detail.empty()) {
            oss << ": " << detail;
        }
        ctx->message = oss.str();
    }
    ctx->done = true;
    CFRunLoopStop(CFRunLoopGetCurrent());
}

bool setBsdWholeDiskMountState(const std::string& bsdName, bool mount, bool& outChangedState,
                               std::string& error) {
    outChangedState = false;
    if (bsdName.empty()) {
        return true;
    }
    DASessionRef session = DASessionCreate(kCFAllocatorDefault);
    if (!session) {
        error = "Failed creating DiskArbitration session.";
        return false;
    }
    DADiskRef disk = DADiskCreateFromBSDName(kCFAllocatorDefault, session, bsdName.c_str());
    if (!disk) {
        CFRelease(session);
        error = "Failed looking up BSD disk " + bsdName + " in DiskArbitration.";
        return false;
    }
    CFDictionaryRef desc = DADiskCopyDescription(disk);
    const bool currentlyMounted =
        desc && CFDictionaryContainsKey(desc, kDADiskDescriptionVolumePathKey);
    if (desc) {
        CFRelease(desc);
    }
    if (!mount && !currentlyMounted) {
        CFRelease(disk);
        CFRelease(session);
        return true;
    }

    DaOpWaitContext waitCtx;
    CFRunLoopRef rl = CFRunLoopGetCurrent();
    DASessionScheduleWithRunLoop(session, rl, kCFRunLoopDefaultMode);
    if (mount) {
        DADiskMount(disk, nullptr, kDADiskMountOptionWhole, onDaOpCompleted, &waitCtx);
    } else {
        DADiskUnmount(disk, kDADiskUnmountOptionWhole, onDaOpCompleted, &waitCtx);
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!waitCtx.done && std::chrono::steady_clock::now() < deadline) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, true);
    }
    DASessionUnscheduleFromRunLoop(session, rl, kCFRunLoopDefaultMode);
    CFRelease(disk);
    CFRelease(session);

    if (!waitCtx.done) {
        error = std::string("Timed out waiting for DiskArbitration ") +
                (mount ? "mount" : "unmount") + " on " + bsdName + ".";
        return false;
    }
    if (!waitCtx.ok) {
        error = waitCtx.message;
        return false;
    }
    outChangedState = true;
    return true;
}

class MacScsiTaskBackend final : public ScsiBackend {
  public:
    MacScsiTaskBackend(IOCFPlugInInterface** plugIn, SCSITaskDeviceInterface** devIf) noexcept
        : plugIn_(plugIn), devIf_(devIf) {}

    ~MacScsiTaskBackend() override {
        if (devIf_) {
            (*devIf_)->ReleaseExclusiveAccess(devIf_);
            (*devIf_)->Release(devIf_);
        }
        if (plugIn_) {
            IODestroyPlugInInterface(plugIn_);
        }
    }

    int reset(std::string& /*error*/) override { return 0; }

    int execute(const std::uint8_t* cdb, int cdbLen, int direction, const std::uint8_t* dataIn,
                int dataInLen, std::uint8_t* dataOut, int dataOutMaxLen,
                std::string& error) override {
        return executeChunkedScsiCommand(
            cdb, cdbLen, direction, dataIn, dataInLen, dataOut, dataOutMaxLen,
            kDefaultMaxScsiTransferBytes,
            [this](const std::uint8_t* chunkCdb, int chunkCdbLen, int dir, const std::uint8_t* in,
                   int inLen, std::uint8_t* out, int outMaxLen, std::string& err) {
                return executeWithSenseRetry(chunkCdb, chunkCdbLen, dir, in, inLen, out, outMaxLen,
                                             err);
            },
            error);
    }

  private:
    int executeWithSenseRetry(const std::uint8_t* cdb, int cdbLen, int direction,
                              const std::uint8_t* dataIn, int dataInLen, std::uint8_t* dataOut,
                              int dataOutMaxLen, std::string& error) {
        for (int attempt = 0; attempt <= kMaxUnitReadyRetries; ++attempt) {
            std::array<std::uint8_t, 16> rawCdb{};
            const int normLen = normalizeOsScsiCdbLength(cdb, cdbLen);
            const int copyLen = std::min(normLen, 16);
            std::memcpy(rawCdb.data(), cdb, static_cast<std::size_t>(copyLen));

            // SCSITask::SetCommandDescriptorBlock requires valid SCSI CDB size: 6, 10, 12, or 16.
            int scsiCdbLen = 16;
            if (normLen <= 6) {
                scsiCdbLen = 6;
            } else if (normLen <= 10) {
                scsiCdbLen = 10;
            } else if (normLen <= 12) {
                scsiCdbLen = 12;
            }

            SCSITaskInterface** task = (*devIf_)->CreateSCSITask(devIf_);
            if (!task) {
                error = "SCSITaskDeviceInterface::CreateSCSITask returned NULL.";
                return -1;
            }

            std::uint8_t txDir = kSCSIDataTransfer_NoDataTransfer;
            UInt64 txCount = 0;
            SCSITaskSGElement sg{};
            SCSITaskSGElement* sgPtr = nullptr;
            UInt8 sgEntries = 0;

            if (direction == 1 && dataOutMaxLen > 0) {
                if (!dataOut) {
                    (*task)->Release(task);
                    error = "macOS SCSITask missing IN output buffer.";
                    return -1;
                }
                txDir = kSCSIDataTransfer_FromTargetToInitiator;
                txCount = static_cast<UInt64>(dataOutMaxLen);
                sg.address = reinterpret_cast<mach_vm_address_t>(dataOut);
                sg.length = static_cast<mach_vm_size_t>(dataOutMaxLen);
                sgPtr = &sg;
                sgEntries = 1;
            } else if (direction == 2 && dataInLen > 0) {
                if (!dataIn) {
                    (*task)->Release(task);
                    error = "macOS SCSITask missing OUT input buffer.";
                    return -1;
                }
                txDir = kSCSIDataTransfer_FromInitiatorToTarget;
                txCount = static_cast<UInt64>(dataInLen);
                sg.address = reinterpret_cast<mach_vm_address_t>(const_cast<std::uint8_t*>(dataIn));
                sg.length = static_cast<mach_vm_size_t>(dataInLen);
                sgPtr = &sg;
                sgEntries = 1;
            }

            IOReturn kr = (*task)->SetCommandDescriptorBlock(
                task, rawCdb.data(), static_cast<UInt8>(scsiCdbLen));
            if (kr == kIOReturnSuccess) {
                kr = (*task)->SetScatterGatherEntries(task, sgPtr, sgEntries, txCount, txDir);
            }
            if (kr == kIOReturnSuccess) {
                kr = (*task)->SetTimeoutDuration(task, static_cast<UInt32>(kUsbTimeoutMs));
            }
            if (kr != kIOReturnSuccess) {
                (*task)->Release(task);
                std::ostringstream oss;
                oss << "SCSITask setup failed (IOReturn 0x" << std::hex
                    << static_cast<unsigned>(kr) << ").";
                error = oss.str();
                return -1;
            }

            SCSI_Sense_Data senseData{};
            SCSITaskStatus taskStatus = kSCSITaskStatus_GOOD;
            UInt64 realized = 0;
            kr = (*task)->ExecuteTaskSync(task, &senseData, &taskStatus, &realized);
            SCSIServiceResponse svcResp = kSCSIServiceResponse_TASK_COMPLETE;
            (void)(*task)->GetSCSIServiceResponse(task, &svcResp);
            (*task)->Release(task);

            if (kr != kIOReturnSuccess ||
                svcResp != kSCSIServiceResponse_TASK_COMPLETE) {
                std::ostringstream oss;
                oss << "macOS SCSITask execution failed for CDB 0x" << std::hex
                    << static_cast<unsigned>(cdb[0]) << " (IOReturn 0x"
                    << static_cast<unsigned>(kr) << ", svcResp " << std::dec
                    << static_cast<int>(svcResp) << ").";
                error = oss.str();
                return -1;
            }

            if (taskStatus == kSCSITaskStatus_GOOD) {
                if (direction == 1) {
                    return static_cast<int>(
                        std::min<UInt64>(realized, static_cast<UInt64>(dataOutMaxLen)));
                }
                return direction == 2 ? dataInLen : 0;
            }

            if (taskStatus == kSCSITaskStatus_CHECK_CONDITION && cdb[0] != 0x03u) {
                const auto parsed = parseAndClassifyScsiSense(
                    reinterpret_cast<const std::uint8_t*>(&senseData), sizeof(senseData));
                if (parsed.disposition == ScsiSenseDisposition::RetryUnitAttention &&
                    attempt < kMaxUnitReadyRetries) {
                    continue;
                }
                if (parsed.disposition == ScsiSenseDisposition::RetryBecomingReady &&
                    attempt < kMaxUnitReadyRetries) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(150));
                    continue;
                }
                error = formatScsiSenseError("macOS SCSITask", parsed);
                return -1;
            }

            std::ostringstream oss;
            oss << "macOS SCSITask failed for CDB 0x" << std::hex << static_cast<unsigned>(cdb[0])
                << " with status 0x" << static_cast<unsigned>(taskStatus) << ".";
            error = oss.str();
            return -1;
        }
        error = "macOS SCSITask exceeded retry limit waiting for device readiness.";
        return -1;
    }

    IOCFPlugInInterface** plugIn_ = nullptr;
    SCSITaskDeviceInterface** devIf_ = nullptr;
};

#elif defined(__linux__)

struct LinuxSgDeviceInfo {
    HiMdPhysicalDeviceInfo info;
    std::string sgPath;
    std::string blockDevName; // e.g. "sda" -> /dev/sda
    int bus = -1;
    int addr = -1;
};

bool readSmallIntFile(const std::filesystem::path& p, int& out, bool hex) {
    std::ifstream f(p);
    if (!f) {
        return false;
    }
    std::string s;
    std::getline(f, s);
    char* endp = nullptr;
    const long v = std::strtol(s.c_str(), &endp, hex ? 16 : 10);
    if (endp == s.c_str()) {
        return false;
    }
    out = static_cast<int>(v);
    return true;
}

std::vector<LinuxSgDeviceInfo> scanLinuxSgDevices() {
    std::vector<LinuxSgDeviceInfo> results;
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path sgClass("/sys/class/scsi_generic");
    if (!fs::exists(sgClass, ec)) {
        return results;
    }
    for (const auto& entry : fs::directory_iterator(sgClass, ec)) {
        const std::string sgName = entry.path().filename().string();
        if (sgName.rfind("sg", 0) != 0) {
            continue;
        }
        const fs::path devLink = fs::canonical(entry.path() / "device", ec);
        if (ec) {
            continue;
        }
        fs::path usbDir = devLink;
        int vid = -1;
        int pid = -1;
        int bus = -1;
        int addr = -1;
        for (int depth = 0; depth < 8 && !usbDir.empty(); ++depth) {
            if (fs::exists(usbDir / "idVendor", ec) && fs::exists(usbDir / "idProduct", ec)) {
                (void)readSmallIntFile(usbDir / "idVendor", vid, true);
                (void)readSmallIntFile(usbDir / "idProduct", pid, true);
                (void)readSmallIntFile(usbDir / "busnum", bus, false);
                (void)readSmallIntFile(usbDir / "devnum", addr, false);
                break;
            }
            usbDir = usbDir.parent_path();
        }
        if (vid < 0 || pid < 0) {
            continue;
        }
        const KnownHiMdProduct* known =
            findKnownHiMdProduct(static_cast<std::uint16_t>(vid), static_cast<std::uint16_t>(pid));
        if (!known) {
            continue;
        }

        LinuxSgDeviceInfo item;
        item.sgPath = "/dev/" + sgName;
        item.bus = bus;
        item.addr = addr;

        const fs::path blkDir = devLink / "block";
        if (fs::exists(blkDir, ec)) {
            for (const auto& bEntry : fs::directory_iterator(blkDir, ec)) {
                item.blockDevName = bEntry.path().filename().string();
                break;
            }
        }

        item.info.id = "sg:" + item.sgPath;
        item.info.vendorId = static_cast<std::uint16_t>(vid);
        item.info.productId = static_cast<std::uint16_t>(pid);
        item.info.name = known->name;
        item.info.transport = "linux-sgio";
        item.info.systemPath = item.sgPath;
        results.push_back(std::move(item));
    }
    return results;
}

bool findLinuxMountPoint(const std::string& blockDevName, std::string& outMountPoint) {
    if (blockDevName.empty()) {
        return false;
    }
    const std::string prefix = "/dev/" + blockDevName;
    std::ifstream f("/proc/mounts");
    std::string src;
    std::string mnt;
    std::string rest;
    while (f >> src >> mnt) {
        std::getline(f, rest);
        if (src == prefix ||
            (src.rfind(prefix, 0) == 0 && src.size() > prefix.size() &&
             (std::isdigit(static_cast<unsigned char>(src[prefix.size()])) ||
              src[prefix.size()] == 'p'))) {
            outMountPoint = mnt;
            return true;
        }
    }
    return false;
}

class LinuxSgIoBackend final : public ScsiBackend {
  public:
    LinuxSgIoBackend(int fd, std::string sgPath, std::string blockDevName) noexcept
        : fd_(fd), sgPath_(std::move(sgPath)), blockDevName_(std::move(blockDevName)) {}

    ~LinuxSgIoBackend() override {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    int reset(std::string& error) override {
        int resetArg = SG_SCSI_RESET_DEVICE;
        if (::ioctl(fd_, SG_SCSI_RESET, &resetArg) < 0) {
            const int e = errno;
            if (e != ENOTTY && e != EINVAL && e != EPERM) {
                error = "SG_SCSI_RESET failed on " + sgPath_ + ": " + std::strerror(e);
                return -1;
            }
        }
        return 0;
    }

    int execute(const std::uint8_t* cdb, int cdbLen, int direction, const std::uint8_t* dataIn,
                int dataInLen, std::uint8_t* dataOut, int dataOutMaxLen,
                std::string& error) override {
        if (isMutatingHiMdScsiCommand(cdb, cdbLen)) {
            std::string mountPoint;
            if (findLinuxMountPoint(blockDevName_, mountPoint)) {
                error = "Hi-MD volume /dev/" + blockDevName_ + " is mounted at " + mountPoint +
                        "; unmount it before writing.";
                return -1;
            }
        }
        return executeChunkedScsiCommand(
            cdb, cdbLen, direction, dataIn, dataInLen, dataOut, dataOutMaxLen,
            kDefaultMaxScsiTransferBytes,
            [this](const std::uint8_t* chunkCdb, int chunkCdbLen, int dir, const std::uint8_t* in,
                   int inLen, std::uint8_t* out, int outMaxLen, std::string& err) {
                return executeWithSenseRetry(chunkCdb, chunkCdbLen, dir, in, inLen, out, outMaxLen,
                                             err);
            },
            error);
    }

  private:
    int executeWithSenseRetry(const std::uint8_t* cdb, int cdbLen, int direction,
                              const std::uint8_t* dataIn, int dataInLen, std::uint8_t* dataOut,
                              int dataOutMaxLen, std::string& error) {
        for (int attempt = 0; attempt <= kMaxUnitReadyRetries; ++attempt) {
            std::array<std::uint8_t, 16> rawCdb{};
            const int normLen = normalizeOsScsiCdbLength(cdb, cdbLen);
            std::memcpy(rawCdb.data(), cdb, static_cast<std::size_t>(normLen));
            std::uint8_t sense[32]{};

            sg_io_hdr_t io{};
            io.interface_id = 'S';
            io.cmdp = rawCdb.data();
            io.cmd_len = static_cast<unsigned char>(normLen);
            io.sbp = sense;
            io.mx_sb_len = static_cast<unsigned char>(sizeof(sense));
            io.timeout = static_cast<unsigned int>(kUsbTimeoutMs);

            if (direction == 1) {
                io.dxfer_direction = dataOutMaxLen > 0 ? SG_DXFER_FROM_DEV : SG_DXFER_NONE;
                io.dxfer_len = static_cast<unsigned int>(std::max(0, dataOutMaxLen));
                io.dxferp = dataOutMaxLen > 0 ? static_cast<void*>(dataOut) : nullptr;
            } else if (direction == 2) {
                io.dxfer_direction = dataInLen > 0 ? SG_DXFER_TO_DEV : SG_DXFER_NONE;
                io.dxfer_len = static_cast<unsigned int>(std::max(0, dataInLen));
                io.dxferp = dataInLen > 0 ? const_cast<void*>(static_cast<const void*>(dataIn))
                                          : nullptr;
            } else {
                io.dxfer_direction = SG_DXFER_NONE;
            }

            if (::ioctl(fd_, SG_IO, &io) < 0) {
                error = "SG_IO failed on " + sgPath_ + ": " + std::strerror(errno);
                return -1;
            }

            if (io.host_status != 0) {
                std::ostringstream oss;
                oss << "Linux SG_IO host error 0x" << std::hex
                    << static_cast<unsigned>(io.host_status) << " on " << sgPath_;
                error = oss.str();
                return -1;
            }

            const bool checkCond = (io.status == 0x02u) || ((io.driver_status & 0x08u) != 0);
            if (!checkCond && io.status == 0) {
                if (direction == 1) {
                    const int xfer = dataOutMaxLen - static_cast<int>(io.resid);
                    return std::max(0, std::min(xfer, dataOutMaxLen));
                }
                return direction == 2 ? dataInLen : 0;
            }

            if (checkCond && cdb[0] != 0x03u && io.sb_len_wr > 0) {
                const auto parsed = parseAndClassifyScsiSense(sense, io.sb_len_wr);
                if (parsed.disposition == ScsiSenseDisposition::RetryUnitAttention &&
                    attempt < kMaxUnitReadyRetries) {
                    continue;
                }
                if (parsed.disposition == ScsiSenseDisposition::RetryBecomingReady &&
                    attempt < kMaxUnitReadyRetries) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(150));
                    continue;
                }
                error = formatScsiSenseError("Linux SG_IO", parsed);
                return -1;
            }

            std::ostringstream oss;
            oss << "Linux SG_IO command 0x" << std::hex << static_cast<unsigned>(cdb[0])
                << " failed with SCSI status 0x" << static_cast<unsigned>(io.status);
            error = oss.str();
            return -1;
        }
        error = "Linux SG_IO exceeded retry limit waiting for device readiness.";
        return -1;
    }

    int fd_ = -1;
    std::string sgPath_;
    std::string blockDevName_;
};

#elif defined(_WIN32)

bool parseWinUsbVidPid(const std::wstring& devId, std::uint16_t& outVid,
                       std::uint16_t& outPid) noexcept {
    unsigned vid = 0;
    unsigned pid = 0;
    for (std::size_t i = 0; i + 17 <= devId.size(); ++i) {
        if (std::swscanf(devId.c_str() + i, L"VID_%4x&PID_%4x", &vid, &pid) == 2 ||
            std::swscanf(devId.c_str() + i, L"vid_%4x&pid_%4x", &vid, &pid) == 2) {
            outVid = static_cast<std::uint16_t>(vid);
            outPid = static_cast<std::uint16_t>(pid);
            return true;
        }
    }
    return false;
}

std::vector<HiMdPhysicalDeviceInfo> scanWindowsPhysicalDrives() {
    std::vector<HiMdPhysicalDeviceInfo> out;
    HDEVINFO devInfo = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_DISK, nullptr, nullptr,
                                            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) {
        return out;
    }

    SP_DEVICE_INTERFACE_DATA ifData{};
    ifData.cbSize = sizeof(ifData);
    for (DWORD idx = 0;
         SetupDiEnumDeviceInterfaces(devInfo, nullptr, &GUID_DEVINTERFACE_DISK, idx, &ifData);
         ++idx) {
        DWORD reqSize = 0;
        SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, nullptr, 0, &reqSize, nullptr);
        if (reqSize == 0) {
            continue;
        }
        std::vector<std::uint8_t> detailBuf(reqSize);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detailBuf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA devData{};
        devData.cbSize = sizeof(devData);
        if (!SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, detail, reqSize, nullptr,
                                             &devData)) {
            continue;
        }

        std::uint16_t vid = 0;
        std::uint16_t pid = 0;
        bool matchedVidPid = false;
        DEVINST cur = devData.DevInst;
        for (int depth = 0; depth < 5; ++depth) {
            wchar_t idBuf[512]{};
            if (CM_Get_Device_IDW(cur, idBuf, 512, 0) == CR_SUCCESS &&
                parseWinUsbVidPid(idBuf, vid, pid)) {
                matchedVidPid = true;
                break;
            }
            DEVINST parent = 0;
            if (CM_Get_Parent(&parent, cur, 0) != CR_SUCCESS) {
                break;
            }
            cur = parent;
        }
        if (!matchedVidPid) {
            continue;
        }
        const KnownHiMdProduct* known = findKnownHiMdProduct(vid, pid);
        if (!known) {
            continue;
        }

        HANDLE hDisk = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, OPEN_EXISTING, 0, nullptr);
        if (hDisk == INVALID_HANDLE_VALUE) {
            continue;
        }
        STORAGE_DEVICE_NUMBER devNum{};
        DWORD bytesRet = 0;
        const BOOL okNum = DeviceIoControl(hDisk, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0,
                                           &devNum, sizeof(devNum), &bytesRet, nullptr);
        CloseHandle(hDisk);
        if (!okNum || devNum.DeviceType != FILE_DEVICE_DISK) {
            continue;
        }

        char idBuf[48];
        std::snprintf(idBuf, sizeof(idBuf), "spti:PhysicalDrive%lu",
                      static_cast<unsigned long>(devNum.DeviceNumber));
        char pathBuf[64];
        std::snprintf(pathBuf, sizeof(pathBuf), "\\\\.\\PhysicalDrive%lu",
                      static_cast<unsigned long>(devNum.DeviceNumber));

        HiMdPhysicalDeviceInfo info;
        info.id = idBuf;
        info.vendorId = vid;
        info.productId = pid;
        info.name = known->name;
        info.transport = "windows-spti";
        info.systemPath = pathBuf;
        out.push_back(std::move(info));
    }

    SetupDiDestroyDeviceInfoList(devInfo);
    return out;
}

class WinSptiBackend final : public ScsiBackend {
  public:
    WinSptiBackend(HANDLE diskHandle, ULONG diskNumber) noexcept
        : disk_(diskHandle), diskNumber_(diskNumber) {}

    ~WinSptiBackend() override {
        for (HANDLE vh : lockedVolumes_) {
            if (vh != INVALID_HANDLE_VALUE) {
                DWORD br = 0;
                DeviceIoControl(vh, FSCTL_UNLOCK_VOLUME, nullptr, 0, nullptr, 0, &br, nullptr);
                CloseHandle(vh);
            }
        }
        if (disk_ != INVALID_HANDLE_VALUE) {
            CloseHandle(disk_);
        }
    }

    int reset(std::string& /*error*/) override { return 0; }

    int execute(const std::uint8_t* cdb, int cdbLen, int direction, const std::uint8_t* dataIn,
                int dataInLen, std::uint8_t* dataOut, int dataOutMaxLen,
                std::string& error) override {
        if (isMutatingHiMdScsiCommand(cdb, cdbLen) && !volumesDismounted_) {
            if (!lockAndDismountVolumes(error)) {
                return -1;
            }
            volumesDismounted_ = true;
        }
        return executeChunkedScsiCommand(
            cdb, cdbLen, direction, dataIn, dataInLen, dataOut, dataOutMaxLen,
            kDefaultMaxScsiTransferBytes,
            [this](const std::uint8_t* chunkCdb, int chunkCdbLen, int dir, const std::uint8_t* in,
                   int inLen, std::uint8_t* out, int outMaxLen, std::string& err) {
                return executeWithSenseRetry(chunkCdb, chunkCdbLen, dir, in, inLen, out, outMaxLen,
                                             err);
            },
            error);
    }

  private:
    bool lockAndDismountVolumes(std::string& error) {
        const DWORD mask = GetLogicalDrives();
        for (int i = 0; i < 26; ++i) {
            if ((mask & (1u << i)) == 0) {
                continue;
            }
            wchar_t volPath[16];
            std::swprintf(volPath, 16, L"\\\\.\\%c:", static_cast<wchar_t>(L'A' + i));
            HANDLE hv = CreateFileW(volPath, GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                                    nullptr);
            if (hv == INVALID_HANDLE_VALUE) {
                continue;
            }
            VOLUME_DISK_EXTENTS ex{};
            DWORD br = 0;
            bool belongsToDisk = false;
            if (DeviceIoControl(hv, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, &ex,
                                sizeof(ex), &br, nullptr)) {
                for (DWORD e = 0; e < ex.NumberOfDiskExtents; ++e) {
                    if (ex.Extents[e].DiskNumber == diskNumber_) {
                        belongsToDisk = true;
                        break;
                    }
                }
            } else {
                STORAGE_DEVICE_NUMBER devNum{};
                if (DeviceIoControl(hv, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &devNum,
                                    sizeof(devNum), &br, nullptr) &&
                    devNum.DeviceType == FILE_DEVICE_DISK &&
                    devNum.DeviceNumber == diskNumber_) {
                    belongsToDisk = true;
                }
            }
            if (!belongsToDisk) {
                CloseHandle(hv);
                continue;
            }
            if (!DeviceIoControl(hv, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &br, nullptr)) {
                const DWORD lockErr = GetLastError();
                CloseHandle(hv);
                std::ostringstream oss;
                oss << "Failed locking Hi-MD volume " << static_cast<char>('A' + i)
                    << ": before write (error " << lockErr
                    << "). Close any Explorer windows using the disc and retry.";
                error = oss.str();
                return false;
            }
            (void)DeviceIoControl(hv, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &br, nullptr);
            lockedVolumes_.push_back(hv);
        }
        return true;
    }

    int executeWithSenseRetry(const std::uint8_t* cdb, int cdbLen, int direction,
                              const std::uint8_t* dataIn, int dataInLen, std::uint8_t* dataOut,
                              int dataOutMaxLen, std::string& error) {
        struct SptdWithSense {
            SCSI_PASS_THROUGH_DIRECT spt;
            ULONG filler;
            UCHAR sense[32];
        };

        for (int attempt = 0; attempt <= kMaxUnitReadyRetries; ++attempt) {
            const int normLen = normalizeOsScsiCdbLength(cdb, cdbLen);
            SptdWithSense pkt{};
            pkt.spt.Length = sizeof(SCSI_PASS_THROUGH_DIRECT);
            pkt.spt.CdbLength = static_cast<UCHAR>(normLen);
            pkt.spt.SenseInfoLength = sizeof(pkt.sense);
            pkt.spt.SenseInfoOffset = offsetof(SptdWithSense, sense);
            pkt.spt.TimeOutValue = 30;
            std::memcpy(pkt.spt.Cdb, cdb, static_cast<std::size_t>(normLen));

            void* alignedBuf = nullptr;
            std::size_t xferLen = 0;
            if (direction == 1 && dataOutMaxLen > 0) {
                xferLen = static_cast<std::size_t>(dataOutMaxLen);
                alignedBuf = _aligned_malloc(xferLen, 4096);
                if (!alignedBuf) {
                    error = "Out of memory allocating aligned Windows SPTI buffer.";
                    return -1;
                }
                std::memset(alignedBuf, 0, xferLen);
                pkt.spt.DataIn = SCSI_IOCTL_DATA_IN;
                pkt.spt.DataTransferLength = static_cast<ULONG>(xferLen);
                pkt.spt.DataBuffer = alignedBuf;
            } else if (direction == 2 && dataInLen > 0) {
                xferLen = static_cast<std::size_t>(dataInLen);
                alignedBuf = _aligned_malloc(xferLen, 4096);
                if (!alignedBuf) {
                    error = "Out of memory allocating aligned Windows SPTI buffer.";
                    return -1;
                }
                std::memcpy(alignedBuf, dataIn, xferLen);
                pkt.spt.DataIn = SCSI_IOCTL_DATA_OUT;
                pkt.spt.DataTransferLength = static_cast<ULONG>(xferLen);
                pkt.spt.DataBuffer = alignedBuf;
            } else {
                pkt.spt.DataIn = SCSI_IOCTL_DATA_UNSPECIFIED;
                pkt.spt.DataTransferLength = 0;
                pkt.spt.DataBuffer = nullptr;
            }

            DWORD bytesReturned = 0;
            const BOOL ok = DeviceIoControl(disk_, IOCTL_SCSI_PASS_THROUGH_DIRECT, &pkt,
                                            sizeof(pkt), &pkt, sizeof(pkt), &bytesReturned, nullptr);
            const DWORD winErr = ok ? ERROR_SUCCESS : GetLastError();
            if (!ok) {
                if (alignedBuf) {
                    _aligned_free(alignedBuf);
                }
                char msg[192];
                std::snprintf(msg, sizeof(msg),
                              "Windows IOCTL_SCSI_PASS_THROUGH_DIRECT failed (error %lu, opcode 0x%02x, cdbLen %d, dir %d, xfer %lu).",
                              static_cast<unsigned long>(winErr), static_cast<unsigned>(cdb[0]),
                              normLen, direction, static_cast<unsigned long>(xferLen));
                error = msg;
                return -1;
            }

            if (pkt.spt.ScsiStatus == 0) {
                const int got = static_cast<int>(
                    std::min<ULONG>(pkt.spt.DataTransferLength, static_cast<ULONG>(dataOutMaxLen)));
                if (direction == 1 && dataOut && got > 0 && alignedBuf) {
                    std::memcpy(dataOut, alignedBuf, static_cast<std::size_t>(got));
                }
                if (alignedBuf) {
                    _aligned_free(alignedBuf);
                }
                if (direction == 1) {
                    return got;
                }
                return direction == 2 ? dataInLen : 0;
            }

            if (alignedBuf) {
                _aligned_free(alignedBuf);
            }

            if (pkt.spt.ScsiStatus == 0x02u && cdb[0] != 0x03u && pkt.spt.SenseInfoLength > 0) {
                const auto parsed =
                    parseAndClassifyScsiSense(pkt.sense, pkt.spt.SenseInfoLength);
                if (parsed.disposition == ScsiSenseDisposition::RetryUnitAttention &&
                    attempt < kMaxUnitReadyRetries) {
                    continue;
                }
                if (parsed.disposition == ScsiSenseDisposition::RetryBecomingReady &&
                    attempt < kMaxUnitReadyRetries) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(150));
                    continue;
                }
                error = formatScsiSenseError("Windows SPTI", parsed);
                return -1;
            }

            std::ostringstream oss;
            oss << "Windows SPTI command 0x" << std::hex << static_cast<unsigned>(cdb[0])
                << " failed with SCSI status 0x" << static_cast<unsigned>(pkt.spt.ScsiStatus);
            error = oss.str();
            return -1;
        }
        error = "Windows SPTI exceeded retry limit waiting for device readiness.";
        return -1;
    }

    HANDLE disk_ = INVALID_HANDLE_VALUE;
    ULONG diskNumber_ = 0;
    bool volumesDismounted_ = false;
    std::vector<HANDLE> lockedVolumes_;
};

#endif

} // namespace

// ============================================================================
// Pure Protocol Helpers
// ============================================================================

const KnownHiMdProduct* findKnownHiMdProduct(std::uint16_t vendorId,
                                               std::uint16_t productId) noexcept {
    for (const auto& entry : kKnownHiMdProducts) {
        if (entry.vendorId == vendorId && entry.productId == productId) {
            return &entry;
        }
    }
    return nullptr;
}

const KnownHiMdProduct* knownHiMdProducts(std::size_t& outCount) noexcept {
    outCount = sizeof(kKnownHiMdProducts) / sizeof(kKnownHiMdProducts[0]);
    return kKnownHiMdProducts;
}

bool isMutatingHiMdScsiCommand(const std::uint8_t* cdb, int cdbLen) noexcept {
    if (!cdb || cdbLen < 1) {
        return true; // fail closed on invalid/empty CDB
    }
    const std::uint8_t op = cdb[0];
    if (op == 0x2Au || op == 0xA3u) {
        return true;
    }
    if (op == 0xC2u) {
        // Sony `getTime`: C2 00 00 50 ... is read-only; all other 0xC2 control commands write/wipe.
        if (cdbLen >= 4 && cdb[1] == 0x00u && cdb[2] == 0x00u && cdb[3] == 0x50u) {
            return false;
        }
        return true;
    }
    return false;
}

int normalizeOsScsiCdbLength(const std::uint8_t* cdb, int cdbLen) noexcept {
    const int clamped = std::clamp(cdbLen, 1, 16);
    if (!cdb) {
        return clamped;
    }
    const std::uint8_t op = cdb[0];
    if ((op == 0x00u || op == 0x03u || op == 0x12u || op == 0x1Eu) && clamped > 6 &&
        bytesAllZero(cdb, 6, clamped)) {
        return 6;
    }
    if ((op == 0x25u || op == 0x28u || op == 0x2Au) && clamped > 10 &&
        bytesAllZero(cdb, 10, clamped)) {
        return 10;
    }
    if ((op == 0xA3u || op == 0xA4u) && clamped == 16 && bytesAllZero(cdb, 12, 16)) {
        return 12;
    }
    return clamped;
}

ParsedScsiSense parseAndClassifyScsiSense(const std::uint8_t* sense, std::size_t len) noexcept {
    ParsedScsiSense out;
    if (!sense || len < 4) {
        return out;
    }
    const std::uint8_t resp = sense[0] & 0x7Fu;
    out.responseCode = resp;
    if (resp == 0x70u || resp == 0x71u) {
        if (len < 14) {
            return out;
        }
        out.senseKey = sense[2] & 0x0Fu;
        out.asc = sense[12];
        out.ascq = sense[13];
    } else if (resp == 0x72u || resp == 0x73u) {
        out.senseKey = sense[1] & 0x0Fu;
        out.asc = sense[2];
        out.ascq = sense[3];
    } else {
        return out;
    }

    if (out.senseKey == 0x00u) {
        out.disposition = ScsiSenseDisposition::Ok;
    } else if (out.senseKey == 0x06u) {
        out.disposition = ScsiSenseDisposition::RetryUnitAttention;
    } else if (out.senseKey == 0x02u && out.asc == 0x04u) {
        out.disposition = ScsiSenseDisposition::RetryBecomingReady;
    } else if (out.senseKey == 0x02u && out.asc == 0x3Au) {
        out.disposition = ScsiSenseDisposition::NoMedium;
    } else if (out.senseKey == 0x07u) {
        out.disposition = ScsiSenseDisposition::WriteProtected;
    } else {
        out.disposition = ScsiSenseDisposition::Fatal;
    }
    return out;
}

std::string formatScsiSenseError(const std::string& context, const ParsedScsiSense& sense) {
    std::ostringstream oss;
    if (sense.disposition == ScsiSenseDisposition::NoMedium) {
        oss << context << ": no Hi-MD disc inserted (SK=0x02, ASC=0x3a, ASCQ=0x" << std::hex
            << static_cast<unsigned>(sense.ascq) << ").";
        return oss.str();
    }
    if (sense.disposition == ScsiSenseDisposition::WriteProtected) {
        oss << context << ": Hi-MD disc is write-protected (SK=0x07, ASC=0x" << std::hex
            << static_cast<unsigned>(sense.asc) << ", ASCQ=0x" << static_cast<unsigned>(sense.ascq)
            << ").";
        return oss.str();
    }
    oss << context << ": SCSI CHECK CONDITION (resp=0x" << std::hex
        << static_cast<unsigned>(sense.responseCode) << ", SK=0x"
        << static_cast<unsigned>(sense.senseKey) << ", ASC=0x" << static_cast<unsigned>(sense.asc)
        << ", ASCQ=0x" << static_cast<unsigned>(sense.ascq) << ").";
    return oss.str();
}

int executeChunkedScsiCommand(const std::uint8_t* cdb, int cdbLen, int direction,
                              const std::uint8_t* dataIn, int dataInLen, std::uint8_t* dataOut,
                              int dataOutMaxLen, std::size_t maxTransferBytes,
                              const RawScsiExecFn& execFn, std::string& error) {
    if (!cdb || cdbLen < 6) {
        error = "Invalid SCSI CDB length.";
        return -1;
    }
    const std::uint8_t op = cdb[0];
    const bool isRead10 = (op == 0x28u && direction == 1 && cdbLen >= 10 && dataOutMaxLen > 0);
    const bool isWrite10 = (op == 0x2Au && direction == 2 && cdbLen >= 10 && dataInLen > 0);
    const std::size_t totalBytes =
        isRead10 ? static_cast<std::size_t>(dataOutMaxLen)
                 : (isWrite10 ? static_cast<std::size_t>(dataInLen) : 0u);

    if ((!isRead10 && !isWrite10) || maxTransferBytes == 0 || totalBytes <= maxTransferBytes) {
        return execFn(cdb, cdbLen, direction, dataIn, dataInLen, dataOut, dataOutMaxLen, error);
    }

    const std::uint32_t totalSectors = readBe16(cdb + 7);
    if (totalSectors == 0 || (totalBytes % totalSectors) != 0) {
        return execFn(cdb, cdbLen, direction, dataIn, dataInLen, dataOut, dataOutMaxLen, error);
    }
    const std::size_t sectorSize = totalBytes / totalSectors;
    if (sectorSize < 512u || maxTransferBytes < sectorSize) {
        return execFn(cdb, cdbLen, direction, dataIn, dataInLen, dataOut, dataOutMaxLen, error);
    }

    const std::uint32_t maxSectorsPerChunk = static_cast<std::uint32_t>(
        std::min<std::size_t>(0xFFFFu, maxTransferBytes / sectorSize));
    std::uint32_t lba = readBe32(cdb + 2);
    std::uint32_t remainingSectors = totalSectors;
    std::size_t byteOffset = 0;

    while (remainingSectors > 0) {
        const std::uint32_t chunkSectors = std::min(remainingSectors, maxSectorsPerChunk);
        const int chunkBytes = static_cast<int>(static_cast<std::size_t>(chunkSectors) * sectorSize);
        std::array<std::uint8_t, 16> chunkCdb{};
        const int copyLen = std::min(cdbLen, 16);
        std::memcpy(chunkCdb.data(), cdb, static_cast<std::size_t>(copyLen));
        writeBe32(chunkCdb.data() + 2, lba);
        writeBe16(chunkCdb.data() + 7, static_cast<std::uint16_t>(chunkSectors));

        const int rc = execFn(
            chunkCdb.data(), copyLen, direction, isWrite10 ? (dataIn + byteOffset) : nullptr,
            isWrite10 ? chunkBytes : 0, isRead10 ? (dataOut + byteOffset) : nullptr,
            isRead10 ? chunkBytes : 0, error);
        if (rc < 0) {
            return -1;
        }
        byteOffset += static_cast<std::size_t>(rc);
        lba += chunkSectors;
        remainingSectors -= chunkSectors;
        if (isRead10 && rc < chunkBytes) {
            break;
        }
    }
    return static_cast<int>(byteOffset);
}

// ============================================================================
// USB Mass Storage BOT Wire Format & Session
// ============================================================================

std::array<std::uint8_t, 31> buildUsbBotCbw(std::uint32_t tag, std::uint32_t dataTransferLen,
                                            int direction, std::uint8_t lun,
                                            const std::uint8_t* cdb, int cdbLen) noexcept {
    std::array<std::uint8_t, 31> cbw{};
    writeLe32(cbw.data() + 0, kCbwSignature);
    writeLe32(cbw.data() + 4, tag);
    writeLe32(cbw.data() + 8, dataTransferLen);
    cbw[12] = (direction == 1) ? 0x80u : 0x00u;
    cbw[13] = static_cast<std::uint8_t>(lun & 0x0Fu);
    const int clampedLen = cdb ? std::clamp(cdbLen, 1, 16) : 1;
    cbw[14] = static_cast<std::uint8_t>(clampedLen);
    if (cdb && clampedLen > 0) {
        std::memcpy(cbw.data() + 15, cdb, static_cast<std::size_t>(clampedLen));
    }
    return cbw;
}

UsbBotCsw parseUsbBotCsw(const std::uint8_t* buf, std::size_t len) noexcept {
    UsbBotCsw csw;
    if (!buf || len < 13) {
        return csw;
    }
    csw.validSignature = (readLe32(buf + 0) == kCswSignature);
    csw.tag = readLe32(buf + 4);
    csw.dataResidue = readLe32(buf + 8);
    csw.status = buf[12];
    return csw;
}

UsbBotSession::UsbBotSession(IUsbBotPipe& pipe, std::uint8_t lun) noexcept
    : pipe_(pipe), lun_(lun) {}

bool UsbBotSession::reset(std::string& error) {
    if (!pipe_.bomsReset(error)) {
        return false;
    }
    std::string ignoreErr;
    (void)pipe_.clearHaltIn(ignoreErr);
    (void)pipe_.clearHaltOut(ignoreErr);
    return true;
}

int UsbBotSession::runRawTransaction(const std::uint8_t* cdb, int cdbLen, int direction,
                                     const std::uint8_t* dataIn, int dataInLen,
                                     std::uint8_t* dataOut, int dataOutMaxLen,
                                     std::uint8_t& outCswStatus, bool& outRetryableTransport,
                                     std::string& error) {
    outCswStatus = 0xFFu;
    outRetryableTransport = false;

    const std::uint32_t tag = nextTag_++;
    if (nextTag_ == 0) {
        nextTag_ = 1;
    }
    const std::uint32_t transferLen =
        direction == 1 ? static_cast<std::uint32_t>(std::max(0, dataOutMaxLen))
                       : (direction == 2 ? static_cast<std::uint32_t>(std::max(0, dataInLen)) : 0u);
    const auto cbw = buildUsbBotCbw(tag, transferLen, direction, lun_, cdb, cdbLen);

    int cbwWritten = 0;
    const UsbPipeStatus cbwSt =
        pipe_.bulkOut(cbw.data(), static_cast<int>(cbw.size()), cbwWritten, kUsbTimeoutMs, error);
    if (cbwSt != UsbPipeStatus::Ok || cbwWritten != static_cast<int>(cbw.size())) {
        if (cbwSt == UsbPipeStatus::Stall) {
            std::string rstErr;
            (void)pipe_.clearHaltOut(rstErr);
            (void)reset(rstErr);
            outRetryableTransport = true;
        } else if (error.empty()) {
            error = "USB BOT CBW transfer failed.";
        }
        return -1;
    }

    int dataTransferred = 0;
    if (direction == 1 && dataOutMaxLen > 0) {
        if (!dataOut) {
            error = "USB BOT missing IN output buffer.";
            return -1;
        }
        const UsbPipeStatus dataSt =
            pipe_.bulkIn(dataOut, dataOutMaxLen, dataTransferred, kUsbTimeoutMs, error);
        if (dataSt == UsbPipeStatus::Stall) {
            std::string clrErr;
            if (!pipe_.clearHaltIn(clrErr)) {
                error = clrErr;
                return -1;
            }
            dataTransferred = 0;
        } else if (dataSt != UsbPipeStatus::Ok) {
            return -1;
        }
    } else if (direction == 2 && dataInLen > 0) {
        if (!dataIn) {
            error = "USB BOT missing OUT input buffer.";
            return -1;
        }
        const UsbPipeStatus dataSt =
            pipe_.bulkOut(dataIn, dataInLen, dataTransferred, kUsbTimeoutMs, error);
        if (dataSt == UsbPipeStatus::Stall) {
            std::string clrErr;
            if (!pipe_.clearHaltOut(clrErr)) {
                error = clrErr;
                return -1;
            }
        } else if (dataSt != UsbPipeStatus::Ok) {
            return -1;
        }
    }

    std::array<std::uint8_t, 13> cswBuf{};
    int cswRead = 0;
    UsbPipeStatus cswSt =
        pipe_.bulkIn(cswBuf.data(), static_cast<int>(cswBuf.size()), cswRead, kUsbTimeoutMs, error);
    if (cswSt == UsbPipeStatus::Stall) {
        std::string clrErr;
        if (!pipe_.clearHaltIn(clrErr)) {
            error = clrErr;
            return -1;
        }
        cswRead = 0;
        cswSt = pipe_.bulkIn(cswBuf.data(), static_cast<int>(cswBuf.size()), cswRead, kUsbTimeoutMs,
                             error);
    }
    if (cswSt != UsbPipeStatus::Ok) {
        return -1;
    }

    const UsbBotCsw csw = parseUsbBotCsw(cswBuf.data(), static_cast<std::size_t>(cswRead));
    if (!csw.validSignature || csw.tag != tag) {
        std::string rstErr;
        (void)reset(rstErr);
        std::ostringstream oss;
        oss << "USB BOT invalid CSW (sigOk=" << (csw.validSignature ? 1 : 0)
            << ", expectedTag=" << tag << ", gotTag=" << csw.tag << ").";
        error = oss.str();
        return -1;
    }

    outCswStatus = csw.status;
    if (csw.status == 0) {
        return direction == 1 ? dataTransferred : (direction == 2 ? dataInLen : 0);
    }
    return -1;
}

int UsbBotSession::executeCommand(const std::uint8_t* cdb, int cdbLen, int direction,
                                  const std::uint8_t* dataIn, int dataInLen,
                                  std::uint8_t* dataOut, int dataOutMaxLen, std::string& error) {
    if (!cdb || cdbLen < 6) {
        error = "Invalid USB BOT CDB length.";
        return -1;
    }

    bool phaseErrorRetried = false;
    for (int attempt = 0; attempt <= kMaxUnitReadyRetries; ++attempt) {
        std::uint8_t cswStatus = 0xFFu;
        bool retryTransport = false;
        const int rc = runRawTransaction(cdb, cdbLen, direction, dataIn, dataInLen, dataOut,
                                         dataOutMaxLen, cswStatus, retryTransport, error);
        if (rc >= 0) {
            return rc;
        }
        if (retryTransport && attempt < kMaxUnitReadyRetries) {
            continue;
        }
        if (cswStatus == 2u) { // Phase Error -> BOMSR reset & retry once
            std::string rstErr;
            if (!reset(rstErr)) {
                error = rstErr;
                return -1;
            }
            if (!phaseErrorRetried && attempt < kMaxUnitReadyRetries) {
                phaseErrorRetried = true;
                continue;
            }
            error = "USB BOT persistent Phase Error.";
            return -1;
        }
        if (cswStatus == 1u) { // Command Failed -> query REQUEST SENSE directly
            if (cdb[0] == 0x03u) {
                error = "USB BOT REQUEST SENSE command failed.";
                return -1;
            }
            std::array<std::uint8_t, 12> senseCdb{};
            senseCdb[0] = 0x03u;
            senseCdb[4] = 18u;
            std::array<std::uint8_t, 18> senseBuf{};
            std::uint8_t senseCswStatus = 0xFFu;
            bool senseRetry = false;
            std::string senseErr;
            const int senseLen = runRawTransaction(senseCdb.data(), 12, 1, nullptr, 0,
                                                   senseBuf.data(), static_cast<int>(senseBuf.size()),
                                                   senseCswStatus, senseRetry, senseErr);
            if (senseLen < 0) {
                error = senseErr.empty() ? "USB BOT REQUEST SENSE failed after CSW status=1."
                                         : senseErr;
                return -1;
            }
            const auto parsed =
                parseAndClassifyScsiSense(senseBuf.data(), static_cast<std::size_t>(senseLen));
            if (parsed.disposition == ScsiSenseDisposition::RetryUnitAttention &&
                attempt < kMaxUnitReadyRetries) {
                continue;
            }
            if (parsed.disposition == ScsiSenseDisposition::RetryBecomingReady &&
                attempt < kMaxUnitReadyRetries) {
                pipe_.sleepMs(150);
                continue;
            }
            error = formatScsiSenseError("USB BOT", parsed);
            return -1;
        }
        return -1;
    }

    error = "USB BOT exceeded retry limit waiting for device readiness.";
    return -1;
}

// ============================================================================
// Physical Device Discovery & RAII Handle
// ============================================================================

struct PhysicalHiMdHandle::Impl {
    std::mutex mutex;
    std::unique_ptr<ScsiBackend> backend;
};

PhysicalHiMdHandle::PhysicalHiMdHandle(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

PhysicalHiMdHandle::~PhysicalHiMdHandle() = default;

int PhysicalHiMdHandle::reset(std::string& error) {
    if (!impl_ || !impl_->backend) {
        error = "Physical Hi-MD handle is closed.";
        return -1;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->backend->reset(error);
}

int PhysicalHiMdHandle::executeCommand(const std::uint8_t* cdb, int cdbLen, int direction,
                                       const std::uint8_t* dataIn, int dataInLen,
                                       std::uint8_t* dataOut, int dataOutMaxLen,
                                       std::string& error) {
    if (!impl_ || !impl_->backend) {
        error = "Physical Hi-MD handle is closed.";
        return -1;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->backend->execute(cdb, cdbLen, direction, dataIn, dataInLen, dataOut,
                                   dataOutMaxLen, error);
}

std::vector<HiMdPhysicalDeviceInfo> listPhysicalHiMdDevices() {
    std::vector<HiMdPhysicalDeviceInfo> out;
    std::set<std::pair<int, int>> coveredBusAddrs;
    std::set<std::pair<std::uint16_t, std::uint16_t>> coveredVidPids;

#if defined(__APPLE__)
    const auto macMatches = scanMacHiMdUsbServices();
    for (const auto& m : macMatches) {
        HiMdPhysicalDeviceInfo info;
        info.id = m.id;
        info.vendorId = m.vendorId;
        info.productId = m.productId;
        info.name = m.name;
        info.transport = (m.scsiService != IO_OBJECT_NULL) ? "iokit-scsi" : "macos-usb-bot";
        info.systemPath = m.bsdWholeDisk.empty() ? m.id : ("/dev/" + m.bsdWholeDisk);
        out.push_back(std::move(info));
        coveredVidPids.insert({m.vendorId, m.productId});
    }
#elif defined(__linux__)
    const auto sgDevs = scanLinuxSgDevices();
    for (const auto& sg : sgDevs) {
        out.push_back(sg.info);
        if (sg.bus > 0 && sg.addr > 0) {
            coveredBusAddrs.insert({sg.bus, sg.addr});
        }
    }
#elif defined(_WIN32)
    const auto winDevs = scanWindowsPhysicalDrives();
    for (const auto& wd : winDevs) {
        out.push_back(wd);
        coveredVidPids.insert({wd.vendorId, wd.productId});
    }
#endif

    for (auto& usbDev : enumerateLibusbHiMdDevices(coveredBusAddrs)) {
        if (coveredVidPids.count({usbDev.vendorId, usbDev.productId}) > 0) {
            continue;
        }
        out.push_back(std::move(usbDev));
    }
    return out;
}

std::unique_ptr<PhysicalHiMdHandle> PhysicalHiMdHandle::open(const std::string& deviceId,
                                                             std::string& error) {
    if (deviceId.empty()) {
        error = "Hi-MD device ID is empty.";
        return nullptr;
    }

    int usbBus = -1;
    int usbAddr = -1;
    if (parseUsbDeviceId(deviceId, usbBus, usbAddr)) {
        auto backend = openLibusbBotBackend(0, 0, usbBus, usbAddr, 0, {}, error);
        if (!backend) {
            return nullptr;
        }
        auto impl = std::make_unique<Impl>();
        impl->backend = std::move(backend);
        return std::make_unique<PhysicalHiMdHandle>(std::move(impl));
    }

#if defined(__APPLE__)
    if (deviceId.rfind("macos:", 0) == 0) {
        auto containsInsensitive = [](const std::string& hay, const char* needle) {
            if (!needle || needle[0] == '\0') {
                return false;
            }
            const std::size_t nLen = std::strlen(needle);
            if (hay.size() < nLen) {
                return false;
            }
            for (std::size_t i = 0; i + nLen <= hay.size(); ++i) {
                bool match = true;
                for (std::size_t j = 0; j < nLen; ++j) {
                    if (std::tolower(static_cast<unsigned char>(hay[i + j])) !=
                        std::tolower(static_cast<unsigned char>(needle[j]))) {
                        match = false;
                        break;
                    }
                }
                if (match) {
                    return true;
                }
            }
            return false;
        };
        auto isTransientMacReenumError = [&](const std::string& msg) {
            if (containsInsensitive(msg, "access denied") ||
                containsInsensitive(msg, "insufficient permissions")) {
                return false;
            }
            return containsInsensitive(msg, "no longer connected") ||
                   containsInsensitive(msg, "entity not found") ||
                   containsInsensitive(msg, "resource busy") ||
                   containsInsensitive(msg, "no such device") ||
                   containsInsensitive(msg, "pipe error") ||
                   containsInsensitive(msg, "not found");
        };

        for (int attempt = 0; attempt < 10; ++attempt) {
            bool foundMatch = false;
            error.clear();
            auto macMatches = scanMacHiMdUsbServices();
            for (auto& m : macMatches) {
                if (m.id != deviceId) {
                    continue;
                }
                foundMatch = true;
                bool didUnmount = false;
                if (!m.bsdWholeDisk.empty()) {
                    std::string daErr;
                    (void)setBsdWholeDiskMountState(m.bsdWholeDisk, false, didUnmount, daErr);
                }
                if (m.scsiService != IO_OBJECT_NULL) {
                    IOCFPlugInInterface** plugIn = nullptr;
                    SInt32 score = 0;
                    IOReturn kr = IOCreatePlugInInterfaceForService(
                        m.scsiService, kIOSCSITaskDeviceUserClientTypeID, kIOCFPlugInInterfaceID,
                        &plugIn, &score);
                    if (kr == kIOReturnSuccess && plugIn) {
                        SCSITaskDeviceInterface** devIf = nullptr;
                        const HRESULT hr = (*plugIn)->QueryInterface(
                            plugIn, CFUUIDGetUUIDBytes(kIOSCSITaskDeviceInterfaceID),
                            reinterpret_cast<LPVOID*>(&devIf));
                        if (hr == S_OK && devIf) {
                            kr = (*devIf)->ObtainExclusiveAccess(devIf);
                            if (kr == kIOReturnSuccess) {
                                auto impl = std::make_unique<Impl>();
                                impl->backend =
                                    std::make_unique<MacScsiTaskBackend>(plugIn, devIf);
                                return std::make_unique<PhysicalHiMdHandle>(std::move(impl));
                            }
                            (*devIf)->Release(devIf);
                        }
                        IODestroyPlugInInterface(plugIn);
                    }
                }
                // Path B: fall back to libusb USB Mass Storage BOT
                auto backend = openLibusbBotBackend(m.vendorId, m.productId, -1, -1, m.locationId,
                                                    m.bsdWholeDisk, error);
                if (!backend) {
                    if (didUnmount) {
                        bool ignored = false;
                        std::string remountErr;
                        (void)setBsdWholeDiskMountState(m.bsdWholeDisk, true, ignored, remountErr);
                    }
                    break;
                }
                auto impl = std::make_unique<Impl>();
                impl->backend = std::move(backend);
                return std::make_unique<PhysicalHiMdHandle>(std::move(impl));
            }
            if (attempt < 9 && (!foundMatch || isTransientMacReenumError(error))) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                continue;
            }
            if (!foundMatch && error.empty()) {
                error = "Physical Hi-MD device not found: " + deviceId;
            }
            return nullptr;
        }
        if (error.empty()) {
            error = "Physical Hi-MD device not found: " + deviceId;
        }
        return nullptr;
    }
#elif defined(__linux__)
    if (deviceId.rfind("sg:", 0) == 0) {
        const std::string sgPath = deviceId.substr(3);
        const auto sgDevs = scanLinuxSgDevices();
        const LinuxSgDeviceInfo* found = nullptr;
        for (const auto& entry : sgDevs) {
            if (entry.sgPath == sgPath) {
                found = &entry;
                break;
            }
        }
        if (!found) {
            error = "Linux SCSI generic Hi-MD device not found: " + sgPath;
            return nullptr;
        }
        const int fd = ::open(sgPath.c_str(), O_RDWR);
        if (fd >= 0) {
            auto impl = std::make_unique<Impl>();
            impl->backend =
                std::make_unique<LinuxSgIoBackend>(fd, sgPath, found->blockDevName);
            return std::make_unique<PhysicalHiMdHandle>(std::move(impl));
        }
        const int openErr = errno;
        if (found->bus > 0 && found->addr > 0) {
            std::string botErr;
            auto backend = openLibusbBotBackend(found->info.vendorId, found->info.productId,
                                                found->bus, found->addr, 0, {}, botErr);
            if (backend) {
                auto impl = std::make_unique<Impl>();
                impl->backend = std::move(backend);
                return std::make_unique<PhysicalHiMdHandle>(std::move(impl));
            }
            error = "Failed opening " + sgPath + " (" + std::strerror(openErr) +
                    ") and libusb BOT fallback (" + botErr + ").";
            return nullptr;
        }
        error = "Failed opening " + sgPath + ": " + std::strerror(openErr);
        return nullptr;
    }
#elif defined(_WIN32)
    if (deviceId.rfind("spti:PhysicalDrive", 0) == 0) {
        const std::string driveName = deviceId.substr(5);
        unsigned long driveNum = 0;
        if (std::sscanf(driveName.c_str(), "PhysicalDrive%lu", &driveNum) != 1) {
            error = "Malformed Windows PhysicalDrive Hi-MD ID: " + deviceId;
            return nullptr;
        }
        wchar_t winPath[64];
        std::swprintf(winPath, 64, L"\\\\.\\PhysicalDrive%lu", driveNum);
        HANDLE hDisk = CreateFileW(winPath, GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                                   nullptr);
        if (hDisk == INVALID_HANDLE_VALUE) {
            const DWORD errCode = GetLastError();
            std::ostringstream oss;
            oss << "Failed opening \\\\.\\" << driveName << " for SCSI pass-through (error "
                << errCode << ").";
            if (errCode == ERROR_ACCESS_DENIED) {
                oss << " Run as Administrator or use WinUSB/libusb mode for direct SCSI access.";
            }
            error = oss.str();
            return nullptr;
        }
        auto impl = std::make_unique<Impl>();
        impl->backend = std::make_unique<WinSptiBackend>(hDisk, static_cast<ULONG>(driveNum));
        return std::make_unique<PhysicalHiMdHandle>(std::move(impl));
    }
#endif

    error = "Unsupported or unknown Hi-MD device ID: " + deviceId;
    return nullptr;
}

} // namespace carta
