import Foundation
import IOKit
import IOKit.hid
import GameController

var reportCounts: [UInt32: Int] = [:]
var sampleReports: [UInt32: String] = [:]

let manager = IOHIDManagerCreate(kCFAllocatorDefault, IOOptionBits(kIOHIDOptionsTypeNone))
IOHIDManagerSetDeviceMatching(manager, [
    kIOHIDVendorIDKey: 0x046D,
    kIOHIDProductIDKey: 0xC262,
] as CFDictionary)
IOHIDManagerScheduleWithRunLoop(manager, CFRunLoopGetMain(), CFRunLoopMode.defaultMode.rawValue)
print("open manager=\(IOHIDManagerOpen(manager, IOOptionBits(kIOHIDOptionsTypeNone)))")

let devices = (IOHIDManagerCopyDevices(manager) as? Set<IOHIDDevice>) ?? []
print("hid_device_count=\(devices.count)")
print("gc_controllers=\(GCController.controllers().count)")

guard let dev = devices.first else { exit(1) }
print("device open=\(IOHIDDeviceOpen(dev, IOOptionBits(kIOHIDOptionsTypeNone)))")

let callback: IOHIDReportCallback = { _, result, _, _, reportID, reportPtr, reportLength in
    reportCounts[reportID, default: 0] += 1
    if sampleReports[reportID] == nil {
        let bytes = UnsafeBufferPointer(start: reportPtr, count: min(Int(reportLength), 16))
        let hex = bytes.map { String(format: "%02x", $0) }.joined(separator: " ")
        sampleReports[reportID] = "result=\(result) len=\(reportLength) \(hex)"
    }
}
let buffer = UnsafeMutablePointer<UInt8>.allocate(capacity: 64)
IOHIDDeviceRegisterInputReportCallback(dev, buffer, 64, callback, nil)
IOHIDDeviceScheduleWithRunLoop(dev, CFRunLoopGetMain(), CFRunLoopMode.defaultMode.rawValue)

func num(_ key: String) -> Int {
    (IOHIDDeviceGetProperty(dev, key as CFString) as? NSNumber)?.intValue ?? -1
}

struct Axis {
    let name: String
    let cookie: IOHIDElementCookie
    let element: IOHIDElement
}

var axes: [Axis] = []
if let elements = IOHIDDeviceCopyMatchingElements(dev, nil, IOOptionBits(0)) as? [IOHIDElement] {
    for el in elements {
        let usagePage = IOHIDElementGetUsagePage(el)
        let usage = IOHIDElementGetUsage(el)
        let type = IOHIDElementGetType(el)
        let cookie = IOHIDElementGetCookie(el)
        if usagePage == 0x01 && [0x30, 0x31, 0x32, 0x35, 0x39].contains(usage) {
            let names = [0x30: "X", 0x31: "Y", 0x32: "Z", 0x35: "Rz", 0x39: "hat"]
            let name = names[Int(usage)] ?? "u\(usage)"
            axes.append(Axis(name: name, cookie: cookie, element: el))
            print("element \(name) cookie=\(cookie) type=\(type.rawValue) min=\(IOHIDElementGetLogicalMin(el)) max=\(IOHIDElementGetLogicalMax(el))")
        }
    }
    print("element_count=\(elements.count) axes=\(axes.count)")
}

let value = UnsafeMutablePointer<Unmanaged<IOHIDValue>>.allocate(capacity: 1)
for tick in 0..<8 {
    var parts: [String] = []
    for axis in axes {
        let kr = IOHIDDeviceGetValue(dev, axis.element, value)
        let raw = kr == kIOReturnSuccess ? IOHIDValueGetIntegerValue(value.pointee.takeUnretainedValue()) : -1
        parts.append("\(axis.name)=\(raw)(\(kr))")
    }
    print("t=\(tick) " + parts.joined(separator: " "))
    RunLoop.current.run(until: Date().addingTimeInterval(0.4))
}

print("reports:")
if reportCounts.isEmpty { print("  none") }
for (id, count) in reportCounts.sorted(by: { $0.key < $1.key }) {
    print("  id=\(id) count=\(count) sample=\(sampleReports[id] ?? "")")
}
print("primaryUsagePage=\(num(kIOHIDPrimaryUsagePageKey)) primaryUsage=\(num(kIOHIDPrimaryUsageKey))")
