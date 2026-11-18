// SPDX-License-Identifier: MIT
import Foundation

@main
struct RoomCircuitProbe {
    static func main() async throws {
        precondition(CommandLine.arguments.count == 2, "usage: probe <isolated relay origin>")
        let origin = try RoomCircuitService.parseOrigin(CommandLine.arguments[1])
        let canonical = try RoomCircuitService.parseOrigin("HTTPS://ROOMS.EXAMPLE.NET:443/")
        precondition(canonical.absoluteString == "https://rooms.example.net")
        for bad in ["http://rooms.example.net", "https://rooms.example.net/appointments/",
                    "https://user@rooms.example.net", "https://rooms.example.net?key=value"] {
            do {
                _ = try RoomCircuitService.parseOrigin(bad)
                fatalError("invalid origin accepted")
            } catch RoomCircuitFailure.origin {}
        }

        let defaultsName = "roomcircuit.probe." + UUID().uuidString
        let defaults = UserDefaults(suiteName: defaultsName)!
        defer { defaults.removePersistentDomain(forName: defaultsName) }
        let identity = RoomCircuitService.savedIdentity(for: origin, defaults: defaults)
        precondition(RoomCircuitService.hex(identity, count: 32))
        precondition(identity == RoomCircuitService.savedIdentity(for: origin, defaults: defaults))
        precondition(identity != RoomCircuitService.savedIdentity(for: canonical, defaults: defaults))
        let owner = RoomCircuitService(origin: origin, identity: identity)
        let observer = RoomCircuitService(origin: origin, identity: "b04daa04f14c572a00ca743e6101cf29")

        let options = try await owner.options()
        precondition(options.nativeApi == 1)
        precondition(options.readyBefore > 60 && options.readyLease > 0)
        precondition(options.endpoints.contains { $0.id == "lanplay" && $0.port > 0 })
        guard let game = options.games.first else { fatalError("no game options") }
        let start = Date().addingTimeInterval(60)
        let post = try await owner.create(start: start, game: game.game, app: game.app)
        guard let row = post.created else { fatalError("created appointment missing") }
        let list = try await observer.get("list")
        precondition(list.rows?.first(where: { $0.id == row.id })?.interested == 0)
        _ = try await observer.get("ready", id: row.id)
        let afterReady = try await owner.get("event", id: row.id)
        precondition(afterReady.rows?.first?.interested == 1)
        let selfState = try await observer.get("event", id: row.id)
        precondition(selfState.rows?.first?.ready == true)
        let rooms = try await owner.get("rooms", id: row.id)
        precondition(rooms.rooms?.isEmpty == true)
        _ = try await observer.get("leave", id: row.id)
        let afterLeave = try await owner.get("event", id: row.id)
        precondition(afterLeave.rows?.first?.interested == 0)
        print("Swift native client and actual relay: options, create, shared list, ready, event, rooms, leave passed.")
    }
}
