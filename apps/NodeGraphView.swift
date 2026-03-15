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

private enum GraphTab: String, CaseIterable
{
    case list = "List"
    case graph = "Graph"
}

struct NodeGraphView: View
{
    @ObservedObject var model: NodeGraphModel
    @State private var selectedTab: GraphTab = .list

    var body: some View
    {
        Group
        {
            switch selectedTab
            {
            case .list:
                listView
            case .graph:
                VisualGraphView(model: model)
            }
        }
        .frame(minWidth: 500, minHeight: 400)
        .toolbar {
            ToolbarItem(placement: .principal)
            {
                Picker("View", selection: $selectedTab)
                {
                    Label("List", systemImage: "list.bullet")
                        .tag(GraphTab.list)
                    Label("Graph", systemImage: "point.3.connected.trianglepath.dotted")
                        .tag(GraphTab.graph)
                }
                .pickerStyle(.segmented)
                .fixedSize()
            }
        }
    }

    private var listView: some View
    {
        List
        {
            if model.tracks.isEmpty
            {
                Text("No tracks")
                    .foregroundColor(.secondary)
            }
            else
            {
                ForEach(model.tracks) { track in
                    TrackRow(track: track, model: model)
                }
            }
        }
    }
}

private struct TrackRow: View
{
    let track: TrackViewModel
    let model: NodeGraphModel
    @State private var isExpanded = true

    var body: some View
    {
        DisclosureGroup(isExpanded: $isExpanded)
        {
            TrackMixerControls(track: track, model: model)
            ForEach(track.processors) { proc in
                ProcessorRow(proc: proc, model: model)
            }
        } label: {
            HStack
            {
                Image(systemName: "waveform")
                Text(track.name)
                    .fontWeight(.medium)
                Spacer()
                Text("\(track.channels)ch")
                    .font(.caption)
                    .foregroundColor(.secondary)
            }
        }
    }
}

private struct TrackMixerControls: View
{
    let track: TrackViewModel
    let model: NodeGraphModel
    @State private var gainDb: Float
    @State private var pan: Float

    init(track: TrackViewModel, model: NodeGraphModel)
    {
        self.track = track
        self.model = model
        _gainDb = State(initialValue: track.gain)
        _pan = State(initialValue: track.pan)
    }

    var body: some View
    {
        VStack(spacing: 6)
        {
            HStack(spacing: 8)
            {
                Image(systemName: "speaker.wave.2")
                    .foregroundColor(.secondary)
                    .frame(width: 16)
                Text(gainLabel)
                    .font(.caption.monospacedDigit())
                    .foregroundColor(.secondary)
                    .frame(width: 52, alignment: .trailing)
                Slider(value: $gainDb, in: -120...24, step: 0.1)
                {
                    Text("Gain")
                }
                .onChange(of: gainDb) { _, newValue in
                    model.setGain(newValue, forTrackId: track.id)
                }
            }

            if track.hasPan
            {
                HStack(spacing: 8)
                {
                    Image(systemName: "arrow.left.and.right")
                        .foregroundColor(.secondary)
                        .frame(width: 16)
                    Text(panLabel)
                        .font(.caption.monospacedDigit())
                        .foregroundColor(.secondary)
                        .frame(width: 52, alignment: .trailing)
                    Slider(value: $pan, in: -1...1, step: 0.01)
                    {
                        Text("Pan")
                    }
                    .onChange(of: pan) { _, newValue in
                        model.setPan(newValue, forTrackId: track.id)
                    }
                }
            }
        }
        .padding(.vertical, 2)
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

private struct ProcessorRow: View
{
    let proc: ProcessorViewModel
    let model: NodeGraphModel

    var body: some View
    {
        HStack
        {
            Text(proc.name)
            Spacer()
            if !proc.pluginType.isEmpty
            {
                Text(proc.pluginType)
                    .font(.caption2.weight(.medium))
                    .padding(.horizontal, 6)
                    .padding(.vertical, 2)
                    .background(pluginTypeColor(proc.pluginType).opacity(0.2))
                    .foregroundColor(pluginTypeColor(proc.pluginType))
                    .clipShape(Capsule())
            }
            if proc.hasEditor
            {
                Button(action: { model.toggleEditor(processorId: proc.id) })
                {
                    Image(systemName: "macwindow")
                        .foregroundColor(.secondary)
                }
                .buttonStyle(.borderless)
                .help("Open/Close Editor")
            }
        }
    }

    private func pluginTypeColor(_ type: String) -> Color
    {
        switch type
        {
        case "VST3": return .blue
        case "VST2": return .cyan
        case "CLAP": return .purple
        case "AUV2": return .orange
        case "LV2":  return .green
        case "CMAJ": return .pink
        case "INT":  return .gray
        default:     return .secondary
        }
    }
}
