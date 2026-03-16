/*
 * Copyright 2026 Oliver Larkin
 *
 * SUSHI is free software: you can redistribute it and/or modify it under the terms of
 * the GNU Affero General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * SUSHI is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE. See the GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License along with
 * SUSHI. If not, see http://www.gnu.org/licenses/
 */

import Cocoa
import SwiftUI
import FaustSwiftUI

// MARK: - View Model

class SushiFaustViewModel: FaustUIValueBinding {
    private var zones: [String: UnsafeMutablePointer<Float>] = [:]

    func setZone(_ address: String, pointer: UnsafeMutablePointer<Float>) {
        zones[address] = pointer
    }

    func clearZones() {
        zones.removeAll()
    }

    func getValue(for address: String, default defaultValue: Double) -> Double {
        guard let zone = zones[address] else {
            return defaultValue
        }
        return Double(zone.pointee)
    }

    func setValue(_ value: Double, for address: String) {
        guard let zone = zones[address] else {
            return
        }
        objectWillChange.send()
        zone.pointee = Float(value)
    }
}

// MARK: - C Bridging Functions

@_cdecl("SushiFaustViewModel_create")
func SushiFaustViewModel_create() -> UnsafeMutableRawPointer {
    let vm = SushiFaustViewModel()
    return Unmanaged.passRetained(vm).toOpaque()
}

@_cdecl("SushiFaustViewModel_setZone")
func SushiFaustViewModel_setZone(_ vmPtr: UnsafeMutableRawPointer,
                                  _ address: UnsafePointer<CChar>,
                                  _ zone: UnsafeMutablePointer<Float>) {
    let vm = Unmanaged<SushiFaustViewModel>.fromOpaque(vmPtr).takeUnretainedValue()
    vm.setZone(String(cString: address), pointer: zone)
}

@_cdecl("SushiFaustViewModel_clearZones")
func SushiFaustViewModel_clearZones(_ vmPtr: UnsafeMutableRawPointer) {
    let vm = Unmanaged<SushiFaustViewModel>.fromOpaque(vmPtr).takeUnretainedValue()
    vm.clearZones()
}

@_cdecl("SushiFaustViewModel_release")
func SushiFaustViewModel_release(_ vmPtr: UnsafeMutableRawPointer) {
    Unmanaged<SushiFaustViewModel>.fromOpaque(vmPtr).release()
}

// MARK: - Editor View

@_cdecl("SushiFaustEditorView_create")
func SushiFaustEditorView_create(_ jsonCStr: UnsafePointer<CChar>,
                                  _ vmPtr: UnsafeMutableRawPointer) -> UnsafeMutableRawPointer? {
    let json = String(cString: jsonCStr)
    let vm = Unmanaged<SushiFaustViewModel>.fromOpaque(vmPtr).takeUnretainedValue()

    // Parse the full JSONUI output using FaustSwiftUI's decoder
    guard let jsonData = json.data(using: .utf8),
          let faustJSON = try? JSONDecoder().decode(FaustUIJSON.self, from: jsonData),
          let uiItems = faustJSON.ui, !uiItems.isEmpty else {
        return nil
    }

    // Compute intrinsic size from content without ScrollView
    let contentView = FaustUIView(ui: uiItems, viewModel: vm, scrollable: false)
    let sizingView = NSHostingView(rootView: contentView)
    let fittingSize = sizingView.fittingSize

    let faustView = FaustUIView(ui: uiItems, viewModel: vm, scrollable: false)
    let hostingView = NSHostingView(rootView: faustView)

    let width = min(max(Int(fittingSize.width), 400), 1200)
    let height = min(max(Int(fittingSize.height), 300), 900)
    hostingView.frame = NSRect(x: 0, y: 0, width: width, height: height)

    return Unmanaged.passRetained(hostingView).toOpaque()
}

@_cdecl("SushiFaustEditorView_getSize")
func SushiFaustEditorView_getSize(_ viewPtr: UnsafeMutableRawPointer,
                                   _ width: UnsafeMutablePointer<Int32>,
                                   _ height: UnsafeMutablePointer<Int32>) {
    let view = Unmanaged<NSView>.fromOpaque(viewPtr).takeUnretainedValue()
    width.pointee = Int32(view.frame.size.width)
    height.pointee = Int32(view.frame.size.height)
}

@_cdecl("SushiFaustEditorView_release")
func SushiFaustEditorView_release(_ viewPtr: UnsafeMutableRawPointer) {
    Unmanaged<NSView>.fromOpaque(viewPtr).release()
}
