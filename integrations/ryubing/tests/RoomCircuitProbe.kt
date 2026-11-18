// SPDX-License-Identifier: MIT
package org.kenjinx.android.views

fun main(args: Array<String>) {
    check(RoomCircuitApi.parseOrigin("https://rooms.example.net") != null)
    check(RoomCircuitApi.parseOrigin("http://192.168.0.1:11464") != null)
    check(RoomCircuitApi.parseOrigin("http://rooms.example.net") == null)
    check(RoomCircuitApi.parseOrigin("https://member@rooms.example.net/") == null)
    check(RoomCircuitApi.parseOrigin("https://rooms.example.net/appointments/") == null)
    val identity = RoomCircuitApi.newIdentity()
    check(RoomCircuitApi.participant.matches(identity))
    val origin = requireNotNull(RoomCircuitApi.parseOrigin(args.single()))
    val api = RoomCircuitApi(origin, identity)
    val options = api.options()
    check(options.getInt("nativeApi") == 1)
    val game = options.getJSONArray("games").getJSONObject(0)
    val page = api.request("list")
    val id = api.request("create", "starts" to (page.getLong("now") + 600), "game" to game.getString("game"),
        "app" to game.getString("app")).getJSONObject("created").getString("id")
    val event = api.request("event", "id" to id).getJSONArray("rows").getJSONObject(0)
    check(event.getString("id") == id)
    check(api.request("ready", "id" to id).getJSONArray("rows").getJSONObject(0).getBoolean("ready"))
    check(api.request("rooms", "id" to id).has("rooms"))
    check(!api.request("leave", "id" to id).getJSONArray("rows").getJSONObject(0).getBoolean("ready"))
    println("Kenji Android HTTP client: origin, identity, options, create, exact event, ready, rooms, and clear passed.")
}
