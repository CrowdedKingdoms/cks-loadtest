// Moves another process's window and brings it to the front, for wall.py (macOS, under Accessibility for the app this runs in).
//   window place <pid> <x> <y> <w> <h>    the window's frame, in points from the main display's top left
//   window front <pid>                    in front of every other app's windows
import ApplicationServices
import Cocoa

func mainWindow(_ pid: pid_t) -> AXUIElement? {
    let app = AXUIElementCreateApplication(pid)
    var value: CFTypeRef?
    guard AXUIElementCopyAttributeValue(app, kAXWindowsAttribute as CFString, &value) == .success, let windows = value as? [AXUIElement] else { return nil }
    return windows.first
}

let args = CommandLine.arguments
guard args.count >= 3, let pid = pid_t(args[2]) else {
    print("usage: window place <pid> <x> <y> <w> <h> | front <pid>")
    exit(2)
}
guard AXIsProcessTrusted() else {
    print("not trusted for Accessibility")
    exit(3)
}
guard let window = mainWindow(pid) else {
    print("no window")
    exit(1)
}
switch args[1] {
case "place":
    guard args.count == 7, let x = Double(args[3]), let y = Double(args[4]), let w = Double(args[5]), let h = Double(args[6]) else { exit(2) }
    var origin = CGPoint(x: x, y: y)
    var size = CGSize(width: w, height: h)
    let position = AXValueCreate(.cgPoint, &origin)!
    let extent = AXValueCreate(.cgSize, &size)!
    // Size, then place, then size again: a window moved onto a larger part of the screen may only then take the size.
    AXUIElementSetAttributeValue(window, kAXSizeAttribute as CFString, extent)
    let moved = AXUIElementSetAttributeValue(window, kAXPositionAttribute as CFString, position)
    let sized = AXUIElementSetAttributeValue(window, kAXSizeAttribute as CFString, extent)
    exit(moved == .success && sized == .success ? 0 : 1)
case "front":
    NSRunningApplication(processIdentifier: pid)?.activate(options: [.activateAllWindows])
    AXUIElementSetAttributeValue(AXUIElementCreateApplication(pid), kAXFrontmostAttribute as CFString, kCFBooleanTrue)
    AXUIElementPerformAction(window, kAXRaiseAction as CFString)
    exit(0)
default:
    exit(2)
}
