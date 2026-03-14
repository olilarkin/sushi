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

import Foundation

struct ProcessorViewModel: Identifiable, Equatable
{
    let id: Int
    let name: String
    let pluginType: String
    let hasEditor: Bool
}

struct TrackViewModel: Identifiable, Equatable
{
    let id: Int
    let name: String
    let channels: Int
    let processors: [ProcessorViewModel]
    var gain: Float    // dB, -120..24
    var pan: Float     // -1..1
    let hasPan: Bool
}

struct AudioConnectionVM: Hashable
{
    let trackId: Int
    let trackChannel: Int
    let engineChannel: Int
}

struct MidiKbdConnectionVM: Hashable
{
    let trackId: Int
    let channel: Int   // 0-15 or 16=OMNI
    let port: Int
    let rawMidi: Bool
}

class NodeGraphModel: NSObject, ObservableObject, SushiGraphChangeListener
{
    @Published var tracks: [TrackViewModel] = []
    @Published var audioInputs: [AudioConnectionVM] = []
    @Published var audioOutputs: [AudioConnectionVM] = []
    @Published var midiKbdInputs: [MidiKbdConnectionVM] = []

    private let bridge: SushiGraphBridge

    init(bridge: SushiGraphBridge)
    {
        self.bridge = bridge
        super.init()
        bridge.add(self)
        reload()
    }

    func graphDidChange()
    {
        reload()
    }

    func toggleEditor(processorId: Int)
    {
        bridge.toggleEditor(forProcessor: Int32(processorId))
    }

    func setGain(_ gainDb: Float, forTrackId trackId: Int)
    {
        bridge.setGain(gainDb, forTrack: Int32(trackId))
        if let idx = tracks.firstIndex(where: { $0.id == trackId })
        {
            tracks[idx].gain = gainDb
        }
    }

    func setPan(_ pan: Float, forTrackId trackId: Int)
    {
        bridge.setPan(pan, forTrack: Int32(trackId))
        if let idx = tracks.firstIndex(where: { $0.id == trackId })
        {
            tracks[idx].pan = pan
        }
    }

    func reload()
    {
        guard let objcTracks = bridge.allTracks() else
        {
            tracks = []
            audioInputs = []
            audioOutputs = []
            midiKbdInputs = []
            return
        }

        tracks = objcTracks.map { track in
            let procs = (track.processors ?? []).map { proc in
                ProcessorViewModel(
                    id: Int(proc.processorId),
                    name: proc.name ?? "",
                    pluginType: proc.pluginType ?? "",
                    hasEditor: proc.hasEditor
                )
            }
            return TrackViewModel(
                id: Int(track.trackId),
                name: track.name ?? "",
                channels: Int(track.channels),
                processors: procs,
                gain: track.gain,
                pan: track.pan,
                hasPan: track.hasPan
            )
        }

        audioInputs = (bridge.allAudioInputConnections() ?? []).map { c in
            AudioConnectionVM(
                trackId: Int(c.trackId),
                trackChannel: Int(c.trackChannel),
                engineChannel: Int(c.engineChannel)
            )
        }

        audioOutputs = (bridge.allAudioOutputConnections() ?? []).map { c in
            AudioConnectionVM(
                trackId: Int(c.trackId),
                trackChannel: Int(c.trackChannel),
                engineChannel: Int(c.engineChannel)
            )
        }

        midiKbdInputs = (bridge.allMidiKbdInputConnections() ?? []).map { c in
            MidiKbdConnectionVM(
                trackId: Int(c.trackId),
                channel: Int(c.channel),
                port: Int(c.port),
                rawMidi: c.rawMidi
            )
        }
    }
}
