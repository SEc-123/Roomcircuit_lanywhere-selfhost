# Shared appointments

Open the relay's `/appointments/` page on a phone or computer. The page and the Switch **Appointments** screen use the same schedule and ready counts.

1. Choose **New appointment**, pick a game and set the local start time. For another game, enter its verified local communication ID (LCID). An application ID alone does not identify its local wireless group.
2. Select the appointment and use **Share appointment** to send its link. Follow it to check for open rooms while the panel is running.
3. Select **I'm ready** from 30 minutes before the start. Each ready action counts the device for five minutes. Cancel ready to clear it immediately, or mark ready again after it expires. The browser's followed appointments belong to that browser.
4. Use **Open rooms** to see the host and seats. **Connect a device** shows the relevant address, port and emulator settings. Open the matching game, choose **Local Wireless** and join it there.

The panel shows public, joinable rooms on the same relay. It identifies rooms explicitly bound to the appointment and labels matches based on LCID when the title has not been confirmed. Private and full rooms are omitted. Browser notifications require a supported secure context and permission, and the page needs to stay running. Switch alerts still use Ultrahand.

## Local preview

Run the [crossplay test relay](CROSSPLAY.md) as usual. Its launcher prints the panel URL on TCP 11464. With the relay host and devices on the same Wi-Fi, open that URL on each browser. Point the Switch test build at the same relay on 11470. The launcher uses a temporary state directory.

## Self-hosted HTTPS

Add the settings below to the relay environment. Replace the example hostnames with your own. The web listener defaults to loopback. Leave the existing game listeners on the ports your players use.

```ini
ROOMCIRCUIT_WEB_PORT=11464
ROOMCIRCUIT_WEB_BIND=127.0.0.1
ROOMCIRCUIT_WEB_ORIGIN=https://rooms.example.net
ROOMCIRCUIT_WEB_RELAY_HOST=relay.example.net
```

In the HTTPS virtual host, point the page and its API to the same loopback listener. Preserve the public Host and overwrite the client IP header at the proxy.

```nginx
location /appointments {
    proxy_set_header Host $http_host;
    proxy_set_header X-Real-IP $remote_addr;
    proxy_pass http://127.0.0.1:11464;
}
location /api/appointments {
    proxy_set_header Host $http_host;
    proxy_set_header X-Real-IP $remote_addr;
    proxy_pass http://127.0.0.1:11464;
}
```

Keep the game's configured TCP/UDP ports reachable separately from HTTPS. The web service gives the browser the enabled connection options. The player still uses the emulator's own Local Wireless feature to join.

## Integrating an emulator menu

Link its RoomCircuit entry to the operator's `https://rooms.example.net/appointments/`. A shared appointment link uses `/appointments/?event=<id>`. Consider an optional server preset that follows the relevant wire profile in [Crossplay](CROSSPLAY.md). The web panel can ship and serve all clients independently of emulator release schedules.

## Verification

From a source checkout with Node 24:

```sh
node --test server/relay/test-appointments.js server/relay/test-appointments-e2e.js \
  server/relay/test-appointments-http.js server/relay/test-appointments-web.js
```

The combined test launches the relay, publishes appointments using both browser HTTP and Switch wire, verifies shared ready counts, then advertises rooms from both a Switch-protocol fixture and a RyuLDN client.
