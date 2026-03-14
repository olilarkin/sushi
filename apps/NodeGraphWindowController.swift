/*
 * Copyright 2017-2023 Elk Audio AB
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

@objc(NodeGraphWindowController) class NodeGraphWindowController: NSObject
{
    private var window: NSWindow?
    private var model: NodeGraphModel?
    private let bridge: SushiGraphBridge

    @objc init(bridge: SushiGraphBridge)
    {
        self.bridge = bridge
        super.init()
    }

    @objc func showWindow()
    {
        if let existing = window
        {
            existing.makeKeyAndOrderFront(nil)
            NSApp.activate(ignoringOtherApps: true)
            return
        }

        let model = NodeGraphModel(bridge: bridge)
        self.model = model

        let view = NodeGraphView(model: model)
        let hostingController = NSHostingController(rootView: view)

        let window = NSWindow(contentViewController: hostingController)
        window.title = "Audio Graph"
        window.styleMask = [.titled, .closable, .resizable, .miniaturizable, .unifiedTitleAndToolbar]
        window.titleVisibility = .hidden
        window.titlebarAppearsTransparent = true
        window.toolbarStyle = .unifiedCompact
        window.setFrameAutosaveName("NodeGraphWindow")
        window.center()
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)

        self.window = window
    }

    @objc func teardown()
    {
        window?.close()
        window = nil
        model = nil
    }
}
