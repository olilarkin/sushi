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

import SwiftUI

// MARK: - Layout constants

private enum Layout
{
    static let nodeWidth: CGFloat = 150
    static let nodeHeight: CGFloat = 52
    static let nodeHSpacing: CGFloat = 40
    static let trackPadding: CGFloat = 24
    static let trackVSpacing: CGFloat = 28
    static let endpointWidth: CGFloat = 86
    static let endpointHeight: CGFloat = 34
    static let cornerRadius: CGFloat = 10
    static let endpointCornerRadius: CGFloat = 8
    static let titleBarHeight: CGFloat = 22
    static let canvasPadding: CGFloat = 20
    static let wireStrokeWidth: CGFloat = 2.0
    static let portDotRadius: CGFloat = 4.0
    static let midiDash: [CGFloat] = [6, 4]
    static let mixerWidth: CGFloat = 120
    static let mixerHeight: CGFloat = 52
    static let mixerHSpacing: CGFloat = 16
}

// MARK: - Layout model

private struct NodeRect: Identifiable
{
    let id: Int
    let name: String
    let pluginType: String
    let hasEditor: Bool
    let rect: CGRect
}

private struct EndpointRect
{
    let label: String
    let rect: CGRect
    let isMidi: Bool
}

private struct TrackLayout
{
    let trackId: Int
    let name: String
    let boundingBox: CGRect
    let hueOffset: Double
    let nodes: [NodeRect]
    let audioInEndpoint: EndpointRect?
    let audioOutEndpoint: EndpointRect?
    let midiInEndpoints: [EndpointRect]
    let mixerRect: CGRect
    let hasPan: Bool
}

private struct GraphLayout
{
    let tracks: [TrackLayout]
    let totalSize: CGSize
}

private func computeLayout(model: NodeGraphModel) -> GraphLayout
{
    var trackLayouts: [TrackLayout] = []
    var currentY: CGFloat = Layout.canvasPadding

    for (trackIndex, track) in model.tracks.enumerated()
    {
        let trackInputs = model.audioInputs.filter { $0.trackId == track.id }
        let trackOutputs = model.audioOutputs.filter { $0.trackId == track.id }
        let trackMidi = model.midiKbdInputs.filter { $0.trackId == track.id }

        let hasAudioIn = !trackInputs.isEmpty
        let hasAudioOut = !trackOutputs.isEmpty
        let hasMidiIn = !trackMidi.isEmpty

        let endpointGap: CGFloat = Layout.nodeHSpacing
        var procStartX = Layout.canvasPadding
        if hasAudioIn
        {
            procStartX += Layout.endpointWidth + endpointGap
        }

        let midiRowHeight: CGFloat = hasMidiIn
            ? Layout.endpointHeight + 12
            : 0
        let procY = currentY + midiRowHeight

        // Place processor nodes
        var nodes: [NodeRect] = []
        var x = procStartX + Layout.trackPadding
        for proc in track.processors
        {
            let rect = CGRect(
                x: x,
                y: procY + Layout.trackPadding,
                width: Layout.nodeWidth,
                height: Layout.nodeHeight
            )
            nodes.append(NodeRect(
                id: proc.id,
                name: proc.name,
                pluginType: proc.pluginType,
                hasEditor: proc.hasEditor,
                rect: rect
            ))
            x += Layout.nodeWidth + Layout.nodeHSpacing
        }

        // Track bounding box
        let nodesWidth: CGFloat
        if nodes.isEmpty
        {
            nodesWidth = Layout.nodeWidth + Layout.trackPadding * 2
        }
        else
        {
            nodesWidth = (nodes.last!.rect.maxX - procStartX - Layout.trackPadding) + Layout.trackPadding * 2
        }
        let innerWidth = nodesWidth + Layout.mixerHSpacing + Layout.mixerWidth + Layout.trackPadding
        let boxHeight = Layout.nodeHeight + Layout.trackPadding * 2
        let boundingBox = CGRect(
            x: procStartX,
            y: procY,
            width: innerWidth,
            height: boxHeight
        )

        // Mixer rect inside the track box, right side
        let mixerRect = CGRect(
            x: procStartX + nodesWidth + Layout.mixerHSpacing,
            y: procY + Layout.trackPadding,
            width: Layout.mixerWidth,
            height: Layout.mixerHeight
        )

        // Audio In endpoint
        var audioInEndpoint: EndpointRect? = nil
        if hasAudioIn
        {
            let channels = Set(trackInputs.map { $0.engineChannel }).sorted()
            let label = "In \(channelRangeLabel(channels))"
            let epRect = CGRect(
                x: Layout.canvasPadding,
                y: boundingBox.midY - Layout.endpointHeight / 2,
                width: Layout.endpointWidth,
                height: Layout.endpointHeight
            )
            audioInEndpoint = EndpointRect(label: label, rect: epRect, isMidi: false)
        }

        // Audio Out endpoint
        var audioOutEndpoint: EndpointRect? = nil
        if hasAudioOut
        {
            let channels = Set(trackOutputs.map { $0.engineChannel }).sorted()
            let label = "Out \(channelRangeLabel(channels))"
            let epRect = CGRect(
                x: boundingBox.maxX + endpointGap,
                y: boundingBox.midY - Layout.endpointHeight / 2,
                width: Layout.endpointWidth,
                height: Layout.endpointHeight
            )
            audioOutEndpoint = EndpointRect(label: label, rect: epRect, isMidi: false)
        }

        // MIDI In endpoints
        var midiInEndpoints: [EndpointRect] = []
        if hasMidiIn
        {
            let byPort = Dictionary(grouping: trackMidi, by: { $0.port })
            var midiX = procStartX
            for port in byPort.keys.sorted()
            {
                let conns = byPort[port]!
                let chLabel = midiChannelLabel(conns.first!.channel)
                let label = "MIDI \(port + 1) \(chLabel)"
                let epRect = CGRect(
                    x: midiX,
                    y: currentY,
                    width: Layout.endpointWidth,
                    height: Layout.endpointHeight
                )
                midiInEndpoints.append(EndpointRect(label: label, rect: epRect, isMidi: true))
                midiX += Layout.endpointWidth + 12
            }
        }

        let hue = Double(trackIndex) * 0.618033988749895
        trackLayouts.append(TrackLayout(
            trackId: track.id,
            name: track.name,
            boundingBox: boundingBox,
            hueOffset: hue.truncatingRemainder(dividingBy: 1.0),
            nodes: nodes,
            audioInEndpoint: audioInEndpoint,
            audioOutEndpoint: audioOutEndpoint,
            midiInEndpoints: midiInEndpoints,
            mixerRect: mixerRect,
            hasPan: track.hasPan
        ))

        let rowBottom = max(boundingBox.maxY, audioInEndpoint?.rect.maxY ?? 0, audioOutEndpoint?.rect.maxY ?? 0)
        currentY = rowBottom + Layout.trackVSpacing
    }

    var maxX: CGFloat = 400
    let maxY: CGFloat = currentY + Layout.canvasPadding
    for tl in trackLayouts
    {
        maxX = max(maxX, tl.boundingBox.maxX + Layout.canvasPadding)
        if let ao = tl.audioOutEndpoint
        {
            maxX = max(maxX, ao.rect.maxX + Layout.canvasPadding)
        }
    }

    return GraphLayout(tracks: trackLayouts, totalSize: CGSize(width: maxX, height: maxY))
}

private func channelRangeLabel(_ channels: [Int]) -> String
{
    guard let first = channels.first, let last = channels.last else { return "" }
    if first == last { return "\(first + 1)" }
    return "\(first + 1)-\(last + 1)"
}

private func midiChannelLabel(_ ch: Int) -> String
{
    if ch >= 16 { return "Omni" }
    return "CH\(ch + 1)"
}

// MARK: - Text cache

private class TextCache: ObservableObject
{
    private var cache: [String: GraphicsContext.ResolvedText] = [:]

    func resolve(_ string: String, font: Font, color: Color = .primary,
                 in context: GraphicsContext) -> GraphicsContext.ResolvedText
    {
        let key = "\(string)|\(font)|\(color)"
        if let cached = cache[key] { return cached }
        let resolved = context.resolve(
            Text(string).font(font).foregroundColor(color)
        )
        cache[key] = resolved
        return resolved
    }

    func clear()
    {
        cache.removeAll()
    }
}

// MARK: - Plugin type colors

private func pluginTypeColor(_ type: String) -> Color
{
    switch type
    {
    case "VST3": return Color(red: 0.35, green: 0.55, blue: 0.95)
    case "VST2": return Color(red: 0.3, green: 0.7, blue: 0.8)
    case "CLAP": return Color(red: 0.65, green: 0.4, blue: 0.9)
    case "AUV2": return Color(red: 0.9, green: 0.6, blue: 0.25)
    case "LV2":  return Color(red: 0.35, green: 0.75, blue: 0.45)
    case "CMAJ": return Color(red: 0.9, green: 0.45, blue: 0.6)
    case "JSFX": return Color(red: 0.8, green: 0.7, blue: 0.3)
    case "INT":  return Color(red: 0.55, green: 0.55, blue: 0.55)
    default:     return .secondary
    }
}

// MARK: - Visual Graph View

struct VisualGraphView: View
{
    @ObservedObject var model: NodeGraphModel
    @StateObject private var textCache = TextCache()

    var body: some View
    {
        let layout = computeLayout(model: model)

        ScrollView([.horizontal, .vertical])
        {
            ZStack(alignment: .topLeading)
            {
                Canvas { context, _ in
                    drawTrackBackgrounds(context: &context, layout: layout)
                    drawWires(context: &context, layout: layout)
                    drawEndpoints(context: &context, layout: layout)
                    drawProcessorNodes(context: &context, layout: layout)
                    drawTrackLabels(context: &context, layout: layout)
                    drawMixerBackgrounds(context: &context, layout: layout)
                }
                .frame(width: layout.totalSize.width, height: layout.totalSize.height)
                .onTapGesture { location in
                    handleTap(at: location, layout: layout)
                }

                ForEach(layout.tracks, id: \.trackId) { track in
                    MixerOverlay(
                        trackId: track.trackId,
                        hasPan: track.hasPan,
                        model: model
                    )
                    .frame(width: track.mixerRect.width, height: track.mixerRect.height)
                    .offset(x: track.mixerRect.minX, y: track.mixerRect.minY)
                }
            }
            .frame(width: layout.totalSize.width, height: layout.totalSize.height)
            .coordinateSpace(name: "canvas")
        }
        .onChange(of: model.tracks.map({ $0.id })) {
            textCache.clear()
        }
    }

    // MARK: Drawing passes

    private func drawTrackBackgrounds(context: inout GraphicsContext, layout: GraphLayout)
    {
        for track in layout.tracks
        {
            let color = Color(
                hue: track.hueOffset,
                saturation: 0.15,
                brightness: 0.35
            )
            let path = RoundedRectangle(cornerRadius: Layout.cornerRadius)
                .path(in: track.boundingBox)
            context.fill(path, with: .color(color.opacity(0.5)))
            context.stroke(path, with: .color(color.opacity(0.8)), lineWidth: 1)
        }
    }

    private func drawWires(context: inout GraphicsContext, layout: GraphLayout)
    {
        let wireColor = Color(white: 0.55)

        for track in layout.tracks
        {
            // Audio chain wires between processors
            for i in 0..<max(0, track.nodes.count - 1)
            {
                let from = track.nodes[i].rect
                let to = track.nodes[i + 1].rect
                drawAudioWire(context: &context, from: from, to: to, color: wireColor)
            }

            // Audio In endpoint -> first processor
            if let ep = track.audioInEndpoint
            {
                let target = track.nodes.first?.rect ?? track.boundingBox
                drawAudioWire(context: &context, from: ep.rect, to: target, color: wireColor)
            }

            // Last processor -> Audio Out endpoint
            if let ep = track.audioOutEndpoint
            {
                let source = track.nodes.last?.rect ?? track.boundingBox
                drawAudioWire(context: &context, from: source, to: ep.rect, color: wireColor)
            }

            // MIDI In endpoints -> track box
            for ep in track.midiInEndpoints
            {
                drawMidiWire(context: &context, from: ep.rect, to: track.boundingBox)
            }
        }
    }

    private func drawAudioWire(context: inout GraphicsContext, from: CGRect, to: CGRect,
                                color: Color)
    {
        let startPt = CGPoint(x: from.maxX, y: from.midY)
        let endPt = CGPoint(x: to.minX, y: to.midY)
        let dx = (endPt.x - startPt.x) * 0.4

        // Wire
        var path = Path()
        path.move(to: startPt)
        path.addCurve(
            to: endPt,
            control1: CGPoint(x: startPt.x + dx, y: startPt.y),
            control2: CGPoint(x: endPt.x - dx, y: endPt.y)
        )
        context.stroke(path, with: .color(color), lineWidth: Layout.wireStrokeWidth)

        // Port dots
        let r = Layout.portDotRadius
        let startDot = Circle().path(in: CGRect(
            x: startPt.x - r, y: startPt.y - r, width: r * 2, height: r * 2
        ))
        let endDot = Circle().path(in: CGRect(
            x: endPt.x - r, y: endPt.y - r, width: r * 2, height: r * 2
        ))
        context.fill(startDot, with: .color(color))
        context.fill(endDot, with: .color(color))
    }

    private func drawMidiWire(context: inout GraphicsContext, from: CGRect, to: CGRect)
    {
        let midiColor = Color(red: 0.95, green: 0.7, blue: 0.2)
        let startPt = CGPoint(x: from.midX, y: from.maxY)
        let endPt = CGPoint(x: to.minX + Layout.trackPadding, y: to.minY)

        var path = Path()
        path.move(to: startPt)
        path.addCurve(
            to: endPt,
            control1: CGPoint(x: startPt.x, y: startPt.y + (endPt.y - startPt.y) * 0.6),
            control2: CGPoint(x: endPt.x, y: endPt.y - (endPt.y - startPt.y) * 0.3)
        )
        context.stroke(
            path,
            with: .color(midiColor),
            style: StrokeStyle(lineWidth: Layout.wireStrokeWidth, dash: Layout.midiDash)
        )

        // Port dots
        let r = Layout.portDotRadius
        let startDot = Circle().path(in: CGRect(
            x: startPt.x - r, y: startPt.y - r, width: r * 2, height: r * 2
        ))
        let endDot = Circle().path(in: CGRect(
            x: endPt.x - r, y: endPt.y - r, width: r * 2, height: r * 2
        ))
        context.fill(startDot, with: .color(midiColor))
        context.fill(endDot, with: .color(midiColor))
    }

    private func drawEndpoints(context: inout GraphicsContext, layout: GraphLayout)
    {
        let epFont = Font.system(size: 10, weight: .medium)
        let midiColor = Color(red: 0.95, green: 0.7, blue: 0.2)
        let audioColor = Color(white: 0.55)

        for track in layout.tracks
        {
            let endpoints: [EndpointRect] = [track.audioInEndpoint, track.audioOutEndpoint]
                .compactMap { $0 } + track.midiInEndpoints

            for ep in endpoints
            {
                let color = ep.isMidi ? midiColor : audioColor
                let bgColor = ep.isMidi
                    ? Color(red: 0.25, green: 0.2, blue: 0.1)
                    : Color(white: 0.2)

                let path = RoundedRectangle(cornerRadius: Layout.endpointCornerRadius)
                    .path(in: ep.rect)
                context.fill(path, with: .color(bgColor))
                context.stroke(path, with: .color(color.opacity(0.6)), lineWidth: 1)

                let textColor: Color = ep.isMidi
                    ? Color(red: 0.95, green: 0.8, blue: 0.4)
                    : Color(white: 0.7)
                let resolved = textCache.resolve(ep.label, font: epFont, color: textColor, in: context)
                context.draw(resolved, at: CGPoint(x: ep.rect.midX, y: ep.rect.midY), anchor: .center)
            }
        }
    }

    private func drawProcessorNodes(context: inout GraphicsContext, layout: GraphLayout)
    {
        let nameFont = Font.system(size: 11, weight: .medium)
        let badgeFont = Font.system(size: 9, weight: .bold)

        for track in layout.tracks
        {
            for node in track.nodes
            {
                let typeColor = pluginTypeColor(node.pluginType)

                // Shadow
                var shadowCtx = context
                shadowCtx.addFilter(.shadow(color: .black.opacity(0.4), radius: 4, x: 0, y: 2))
                let shadowPath = RoundedRectangle(cornerRadius: Layout.cornerRadius)
                    .path(in: node.rect)
                shadowCtx.fill(shadowPath, with: .color(.clear))

                // Node background
                let nodeBg = Color(white: 0.18)
                let nodePath = RoundedRectangle(cornerRadius: Layout.cornerRadius)
                    .path(in: node.rect)
                context.fill(nodePath, with: .color(nodeBg))
                context.stroke(nodePath, with: .color(Color(white: 0.3)), lineWidth: 1)

                // Title bar (top rounded corners only)
                let titleRect = CGRect(
                    x: node.rect.minX,
                    y: node.rect.minY,
                    width: node.rect.width,
                    height: Layout.titleBarHeight
                )
                var titleClip = Path()
                titleClip.addRoundedRect(
                    in: CGRect(x: node.rect.minX, y: node.rect.minY,
                               width: node.rect.width, height: Layout.cornerRadius * 2),
                    cornerSize: CGSize(width: Layout.cornerRadius, height: Layout.cornerRadius)
                )
                titleClip.addRect(CGRect(
                    x: node.rect.minX,
                    y: node.rect.minY + Layout.cornerRadius,
                    width: node.rect.width,
                    height: Layout.titleBarHeight - Layout.cornerRadius
                ))
                context.fill(titleClip, with: .color(typeColor.opacity(0.3)))

                // Separator line below title
                var sepPath = Path()
                sepPath.move(to: CGPoint(x: node.rect.minX, y: titleRect.maxY))
                sepPath.addLine(to: CGPoint(x: node.rect.maxX, y: titleRect.maxY))
                context.stroke(sepPath, with: .color(Color(white: 0.25)), lineWidth: 0.5)

                // Plugin type badge
                if !node.pluginType.isEmpty
                {
                    let badge = textCache.resolve(
                        node.pluginType, font: badgeFont,
                        color: typeColor, in: context
                    )
                    context.draw(
                        badge,
                        at: CGPoint(x: node.rect.maxX - 8, y: titleRect.midY),
                        anchor: .trailing
                    )
                }

                // Editor indicator dot
                if node.hasEditor
                {
                    let dotRadius: CGFloat = 3
                    let dotCenter = CGPoint(
                        x: node.rect.minX + 10,
                        y: titleRect.midY
                    )
                    let dotPath = Circle().path(in: CGRect(
                        x: dotCenter.x - dotRadius,
                        y: dotCenter.y - dotRadius,
                        width: dotRadius * 2,
                        height: dotRadius * 2
                    ))
                    context.fill(dotPath, with: .color(typeColor))
                }

                // Processor name
                let nameText = textCache.resolve(
                    node.name, font: nameFont,
                    color: Color(white: 0.9), in: context
                )
                let nameY = node.rect.minY + Layout.titleBarHeight +
                    (node.rect.height - Layout.titleBarHeight) / 2
                context.draw(
                    nameText,
                    at: CGPoint(x: node.rect.midX, y: nameY),
                    anchor: .center
                )
            }
        }
    }

    private func drawTrackLabels(context: inout GraphicsContext, layout: GraphLayout)
    {
        let labelFont = Font.system(size: 11, weight: .semibold)
        for track in layout.tracks
        {
            let label = textCache.resolve(
                track.name, font: labelFont,
                color: Color(white: 0.75), in: context
            )
            context.draw(
                label,
                at: CGPoint(x: track.boundingBox.minX + 8, y: track.boundingBox.minY + 5),
                anchor: .topLeading
            )
        }
    }

    private func drawMixerBackgrounds(context: inout GraphicsContext, layout: GraphLayout)
    {
        for track in layout.tracks
        {
            let bg = Color(white: 0.14)
            let border = Color(white: 0.28)
            let path = RoundedRectangle(cornerRadius: Layout.cornerRadius)
                .path(in: track.mixerRect)
            context.fill(path, with: .color(bg))
            context.stroke(path, with: .color(border), lineWidth: 1)
        }
    }

    // MARK: Hit testing

    private func handleTap(at point: CGPoint, layout: GraphLayout)
    {
        for track in layout.tracks
        {
            for node in track.nodes
            {
                if node.hasEditor && node.rect.contains(point)
                {
                    model.toggleEditor(processorId: node.id)
                    return
                }
            }
        }
    }
}

// MARK: - Mixer overlay (SwiftUI controls over canvas)

private struct MixerOverlay: View
{
    let trackId: Int
    let hasPan: Bool
    @ObservedObject var model: NodeGraphModel
    @State private var gainDb: Float = 0
    @State private var pan: Float = 0

    var body: some View
    {
        VStack(spacing: 2)
        {
            HStack(spacing: 4)
            {
                Text(gainLabel)
                    .font(.system(size: 9).monospacedDigit())
                    .foregroundColor(Color(white: 0.6))
                    .frame(width: 40, alignment: .trailing)
                Slider(value: $gainDb, in: -120...24, step: 0.1)
                    .controlSize(.mini)
                    .onChange(of: gainDb) { _, newValue in
                        model.setGain(newValue, forTrackId: trackId)
                    }
            }

            if hasPan
            {
                HStack(spacing: 4)
                {
                    Text(panLabel)
                        .font(.system(size: 9).monospacedDigit())
                        .foregroundColor(Color(white: 0.6))
                        .frame(width: 40, alignment: .trailing)
                    Slider(value: $pan, in: -1...1, step: 0.01)
                        .controlSize(.mini)
                        .onChange(of: pan) { _, newValue in
                            model.setPan(newValue, forTrackId: trackId)
                        }
                }
            }
        }
        .padding(.horizontal, 6)
        .padding(.vertical, 4)
        .onAppear { syncFromModel() }
        .onChange(of: model.tracks) { syncFromModel() }
    }

    private func syncFromModel()
    {
        if let track = model.tracks.first(where: { $0.id == trackId })
        {
            gainDb = track.gain
            pan = track.pan
        }
    }

    private var gainLabel: String
    {
        if gainDb <= -120 { return "-inf dB" }
        return String(format: "%.1f dB", gainDb)
    }

    private var panLabel: String
    {
        if abs(pan) < 0.01 { return "C" }
        if pan < 0 { return String(format: "L%.0f", abs(pan) * 100) }
        return String(format: "R%.0f", pan * 100)
    }
}
