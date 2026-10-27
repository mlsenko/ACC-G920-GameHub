import Foundation
import IOKit
import IOKit.hid

let manager = IOHIDManagerCreate(kCFAllocatorDefault, IOOptionBits(kIOHIDOptionsTypeNone))
IOHIDManagerSetDeviceMatching(manager, [
    kIOHIDVendorIDKey: 0x046D,
    kIOHIDProductIDKey: 0xC262,
] as CFDictionary)
IOHIDManagerScheduleWithRunLoop(manager, CFRunLoopGetMain(), CFRunLoopMode.defaultMode.rawValue)
guard IOHIDManagerOpen(manager, IOOptionBits(kIOHIDOptionsTypeNone)) == kIOReturnSuccess,
      let dev = (IOHIDManagerCopyDevices(manager) as? Set<IOHIDDevice>)?.first,
      IOHIDDeviceOpen(dev, IOOptionBits(kIOHIDOptionsTypeNone)) == kIOReturnSuccess else {
    fputs("no G920\n", stderr)
    exit(1)
}

final class Box: @unchecked Sendable {
    var reports: [[UInt8]] = []
    let lock = NSLock()
}
let box = Box()
let callback: IOHIDReportCallback = { ctx, _, _, _, reportID, reportPtr, reportLength in
    let box = Unmanaged<Box>.fromOpaque(ctx!).takeUnretainedValue()
    var bytes = [UInt8](UnsafeBufferPointer(start: reportPtr, count: Int(reportLength)))
    if bytes.first != UInt8(reportID) {
        bytes.insert(UInt8(reportID), at: 0)
    }
    box.lock.lock()
    if bytes.count > 0 && (bytes[0] == 0x10 || bytes[0] == 0x11 || bytes[0] == 0x12) {
        box.reports.append(bytes)
    }
    box.lock.unlock()
}
let buffer = UnsafeMutablePointer<UInt8>.allocate(capacity: 64)
IOHIDDeviceRegisterInputReportCallback(dev, buffer, 64, callback, Unmanaged.passUnretained(box).toOpaque())
IOHIDDeviceScheduleWithRunLoop(dev, CFRunLoopGetMain(), CFRunLoopMode.defaultMode.rawValue)

func pump(_ seconds: Double) {
    RunLoop.current.run(until: Date().addingTimeInterval(seconds))
}

@discardableResult
func setReport(_ bytes: [UInt8]) -> Int32 {
    bytes.withUnsafeBufferPointer { ptr in
        IOHIDDeviceSetReport(dev, kIOHIDReportTypeOutput, CFIndex(bytes[0]), ptr.baseAddress!, ptr.count)
    }
}

func waitResponse(match: (UInt8, UInt8), timeout: Double) -> [UInt8]? {
    let deadline = Date().addingTimeInterval(timeout)
    while Date() < deadline {
        box.lock.lock()
        if let idx = box.reports.lastIndex(where: { r in
            r.count >= 4 && r[2] == match.0 && r[3] == match.1
        }) {
            let r = box.reports[idx]
            box.reports.remove(at: idx)
            box.lock.unlock()
            return r
        }
        box.lock.unlock()
        pump(0.01)
    }
    return nil
}

func hidpp(feature: UInt8, function: UInt8, params: [UInt8], veryLong: Bool = false) -> [UInt8]? {
    let funcByte = (function << 4) | 0x01
    var report = [UInt8](repeating: 0, count: veryLong ? 64 : 20)
    report[0] = veryLong ? 0x12 : 0x11
    report[1] = 0xFF
    report[2] = feature
    report[3] = funcByte
    for (i, b) in params.enumerated() where 4 + i < report.count {
        report[4 + i] = b
    }
    let kr = setReport(report)
    let resp = waitResponse(match: (feature, funcByte), timeout: 1.0)
    let hex = report.prefix(8).map { String(format: "%02x", $0) }.joined(separator: " ")
    let rhex = resp.map { $0.prefix(12).map { String(format: "%02x", $0) }.joined(separator: " ") } ?? "timeout"
    print(String(format: "set=%d req %@ -> %@", kr, hex, rhex))
    return resp
}

print("ping protocol")
_ = hidpp(feature: 0x00, function: 0x01, params: [0x00, 0x00, 0x5A])
print("lookup 0x8123")
guard let found = hidpp(feature: 0x00, function: 0x00, params: [0x81, 0x23]), found.count >= 6, found[4] != 0 else {
    print("feature 0x8123 missing")
    exit(2)
}
let ff = found[4]
print("ff feature index \(ff) type \(found[5])")
print("getInfo")
_ = hidpp(feature: ff, function: 0x00, params: [])
print("resetAll")
_ = hidpp(feature: ff, function: 0x01, params: [])
print("setGain full")
_ = hidpp(feature: ff, function: 0x08, params: [0xFF, 0xFF, 0x00, 0x00])
print("setAperture 900")
_ = hidpp(feature: ff, function: 0x06, params: [0x03, 0x84])

func constant(level: Int16, start: Bool) {
    var p = [UInt8](repeating: 0, count: 14)
    p[0] = 0 // new effect
    p[1] = (start ? 0x80 : 0) | 0x00 // constant, axis 0
    let mag = UInt16(bitPattern: level)
    p[6] = UInt8(mag >> 8)
    p[7] = UInt8(mag & 0xFF)
    print("constant level \(level)")
    _ = hidpp(feature: ff, function: 0x02, params: p)
}

print("tug right")
constant(level: 9000, start: true)
pump(1.2)
print("tug left")
constant(level: -9000, start: true)
pump(1.2)
print("release")
_ = hidpp(feature: ff, function: 0x01, params: [])
print("done")
